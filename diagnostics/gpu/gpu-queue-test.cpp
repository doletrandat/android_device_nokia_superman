// SPDX-License-Identifier: Apache-2.0
// Real process-local Android BufferQueue + independent-context GPU composition.
// Explicit DRM attachments bypass the system allocator. Not Binder/SF/scanout.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gui/BufferQueue.h>
#include <gui/BufferItemConsumer.h>
#include <gui/BufferItem.h>
#include <gui/Surface.h>
#include <ui/GraphicBuffer.h>
#include <ui/Fence.h>
#include <hardware/gralloc.h>
#include <cutils/native_handle.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <array>
using namespace android;
static constexpr int width = 720, height = 1280, frames = 6, maxSourceBuffers = 3;
static int sourceBuffers = 2;
static constexpr int usage = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE |
                             GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;
static double now() { return std::chrono::duration<double,std::milli>(
    std::chrono::steady_clock::now().time_since_epoch()).count(); }
// A serial consumer can leave an allocated source buffer as a spare. Surface
// and BufferQueue do not promise that every preattached buffer will be chosen.
static bool validQueueReuse(const std::array<int,maxSourceBuffers>& seen,
                            int configuredSources, int completedFrames) {
    if ((configuredSources != 2 && configuredSources != 3) || completedFrames != frames) return false;
    int total = 0, used = 0;
    for (int i = 0; i < maxSourceBuffers; ++i) {
        if (seen[i] < 0 || (i >= configuredSources && seen[i] != 0)) return false;
        total += seen[i];
        if (seen[i] > 0) ++used;
    }
    return total == completedFrames && used >= 2;
}

static void stage(const char* s) { std::printf("QUEUE stage=%s monotonic_ms=%.3f\n", s, now()); }
struct Queue {
    gralloc_module_t* module = nullptr;
    alloc_device_t* allocator = nullptr;
    std::array<buffer_handle_t,maxSourceBuffers+1> owned{};
    std::array<native_handle_t*,maxSourceBuffers> clones{};
    std::array<sp<GraphicBuffer>,maxSourceBuffers+1> buffers{};
    std::array<sp<GraphicBuffer>,maxSourceBuffers> imported{};
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> endpoint;
    sp<BufferItemConsumer> consumer;
    sp<Surface> window;
    int initialize(gralloc_module_t* m) {
        module = m;
        hw_device_t* device = nullptr;
        if (module->common.methods->open(&module->common,GRALLOC_HARDWARE_GPU0,&device) || !device) return 1;
        allocator = reinterpret_cast<alloc_device_t*>(device);
        for (size_t i=0; i<=size_t(sourceBuffers); ++i) {
            int stride = 0;
            int error=allocator->alloc(allocator,width,height,HAL_PIXEL_FORMAT_BGRA_8888,usage,&owned[i],&stride);
            std::printf("QUEUE allocate index=%zu error=%d errno=%d stride=%d\n",i,error,errno,stride);
            if (error || !owned[i] || stride < width || stride > 8192) return 1;
            buffers[i] = new GraphicBuffer(owned[i],GraphicBuffer::WRAP_HANDLE,width,height,
                HAL_PIXEL_FORMAT_BGRA_8888,1,uint64_t(usage),stride);
            if (buffers[i]->initCheck()) return 1;
            if (i<size_t(sourceBuffers)) {
                clones[i] = native_handle_clone(owned[i]);
                if (!clones[i]) return 1;
                int error=module->registerBuffer(module,clones[i]);
                std::printf("QUEUE import index=%zu error=%d fd=%d original_fd=%d\n",i,error,clones[i]->data[0],owned[i]->data[0]);
                if (error) return 1;
                // Exercise genuine FD duplication/GBM import, without the global mapper.
                imported[i] = new GraphicBuffer(clones[i],GraphicBuffer::WRAP_HANDLE,width,height,
                    HAL_PIXEL_FORMAT_BGRA_8888,1,uint64_t(usage),stride);
                if (imported[i]->initCheck()) return 1;
            }
        }
        stage("allocator_contract_checks");
        native_handle_t tiny{}; tiny.version=sizeof(native_handle_t);
        if (module->registerBuffer(module,&tiny)!=-EINVAL) return 1;
        void* mapping=nullptr;
        if (module->lock(module,owned[0],GRALLOC_USAGE_SW_READ_OFTEN,0,0,width,height,&mapping) || !mapping) return 1;
        void* nested=nullptr;
        if (module->lock(module,owned[0],GRALLOC_USAGE_SW_READ_OFTEN,0,0,width,height,&nested)!=-EBUSY ||
            allocator->free(allocator,owned[0])!=-EBUSY ||
            module->unlock(module,owned[0]) || module->unlock(module,owned[0])!=-EINVAL) return 1;
        hw_device_t* other=nullptr;
        if (module->common.methods->open(&module->common,GRALLOC_HARDWARE_GPU0,&other) || !other ||
            other->close(other)) return 1;
        buffer_handle_t extra=nullptr;int extraStride=0;
        if (allocator->alloc(allocator,64,64,HAL_PIXEL_FORMAT_BGRA_8888,usage,&extra,&extraStride) ||
            !extra || allocator->free(allocator,extra)) return 1;
        for (auto* h : clones) if (h && fcntl(h->data[0],F_GETFD)<0) return 1;
        std::printf("QUEUE allocator_contracts=PASS malformed_header nested_lock locked_free double_unlock shared_allocator_close\n");
        BufferQueue::createBufferQueue(&producer,&endpoint);
        consumer = new BufferItemConsumer(endpoint,GRALLOC_USAGE_HW_TEXTURE,1,false);
        if (consumer->setDefaultBufferSize(width,height) ||
            consumer->setDefaultBufferFormat(HAL_PIXEL_FORMAT_BGRA_8888)) return 1;
        window = new Surface(producer,false);
        if (window->setDequeueTimeout(1000000000LL) ||
            native_window_api_connect(window.get(),NATIVE_WINDOW_API_EGL) ||
            native_window_set_buffers_dimensions(window.get(),width,height) ||
            native_window_set_buffers_format(window.get(),HAL_PIXEL_FORMAT_BGRA_8888) ||
            producer->allowAllocation(false)) return 1;
        // Seed the real GraphicBuffer slots; each attachment is cancelled to FREE.
        for (int i=0;i<sourceBuffers;++i) {
            const auto& buffer=imported[i];
            if (window->attachBuffer(buffer->getNativeBuffer()) ||
                static_cast<ANativeWindow*>(window.get())->cancelBuffer(window.get(),buffer->getNativeBuffer(),-1)) return 1;
        }
        std::printf("QUEUE initialized=PASS buffers=%d cloned_fd_imports=%d allocation_disabled=true\n",sourceBuffers,sourceBuffers);
        return 0;
    }
    int cleanup() {
        if (native_window_api_disconnect(window.get(),NATIVE_WINDOW_API_EGL)) return 1;
        consumer->abandon(); window.clear(); consumer.clear(); endpoint.clear(); producer.clear();
        for (auto& b : imported) b.clear();
        for (auto& b : buffers) b.clear();
        for (auto* h : clones) {
            if (!h) continue;
            if (module->unregisterBuffer(module,h)) return 1;
            native_handle_close(h); native_handle_delete(h);
        }
        for (auto h : owned) if (h && allocator->free(allocator,h)) return 1;
        return allocator->common.close(&allocator->common) ? 1 : 0;
    }
};
int main(int argc,char** argv) {
    if ((argc!=6 && argc!=7) || std::strcmp(argv[1],"--run-six-frames") ||
        std::strcmp(argv[2],"--mesa-library") || std::strcmp(argv[4],"--gralloc-library") ||
        std::strncmp(argv[3],"/data/local/tmp/",16) || std::strncmp(argv[5],"/data/local/tmp/",16) ||
        (argc==7 && std::strcmp(argv[6],"--three-source-buffers"))) {
        std::fprintf(stderr,"Usage: %s --run-six-frames --mesa-library /data/local/tmp/... --gralloc-library /data/local/tmp/...\n",argv[0]); return 2;
    }
    sourceBuffers=argc==7?3:2;
    std::setvbuf(stdout,nullptr,_IONBF,0); stage("libraries");
    void* hal=dlopen(argv[5],RTLD_NOW|RTLD_LOCAL); void* mesa=dlopen(argv[3],RTLD_NOW|RTLD_LOCAL);
    if (!hal || !mesa) { std::fprintf(stderr,"dlopen: %s\n",dlerror()); return 1; }
#define LOAD(n) auto n=reinterpret_cast<decltype(&::n)>(dlsym(mesa,#n)); if (!n) return 1;
    LOAD(eglGetProcAddress) LOAD(eglInitialize) LOAD(eglGetConfigs) LOAD(eglGetConfigAttrib)
    LOAD(eglBindAPI) LOAD(eglCreateContext) LOAD(eglCreateWindowSurface) LOAD(eglMakeCurrent)
    LOAD(eglSwapBuffers) LOAD(eglSwapInterval) LOAD(eglDestroySurface) LOAD(eglDestroyContext)
    LOAD(eglTerminate) LOAD(eglGetError) LOAD(eglQueryString)
#undef LOAD
#define GL(n) auto n=reinterpret_cast<decltype(&::n)>(eglGetProcAddress(#n)); if (!n) return 1;
    GL(glGetString) GL(glGetError) GL(glViewport) GL(glCreateShader) GL(glShaderSource)
    GL(glCompileShader) GL(glGetShaderiv) GL(glGetShaderInfoLog) GL(glDeleteShader)
    GL(glCreateProgram) GL(glAttachShader) GL(glLinkProgram) GL(glGetProgramiv) GL(glDeleteProgram)
    GL(glUseProgram) GL(glGetAttribLocation) GL(glGetUniformLocation) GL(glUniform1f) GL(glUniform1i)
    GL(glVertexAttribPointer) GL(glEnableVertexAttribArray) GL(glDisableVertexAttribArray)
    GL(glDrawArrays) GL(glGenTextures) GL(glBindTexture) GL(glTexParameteri) GL(glDeleteTextures)
    GL(glGenFramebuffers) GL(glBindFramebuffer) GL(glFramebufferTexture2D)
    GL(glCheckFramebufferStatus) GL(glDeleteFramebuffers) GL(glActiveTexture) GL(glFlush)
#undef GL
#define EXT(type,n) auto n=reinterpret_cast<type>(eglGetProcAddress(#n)); if (!n) return 1;
    EXT(PFNEGLGETPLATFORMDISPLAYEXTPROC,eglGetPlatformDisplayEXT)
    EXT(PFNEGLCREATEIMAGEKHRPROC,eglCreateImageKHR) EXT(PFNEGLDESTROYIMAGEKHRPROC,eglDestroyImageKHR)
    EXT(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC,glEGLImageTargetTexture2DOES)
    EXT(PFNEGLCREATESYNCKHRPROC,eglCreateSyncKHR) EXT(PFNEGLDESTROYSYNCKHRPROC,eglDestroySyncKHR)
    EXT(PFNEGLDUPNATIVEFENCEFDANDROIDPROC,eglDupNativeFenceFDANDROID)
#undef EXT
    auto* module=reinterpret_cast<gralloc_module_t*>(dlsym(hal,HAL_MODULE_INFO_SYM_AS_STR));
    if (!module || !module->registerBuffer || !module->unregisterBuffer || !module->lock || !module->unlock) return 1;
    Queue q; EGLDisplay display=EGL_NO_DISPLAY; EGLSurface surface=EGL_NO_SURFACE;
    EGLContext producerContext=EGL_NO_CONTEXT,consumerContext=EGL_NO_CONTEXT;
    std::array<EGLImageKHR,maxSourceBuffers+1> images{}; images.fill(EGL_NO_IMAGE_KHR);
    std::array<GLuint,maxSourceBuffers+1> textures{}; GLuint fbo=0,producerProgram=0,consumerProgram=0;
    std::array<GLuint,4> shaders{}; int completed=0; bool producerCurrent=false;
    BufferItem acquired; bool hasAcquired=false;
    const int result=[&]() -> int {
        stage("bufferqueue_initialize"); if (q.initialize(module)) return 1;
        display=eglGetPlatformDisplayEXT(EGL_PLATFORM_ANDROID_KHR,EGL_DEFAULT_DISPLAY,nullptr);
        if (display==EGL_NO_DISPLAY || !eglInitialize(display,nullptr,nullptr)) return 1;
        const char* extensions=eglQueryString(display,EGL_EXTENSIONS);
        if (!extensions || !std::strstr(extensions,"EGL_ANDROID_native_fence_sync") ||
            !std::strstr(extensions,"EGL_KHR_surfaceless_context")) return 1;
        EGLConfig configs[128]{},config=nullptr; EGLint count=0;
        if (!eglGetConfigs(display,configs,128,&count) || count<0 || count>128) return 1;
        for (int i=0;i<count;++i) {
            EGLint visual=0,type=0,render=0;
            if (!eglGetConfigAttrib(display,configs[i],EGL_NATIVE_VISUAL_ID,&visual) ||
                !eglGetConfigAttrib(display,configs[i],EGL_SURFACE_TYPE,&type) ||
                !eglGetConfigAttrib(display,configs[i],EGL_RENDERABLE_TYPE,&render)) return 1;
            if (visual==HAL_PIXEL_FORMAT_BGRA_8888 && (type&EGL_WINDOW_BIT) && (render&EGL_OPENGL_ES2_BIT)) {config=configs[i];break;}
        }
        if (!config || !eglBindAPI(EGL_OPENGL_ES_API)) return 1;
        const EGLint attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
        producerContext=eglCreateContext(display,config,EGL_NO_CONTEXT,attrs);
        consumerContext=eglCreateContext(display,config,EGL_NO_CONTEXT,attrs); // Independent GL objects/caches.
        surface=eglCreateWindowSurface(display,config,q.window.get(),nullptr);
        if (producerContext==EGL_NO_CONTEXT || consumerContext==EGL_NO_CONTEXT || surface==EGL_NO_SURFACE ||
            !eglMakeCurrent(display,surface,surface,producerContext) || !eglSwapInterval(display,0)) return 1;
        producerCurrent=true;
        const char* renderer=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        std::printf("QUEUE GL_RENDERER=%s dimensions=720x1280 independent_contexts=2\n",renderer?renderer:"null");
        if (!renderer || !std::strstr(renderer,"FD305")) return 1;
        auto program=[&](const char* frag,GLuint& out,GLuint& vs,GLuint& fs) -> bool {
            auto compile=[&](GLenum type,const char* src) {
                GLuint s=glCreateShader(type); glShaderSource(s,1,&src,nullptr); glCompileShader(s); GLint ok=0;
                glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
                if (!ok) {char log[1024]{};glGetShaderInfoLog(s,sizeof(log),nullptr,log);std::fprintf(stderr,"%s\n",log);glDeleteShader(s);return GLuint(0);} return s;
            };
            vs=compile(GL_VERTEX_SHADER,"attribute vec2 p; varying vec2 uv; void main(){uv=p*.5+.5;gl_Position=vec4(p,0.,1.);}");
            fs=compile(GL_FRAGMENT_SHADER,frag); if (!vs || !fs) return false;
            out=glCreateProgram();glAttachShader(out,vs);glAttachShader(out,fs);glLinkProgram(out);GLint ok=0;glGetProgramiv(out,GL_LINK_STATUS,&ok);return ok;
        };
        if (!program("precision highp float;uniform float frame;void main(){float q=step(360.,gl_FragCoord.x)+2.*step(640.,1280.-gl_FragCoord.y);q=mod(q+frame,4.);vec3 c=q<.5?vec3(1,0,0):q<1.5?vec3(0,1,0):q<2.5?vec3(0,0,1):vec3(1,1,1);gl_FragColor=vec4(c,1.);}",producerProgram,shaders[0],shaders[1])) return 1;
        if (!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,consumerContext)) return 1; producerCurrent=false;
        if (!program("precision highp float;varying vec2 uv;uniform sampler2D source;void main(){gl_FragColor=texture2D(source,uv);}",consumerProgram,shaders[2],shaders[3])) return 1;
        for (size_t i=0;i<=size_t(sourceBuffers);++i) {
            ANativeWindowBuffer* buffer=i<size_t(sourceBuffers)?q.imported[i]->getNativeBuffer():q.buffers[sourceBuffers]->getNativeBuffer();
            const EGLint imageAttrs[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
            images[i]=eglCreateImageKHR(display,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,buffer,imageAttrs);
            if (images[i]==EGL_NO_IMAGE_KHR) return 1;
            glGenTextures(1,&textures[i]);glBindTexture(GL_TEXTURE_2D,textures[i]);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,images[i]); if (glGetError()!=GL_NO_ERROR) return 1;
        }
        glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[sourceBuffers],0);
        if (!fbo || glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return 1;
        const GLfloat vertices[]={-1,-1,3,-1,-1,3};
        std::array<int,maxSourceBuffers> seen{};
        for (int frame=0;frame<frames;++frame) {
            stage("producer_draw");double begin=now();
            if (!eglMakeCurrent(display,surface,surface,producerContext)) return 1;producerCurrent=true;
            glUseProgram(producerProgram);GLint position=glGetAttribLocation(producerProgram,"p"),frameLoc=glGetUniformLocation(producerProgram,"frame");
            if (position<0 || frameLoc<0) return 1;
            glVertexAttribPointer(position,2,GL_FLOAT,GL_FALSE,0,vertices);glEnableVertexAttribArray(position);
            glViewport(0,0,width,height);glUniform1f(frameLoc,frame%4);glDrawArrays(GL_TRIANGLES,0,3);
            if (glGetError()!=GL_NO_ERROR || !eglSwapBuffers(display,surface)) return 1;
            double swapEnd=now();
            if (q.consumer->acquireBuffer(&acquired,0,false)) return 1;hasAcquired=true;
            if (!acquired.mFence || acquired.mFence->wait(5000)) return 1;
            int index=-1;for(int i=0;i<sourceBuffers;++i) if(acquired.mGraphicBuffer==q.imported[i]) index=i;
            std::printf("QUEUE acquired buffer=%d frame_number=%llu crop=%d,%d,%d,%d transform=%u fence_fd=%d\n",
                index,static_cast<unsigned long long>(acquired.mFrameNumber),acquired.mCrop.left,acquired.mCrop.top,
                acquired.mCrop.right,acquired.mCrop.bottom,acquired.mTransform,acquired.mFence->get());
            // Empty crop means use the full buffer, per BufferQueue/Surface defaults.
            if(index<0 || (!acquired.mCrop.isEmpty() && acquired.mCrop!=Rect(width,height)) || acquired.mTransform!=0) return 1;
            double acquireEnd=now();++seen[index];
            stage("consumer_compose");
            if(!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,consumerContext)) return 1;producerCurrent=false;
            glBindFramebuffer(GL_FRAMEBUFFER,fbo);glUseProgram(consumerProgram);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,textures[index]);
            GLint cp=glGetAttribLocation(consumerProgram,"p"),source=glGetUniformLocation(consumerProgram,"source");if(cp<0 || source<0) return 1;
            glVertexAttribPointer(cp,2,GL_FLOAT,GL_FALSE,0,vertices);glEnableVertexAttribArray(cp);glUniform1i(source,0);
            glViewport(0,0,width,height);glDrawArrays(GL_TRIANGLES,0,3);if(glGetError()!=GL_NO_ERROR) return 1;
            EGLSyncKHR sync=eglCreateSyncKHR(display,EGL_SYNC_NATIVE_FENCE_ANDROID,nullptr);if(sync==EGL_NO_SYNC_KHR) return 1;
            glFlush();int fd=eglDupNativeFenceFDANDROID(display,sync);eglDestroySyncKHR(display,sync);if(fd<0) return 1;
            sp<Fence> release=new Fence(fd);if(release->wait(5000)) return 1;
            double composeEnd=now();
            // Only sparse diagnostic pixels are read; no full-frame readback/copy.
            void* pixels=nullptr;if(module->lock(module,q.owned[sourceBuffers],GRALLOC_USAGE_SW_READ_OFTEN,0,0,width,height,&pixels)) return 1;
            int mismatch=0;
            const unsigned char colors[4][4]={{0,0,255,255},{0,255,0,255},{255,0,0,255},{255,255,255,255}};
            if(!pixels) mismatch=1;
            else for(int quadrant=0;quadrant<4;++quadrant) {
                int x=(quadrant%2)*width/2+width/4,y=(quadrant/2)*height/2+height/4;
                const auto* p=static_cast<const unsigned char*>(pixels)+(size_t(y)*q.buffers[sourceBuffers]->getStride()+x)*4;
                if(std::memcmp(p,colors[(quadrant+frame)%4],4)) {
                    std::fprintf(stderr,"QUEUE pixel_mismatch frame=%d quadrant=%d BGRA=%u,%u,%u,%u\n",frame,quadrant,p[0],p[1],p[2],p[3]);mismatch=1;
                }
            }
            if(module->unlock(module,q.owned[sourceBuffers]) || mismatch) return 1;
            if(q.consumer->releaseBuffer(acquired,release)) return 1;acquired=BufferItem{};hasAcquired=false;
            ++completed;
            std::printf("QUEUE frame=%d buffer=%d swap_ms=%.3f acquire_wait_ms=%.3f compose_wait_ms=%.3f sparse_check_release_ms=%.3f pixel_check=PASS full_frame_cpu_copy=false\n",frame,index,swapEnd-begin,acquireEnd-swapEnd,composeEnd-acquireEnd,now()-composeEnd);
        }
        if (!validQueueReuse(seen,sourceBuffers,completed)) return 1;
        std::printf("QUEUE reuse_counts=%d,%d,%d source_buffers=%d native_release_fences=true\n",seen[0],seen[1],seen[2],sourceBuffers);return 0;
    }();
    if(result) {
        std::fprintf(stderr,"QUEUE result=FAIL frames=%d EGL_error=0x%x acquired=%d producer_current=%d retained_resources=true\n",completed,eglGetError(),hasAcquired,producerCurrent);
        // Do not free/unregister buffers or abandon slots after a failed fence.
        // External timeout/capture preserves partial logs; no module unloading.
        std::fflush(nullptr);_Exit(1);
    }
    stage("cleanup");int cleanup=0;
    if(!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,consumerContext)) cleanup=1;
    glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteFramebuffers(1,&fbo);glDeleteTextures(textures.size(),textures.data());
    glDeleteProgram(consumerProgram);glDeleteShader(shaders[2]);glDeleteShader(shaders[3]);
    for(auto image:images) if(image!=EGL_NO_IMAGE_KHR && !eglDestroyImageKHR(display,image)) cleanup=1;
    if(!eglMakeCurrent(display,surface,surface,producerContext)) cleanup=1;
    glDeleteProgram(producerProgram);glDeleteShader(shaders[0]);glDeleteShader(shaders[1]);
    if(!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT) ||
        !eglDestroySurface(display,surface) || !eglDestroyContext(display,producerContext) ||
        !eglDestroyContext(display,consumerContext) || !eglTerminate(display)) cleanup=1;
    if(q.cleanup()) cleanup=1;
    std::printf("QUEUE result=%s frames=%d real_BufferQueue=true Binder_IPC=false SurfaceFlinger=false scanout=false sustained_fps_claim=false\n",cleanup?"FAIL":"PASS",completed);
    return cleanup;
}
