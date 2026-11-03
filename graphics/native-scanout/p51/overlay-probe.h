// SPDX-License-Identifier: Apache-2.0
// Two sequential overlay generations. Never owns a DRM node or releases apps.
struct OverlayProbe {
    sp<SurfaceControl> control;
    sp<Surface> window;
    EGLSurface surface=EGL_NO_SURFACE;
    BufferAgreement agreement;
    std::set<uint64_t> buffers;
    uint64_t previous=0;
    unsigned generation=0,completed=0,released=0;
    bool fail_stage(const char* stage) const {
        std::printf("NATIVE_SF_OVERLAY_FAILURE generation=%u stage=%s presents=%u releases=%u dequeues=%u metadata_ready=%d EGL_error=0x%04x\n",generation,stage,completed,released,agreement.dequeues,agreement.ready(),eglGetError());
        return false;
    }
    bool open(const sp<SurfaceComposerClient>& client,EGLDisplay egl,EGLConfig config,unsigned gen) {
        if(control || window || surface!=EGL_NO_SURFACE) return fail_stage("already_open");
        generation=gen;completed=released=0;previous=0;buffers.clear();agreement=BufferAgreement{};
        char name[64];std::snprintf(name,sizeof(name),"Superman-P60-overlay-%u",gen);
        control=client->createSurface(String8(name),720,1280,PIXEL_FORMAT_BGRA_8888,ISurfaceComposerClient::eOpaque);
        if(!control || !control->isValid()) return fail_stage("create_control");
        window=control->getSurface();
        if(!window) return fail_stage("get_window");
        if(window->setDequeueTimeout(3000000000LL)!=NO_ERROR) return fail_stage("dequeue_timeout");
        window->enableFrameTimestamps(true);
        if(ANativeWindow_setDequeueBufferInterceptor(window.get(),observe_dequeue,&agreement)!=NO_ERROR) return fail_stage("interceptor");
        surface=eglCreateWindowSurface(egl,config,window.get(),nullptr);
        if(surface==EGL_NO_SURFACE) return fail_stage("create_EGL_surface");
        // Mesa may defer dequeue until first GL rendering. Surface creation is
        // not buffer validation: retain the exact interceptor check pre-queue.
        std::printf("NATIVE_SF_OVERLAY_SETUP generation=%u surface_created=1 dequeues=%u metadata_ready=%d deferred_validation=true\n",gen,agreement.dequeues,agreement.ready());
        // Opaque GPU producer pixels, cropped and alpha blended by SF's GPU.
        if(SurfaceComposerClient::Transaction{}.setLayer(control,100001)
            .setCrop(control,Rect(0,0,360,640)).setPosition(control,180,320)
            .setAlpha(control,0.75f).show(control).apply(true)!=NO_ERROR) return fail_stage("show_transaction");
        std::printf("NATIVE_SF_OVERLAY_OPEN generation=%u geometry=720x1280 crop=360x640 position=180/320 alpha=0.75 producer=GPU\n",gen);
        return true;
    }
    bool queue(EGLDisplay egl,EGLContext context,unsigned base_frame) {
        if(!control || surface==EGL_NO_SURFACE || completed>=6) return fail_stage("queue_state");
        if(!eglMakeCurrent(egl,surface,surface,context)) return fail_stage("make_current");
        if(!eglSwapInterval(egl,1)) return fail_stage("swap_interval");
        const unsigned char color[4]={static_cast<unsigned char>(generation?255:0),static_cast<unsigned char>((completed&1)?255:0),255,255};
        glViewport(0,0,720,1280);glDisable(GL_SCISSOR_TEST);
        glClearColor(color[0]/255.f,color[1]/255.f,1.f,1.f);glClear(GL_COLOR_BUFFER_BIT);
        if(!check_gl("overlay_draw",base_frame)) return fail_stage("GPU_draw");
        for(unsigned i=0;i<12;++i) {
            unsigned char pixel[4]{};glReadPixels(30+(i%4)*170,60+(i/4)*400,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            const bool read_ok=check_gl("overlay_pixels",base_frame,-1,i);
            if(!read_ok || std::memcmp(pixel,color,4)) {
                std::printf("NATIVE_SF_OVERLAY_PIXEL_FAILURE generation=%u frame=%u base_frame=%u sample=%u x=%u y=%u expected=%u/%u/%u/%u actual=%u/%u/%u/%u read_GL_ok=%d\n",generation,completed,base_frame,i,30+(i%4)*170,60+(i/4)*400,color[0],color[1],color[2],color[3],pixel[0],pixel[1],pixel[2],pixel[3],read_ok);
                return fail_stage("GPU_pixels");
            }
        }
        if(!agreement.ready()) return fail_stage("prequeue_buffer_agreement");
        std::printf("NATIVE_SF_OVERLAY_BUFFER_READY generation=%u frame=%u dequeues=%u metadata_ready=1 before_queue=true\n",generation,completed,agreement.dequeues);
        const auto frame=window->getNextFrameNumber();
        if(!eglSwapBuffers(egl,surface)) return fail_stage("swap_buffers");
        sp<GraphicBuffer> buffer;sp<Fence> acquire;float transform[16]{};
        if(window->getLastQueuedBuffer(&buffer,&acquire,transform)!=NO_ERROR || !buffer || !acquire) return fail_stage("last_queued_buffer");
        if(acquire->get()<0 || acquire->wait(1000)!=NO_ERROR) return fail_stage("GPU_acquire");
        buffers.insert(buffer->getId());if(buffers.size()>3) return fail_stage("producer_backing_limit");
        std::printf("NATIVE_SF_OVERLAY_QUEUE generation=%u frame=%u queue_frame=%" PRIu64 " base_frame=%u buffer_id=%" PRIu64 " distinct_buffers=%zu GPU_pixels=12-pass acquire_fence=signaled CPU_writes=false\n",generation,completed,frame,base_frame,buffer->getId(),buffers.size());
        Times t;if(!times(window,frame,t,false,completed==0)) return fail_stage("present_timestamps");
        std::printf("NATIVE_SF_OVERLAY_PRESENT generation=%u frame=%u acquire_ns=%" PRId64 " latch_ns=%" PRId64 " SF_GPU_composition_ns=%" PRId64 " display_present_ns=%" PRId64 " actual_fence_timestamps=true\n",generation,completed,t.acquire,t.latch,t.gpu,t.present);
        if(previous) {
            Times prior;if(!times(window,previous,prior,true)) return fail_stage("prior_release");
            ++released;std::printf("NATIVE_SF_OVERLAY_RELEASE generation=%u frame=%u release_ns=%" PRId64 " signaled=true\n",generation,completed-1,prior.release);
        }
        previous=frame;++completed;return true;
    }
    bool close(EGLDisplay egl) {
        bool ok=true;
        if(control) ok=SurfaceComposerClient::Transaction{}.hide(control).apply(true)==NO_ERROR;
        if(surface!=EGL_NO_SURFACE) {ok=eglDestroySurface(egl,surface)==EGL_TRUE && ok;surface=EGL_NO_SURFACE;}
        if(window) ok=ANativeWindow_setDequeueBufferInterceptor(window.get(),nullptr,nullptr)==NO_ERROR && ok;
        window.clear();control.clear();
        std::printf("NATIVE_SF_OVERLAY_CLOSE generation=%u presents=%u prior_releases=%u final_release=unobserved cleanup_ok=%d\n",generation,completed,released,ok);
        return ok;
    }
};
