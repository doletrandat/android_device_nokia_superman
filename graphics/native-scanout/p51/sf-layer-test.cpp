// SPDX-License-Identifier: Apache-2.0
// Fresh-boot P67 only. No DRM node, gralloc lock, MMIO, power, or app release.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <binder/ProcessState.h>
#include <cutils/properties.h>
#include <gui/ISurfaceComposerClient.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <ui/DisplayConfig.h>
#include <ui/Fence.h>
#include <ui/Rect.h>
#include <utils/Timers.h>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <set>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <apex/window.h>
#include <android/gralloc_handle.h>

using namespace android;
static bool check_gl(const char* stage, int frame=-1, int quadrant=-1, int sample=-1) {
    GLenum error=glGetError();
    if(error==GL_NO_ERROR) return true;
    std::printf("NATIVE_SF_GL_ERROR stage=%s frame=%d quadrant=%d sample=%d error=0x%04x\n",stage,frame,quadrant,sample,error);
    for(unsigned extra=0;extra<7;++extra) {
        error=glGetError();if(error==GL_NO_ERROR) break;
        std::printf("NATIVE_SF_GL_ERROR stage=%s extra=%u error=0x%04x\n",stage,extra+1,error);
    }
    return false;
}

#include "render-matrix.h"
#include "buffer-observer.h"
struct Times { nsecs_t acquire=-2, latch=-2, gpu=-2, present=-2, release=-2; };
static bool times(const sp<Surface>& s, uint64_t frame, Times& t, bool release,
                  bool first=false) {
    const uint64_t begin=probe_ns(),budget=first?8000000000ULL:4000000000ULL;
    const unsigned polls=first?160:80;
    std::printf("NATIVE_SF_TIMESTAMP_BEGIN queue_frame=%" PRIu64 " release_required=%d client_target_GPU_required=true budget_ns=%" PRIu64 " begin_ns=%" PRIu64 "\n",frame,release,budget,begin);
    for (unsigned attempt=0; attempt<polls && probe_ns()-begin<budget; ++attempt) {
        status_t ret=s->getFrameTimestamps(frame, nullptr, &t.acquire, &t.latch,
                nullptr, nullptr, &t.gpu, &t.present, nullptr, release?&t.release:nullptr);
        if (ret!=NO_ERROR) { std::printf("NATIVE_SF timestamps_error frame=%" PRIu64 " status=%d\n",frame,ret); return false; }
        // This is SF GPU composition, not the producer's actual GPU acquire
        // fence. This client-target test requires both GPU fences to be real.
        const bool composition_done=t.gpu>0;
        if (t.acquire>0 && t.latch>0 && composition_done && t.present>0 && (!release || t.release>0)) {
            std::printf("NATIVE_SF_TIMESTAMP_READY queue_frame=%" PRIu64 " polls=%u wait_ns=%" PRIu64 " producer_GPU_acquire_ns=%" PRId64 " SF_GPU_composition_ns=%" PRId64 " composition_GPU=%s display_present_ns=%" PRId64 " release_ns=%" PRId64 "\n",frame,attempt+1,probe_ns()-begin,t.acquire,t.gpu,"fenced",t.present,t.release);
            return true;
        }
        if(attempt==0 || attempt%20==19)
            std::printf("NATIVE_SF_TIMESTAMP_PENDING queue_frame=%" PRIu64 " polls=%u wait_ns=%" PRIu64 " acquire=%" PRId64 " latch=%" PRId64 " composition_GPU=%" PRId64 " present=%" PRId64 " release=%" PRId64 "\n",frame,attempt+1,probe_ns()-begin,t.acquire,t.latch,t.gpu,t.present,t.release);
        usleep(50000);
    }
    std::printf("NATIVE_SF timestamps_timeout frame=%" PRIu64 " acquire=%" PRId64 " latch=%" PRId64 " gpu=%" PRId64 " present=%" PRId64 " release=%" PRId64 " wait_ns=%" PRIu64 "\n",frame,t.acquire,t.latch,t.gpu,t.present,t.release,probe_ns()-begin);
    return false;
}
#include "overlay-probe.h"
int main(int argc, char**) {
    setvbuf(stdout,nullptr,_IONBF,0);
    char value[PROPERTY_VALUE_MAX]{};
    if (argc!=1 || property_get("ro.superman.native_sf_layer_candidate",value,"")<=0 || std::strcmp(value,"p69")) return 64;
    property_get("vendor.superman.native_result",value,""); if(std::strcmp(value,"hwc-pass")) return 65;
    property_get("ro.hardware.egl",value,""); if(std::strcmp(value,"mesa")) return 65;
    property_get("ro.superman.native_sf_client_target",value,""); if(std::strcmp(value,"p69")) return 66;
    property_get("vendor.superman.native_sf_result",value,""); if(std::strcmp(value,"started")) return 66;
    ProcessState::self()->startThreadPool();
    sp<SurfaceComposerClient> client=new SurfaceComposerClient;
    if(client->initCheck()!=NO_ERROR) return 1;
    const auto display=SurfaceComposerClient::getInternalDisplayToken();
    DisplayConfig mode;
    if(!display || SurfaceComposerClient::getActiveDisplayConfig(display,&mode)!=NO_ERROR || mode.resolution.getWidth()!=720 || mode.resolution.getHeight()!=1280) return 2;
    sp<SurfaceControl> sc=client->createSurface(String8("Superman-P67-six-frame-test"),720,1280,PIXEL_FORMAT_BGRA_8888,ISurfaceComposerClient::eOpaque);
    if(!sc || !sc->isValid()) return 3;
    sp<Surface> window=sc->getSurface(); if(!window || window->setDequeueTimeout(3000000000LL)!=NO_ERROR) return 4;
    window->enableFrameTimestamps(true);
    BufferAgreement agreement;
    if(ANativeWindow_setDequeueBufferInterceptor(window.get(),observe_dequeue,&agreement)!=NO_ERROR) return 19;
    EGLDisplay egl=EGL_NO_DISPLAY; EGLSurface surface=EGL_NO_SURFACE; EGLContext context=EGL_NO_CONTEXT;
    uint64_t frames[20]{}; unsigned completed=0, released=0, overlay_completed=0, overlay_released=0, overlay_cycles=0;
    OverlayProbe overlay; std::set<uint64_t> producer_buffers;
    const int result=[&]() -> int {
        egl=eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if(egl==EGL_NO_DISPLAY || !eglInitialize(egl,nullptr,nullptr)) return 5;
        EGLConfig configs[128]{},config=nullptr;EGLint count=0;
        if(!eglGetConfigs(egl,configs,128,&count) || count<1 || count>128) return 6;
        for(int i=0;i<count;++i) {
            EGLint visual=0,type=0,render=0;
            if(!eglGetConfigAttrib(egl,configs[i],EGL_NATIVE_VISUAL_ID,&visual) || !eglGetConfigAttrib(egl,configs[i],EGL_SURFACE_TYPE,&type) || !eglGetConfigAttrib(egl,configs[i],EGL_RENDERABLE_TYPE,&render)) return 6;
            if(visual==PIXEL_FORMAT_BGRA_8888 && (type&EGL_WINDOW_BIT) && (render&EGL_OPENGL_ES2_BIT)) {config=configs[i];break;}
        }
        const EGLint attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
        if(!config || !eglBindAPI(EGL_OPENGL_ES_API)) return 7;
        context=eglCreateContext(egl,config,EGL_NO_CONTEXT,attrs);
        surface=eglCreateWindowSurface(egl,config,window.get(),nullptr);
        if(context==EGL_NO_CONTEXT || surface==EGL_NO_SURFACE || !eglMakeCurrent(egl,surface,surface,context) || !eglSwapInterval(egl,1)) return 8;
        const char* renderer=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        std::printf("NATIVE_SF renderer=%s layer=Superman-P67-six-frame-test dimensions=720x1280 producer=GPU CPU_writes=false\n",renderer?renderer:"null");
        if(!renderer || !std::strstr(renderer,"FD305")) return 9;
        if(!check_gl("context_initial")) return 11;
        EGLint width=0,height=0,red=0,green=0,blue=0,alpha=0,visual=0;
        if(!eglQuerySurface(egl,surface,EGL_WIDTH,&width) || !eglQuerySurface(egl,surface,EGL_HEIGHT,&height) ||
           !eglGetConfigAttrib(egl,config,EGL_RED_SIZE,&red) || !eglGetConfigAttrib(egl,config,EGL_GREEN_SIZE,&green) ||
           !eglGetConfigAttrib(egl,config,EGL_BLUE_SIZE,&blue) || !eglGetConfigAttrib(egl,config,EGL_ALPHA_SIZE,&alpha) ||
           !eglGetConfigAttrib(egl,config,EGL_NATIVE_VISUAL_ID,&visual)) {
            std::printf("NATIVE_SF_EGL_ERROR stage=surface_config error=0x%04x\n",eglGetError());return 18;
        }
        GLint binding=0,read_format=0,read_type=0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING,&binding);if(!check_gl("framebuffer_binding")) return 11;
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT,&read_format);if(!check_gl("read_format")) return 11;
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE,&read_type);if(!check_gl("read_type")) return 11;
        const GLenum status=glCheckFramebufferStatus(GL_FRAMEBUFFER);if(!check_gl("framebuffer_status")) return 11;
        std::printf("NATIVE_SF_WINDOW width=%d height=%d visual=%d RGBA_bits=%d/%d/%d/%d framebuffer=%d status=0x%04x read_format=0x%04x read_type=0x%04x\n",width,height,visual,red,green,blue,alpha,binding,status,read_format,read_type);
        if(visual!=PIXEL_FORMAT_BGRA_8888 || !agreement.ready()) {
            std::printf("NATIVE_SF_FORMAT_FAILURE stage=before_matrix visual=%d dequeues=%u queue_allowed=false\n",visual,agreement.dequeues);return 20;
        }
        if(!probe_matrix()) return 11;
        if(SurfaceComposerClient::Transaction{}.setLayer(sc,100000).show(sc).apply(true)!=NO_ERROR) return 10;
        constexpr unsigned char colors[4][4]={{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}};
        glViewport(0,0,720,1280);if(!check_gl("viewport")) return 11;
        glEnable(GL_SCISSOR_TEST);if(!check_gl("scissor_enable")) return 11;
        for(unsigned frame=0;frame<20;++frame) {
            if(frame==6 || frame==13) {
                if(!overlay.open(client,egl,config,frame==6?0:1)) return 21;
            }
            if(frame==12 || frame==19) {
                if(overlay.completed!=6 || overlay.released!=5 || !overlay.close(egl)) return 23;
                overlay_completed+=6;overlay_released+=5;++overlay_cycles;
                std::printf("NATIVE_SF_OVERLAY_REMOVED base_frame=%u retain_base_layer=true\n",frame);
            }
            if(overlay.control && !overlay.queue(egl,context,frame)) return 22;
            if(!eglMakeCurrent(egl,surface,surface,context)) return 8;
            glViewport(0,0,720,1280);glEnable(GL_SCISSOR_TEST);

            const uint64_t draw_begin=probe_ns();
            std::printf("NATIVE_SF_DRAW_BEGIN frame=%u time_ns=%" PRIu64 "\n",frame,draw_begin);
            for(unsigned q=0;q<4;++q) {
                const auto& color=colors[(q+frame)%4];
                glScissor((q%2)*360,(q/2)*640,360,640);if(!check_gl("scissor",frame,q)) return 11;
                glClearColor(color[0]/255.0f,color[1]/255.0f,color[2]/255.0f,1.0f);if(!check_gl("clear_color",frame,q)) return 11;
                glClear(GL_COLOR_BUFFER_BIT);if(!check_gl("clear",frame,q)) return 11;
                for(unsigned sample=0;sample<3;++sample) {
                    const GLint x=(q%2)*360+90+sample*60,y=(q/2)*640+160+sample*80;
                    unsigned char pixel[4]{};glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    const bool read_ok=check_gl("read_pixels",frame,q,sample);
                    if(!read_ok || std::memcmp(pixel,color,4)) {
                        std::printf("NATIVE_SF_PIXEL_FAILURE frame=%u quadrant=%u sample=%u x=%d y=%d expected=%u/%u/%u/%u actual=%u/%u/%u/%u read_GL_ok=%d\n",frame,q,sample,x,y,color[0],color[1],color[2],color[3],pixel[0],pixel[1],pixel[2],pixel[3],read_ok);
                        return 11;
                    }
                }
            }
            std::printf("NATIVE_SF_DRAW_END frame=%u time_ns=%" PRIu64 " wall_ns=%" PRIu64 " samples=12 verified=true\n",frame,probe_ns(),probe_ns()-draw_begin);
            if(!check_gl("frame_checks",frame)) return 11;
            if(!agreement.ready()) {
                std::printf("NATIVE_SF_FORMAT_FAILURE stage=before_queue frame=%u dequeues=%u queue_allowed=false\n",frame,agreement.dequeues);return 20;
            }
            const auto queue_begin=probe_ns();
            frames[frame]=window->getNextFrameNumber();
            if(!eglSwapBuffers(egl,surface)) return 12;
            std::printf("NATIVE_SF_SWAP queue_frame=%" PRIu64 " begin_ns=%" PRIu64 " end_ns=%" PRIu64 "\n",frames[frame],queue_begin,probe_ns());
            sp<GraphicBuffer> buffer;sp<Fence> acquire;float transform[16]{};
            if(window->getLastQueuedBuffer(&buffer,&acquire,transform)!=NO_ERROR || !buffer || !acquire || acquire->get()==-1 || acquire->wait(1000)!=NO_ERROR) return 13;
            producer_buffers.insert(buffer->getId());if(producer_buffers.size()>3) return 14;
            std::printf("NATIVE_SF_QUEUE frame=%u queue_frame=%" PRIu64 " buffer_id=%" PRIu64 " distinct_producer_buffers=%zu GPU_pixels=12-pass acquire_fence=signaled CPU_writes=false\n",frame,frames[frame],buffer->getId(),producer_buffers.size());
            Times t;if(!times(window,frames[frame],t,false,frame==0)) return 15;
            std::printf("NATIVE_SF_PRESENT frame=%u queue_frame=%" PRIu64 " acquire_ns=%" PRId64 " latch_ns=%" PRId64 " SF_GPU_composition_ns=%" PRId64 " display_present_ns=%" PRId64 " actual_fence_timestamps=true\n",frame,frames[frame],t.acquire,t.latch,t.gpu,t.present);
            ++completed;
            if(frame) {
                Times previous;if(!times(window,frames[frame-1],previous,true)) return 16;
                ++released;std::printf("NATIVE_SF_RELEASE frame=%u release_ns=%" PRId64 " signaled=true\n",frame-1,previous.release);
            }
        }
        if(overlay_completed!=12 || overlay_released!=10 || overlay_cycles!=2) return 24;
        return 0;
    }();
    // Cleanup partial overlay resources on every failure while EGL is alive.
    const bool overlay_cleanup=overlay.control || overlay.window || overlay.surface!=EGL_NO_SURFACE;
    bool overlay_cleanup_ok=true;
    if(overlay_cleanup) {
        if(egl!=EGL_NO_DISPLAY) eglMakeCurrent(egl,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        overlay_cleanup_ok=overlay.close(egl);
    }
    // Request layer removal through SF; the P67 owning HWC restores its original
    // framebuffer before destroying the final HWC layer. Never force-stop it.
    const status_t removal=SurfaceComposerClient::Transaction{}.hide(sc).apply(true);
    if(egl!=EGL_NO_DISPLAY) {
        eglMakeCurrent(egl,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if(surface!=EGL_NO_SURFACE) eglDestroySurface(egl,surface);
        if(context!=EGL_NO_CONTEXT) eglDestroyContext(egl,context);
        eglTerminate(egl);
    }
    ANativeWindow_setDequeueBufferInterceptor(window.get(),nullptr,nullptr);
    window.clear();sc.clear();client->dispose();client.clear();
    std::printf("NATIVE_SF_TEST result=%d presents=%u prior_producer_releases=%u overlay_presents=%u overlay_prior_releases=%u overlay_cycles=%u overlay_cleanup_ok=%d final_producer_release=unobserved removal_status=%d original_restore=must_verify_owner UI_held=true throughput_claim=false\n",result,completed,released,overlay_completed,overlay_released,overlay_cycles,overlay_cleanup_ok,removal);
    return result?result:(removal==NO_ERROR && overlay_cleanup_ok?0:17);
}
