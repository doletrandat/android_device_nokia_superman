// SPDX-License-Identifier: Apache-2.0
// Early-boot BGRA DMA-BUF interoperability and framebuffer descriptor check; no post.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <hardware/gralloc.h>
#include <hardware/fb.h>
#include <sync/sync.h>
#include <cutils/native_handle.h>
#include <unistd.h>
#include <algorithm>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <chrono>

static void stage(const char* name) {
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::printf("SUPERMAN_BUFFER_TEST monotonic_ms=%.3f stage=%s\n", ms, name);
}
static bool extension(const char* list, const char* word) {
    if (!list) return false;
    const size_t length = std::strlen(word);
    for (const char* p = list; (p = std::strstr(p, word)); p += length)
        if ((p == list || p[-1] == ' ') && (p[length] == ' ' || p[length] == '\0')) return true;
    return false;
}
// Exercise the Gralloc0Hal lock rectangle forwarded by ImageReader/rotation.
// A clone/import models the handle metadata and FD ownership in another process.
static bool rgbaImageReaderLockCheck(gralloc_module_t* module, alloc_device_t* allocator) {
    buffer_handle_t original = nullptr;
    native_handle_t* imported = nullptr;
    bool registered = false, locked = false, originalLocked = false;
    int stride = 0;
    constexpr int size = 64;
    constexpr unsigned char color[4] = {17, 93, 201, 255};
    const int usage = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE |
        GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;
    const bool passed = [&]() {
        if (!module->registerBuffer || !module->unregisterBuffer ||
            allocator->alloc(allocator, size, size, HAL_PIXEL_FORMAT_RGBA_8888,
                             usage, &original, &stride) || !original || stride < size) return false;
        void* data = nullptr;
        if (module->lock(module, original, GRALLOC_USAGE_SW_WRITE_OFTEN,
                         0, 0, size, size, &data)) return false;
        originalLocked = true;
        if (!data) return false;
        // Initialize all pixels before reading through the imported handle.
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                std::memcpy(static_cast<unsigned char*>(data) + (y * stride + x) * 4, color, 4);
        if (module->unlock(module, original)) return false;
        originalLocked = false;
        imported = native_handle_clone(original);
        if (!imported || module->registerBuffer(module, imported)) return false;
        registered = true;
        // All-zero accessRegion is the full buffer, per IMapper 2.0.
        const int rectangles[][4] = {{0,0,0,0}, {0,0,size,size}, {7,9,11,13}};
        for (const auto& rect : rectangles) {
            data = nullptr;
            if (module->lock(module, imported, GRALLOC_USAGE_SW_READ_OFTEN,
                             rect[0], rect[1], rect[2], rect[3], &data)) return false;
            locked = true;
            if (!data) return false;
            // Mapper data points to buffer origin even for a nonzero rectangle.
            const int x = rect[0], y = rect[1];
            if (std::memcmp(static_cast<unsigned char*>(data) + (y * stride + x) * 4, color, 4)) return false;
            if (module->unlock(module, imported)) return false;
            locked = false;
        }
        return true;
    }();
    bool cleanup = true;
    if (originalLocked) cleanup = !module->unlock(module, original);
    if (locked) cleanup = !module->unlock(module, imported);
    if (registered) cleanup = !module->unregisterBuffer(module, imported) && cleanup;
    if (imported) { native_handle_close(imported); native_handle_delete(imported); }
    if (original) cleanup = !allocator->free(allocator, original) && cleanup;
    if (passed && cleanup) stage("RGBA_import_empty_full_and_subregion_CPU_lock_verified");
    else stage("RGBA_CPU_lock_failed");
    return passed && cleanup;
}

int main(int argc, char** argv) {
    const char* halPath="/vendor/lib/hw/gralloc.superman_drm.so";
    const char* mesaPath="/vendor/lib/egl/libGLES_mesa.so";
    if(argc==6&&!std::strcmp(argv[1],"--run-once")&&!std::strcmp(argv[2],"--mesa-library")&&!std::strcmp(argv[4],"--gralloc-library")&&!std::strncmp(argv[3],"/data/local/tmp/",16)&&!std::strncmp(argv[5],"/data/local/tmp/",16)){mesaPath=argv[3];halPath=argv[5];}
    else if(argc!=2||std::strcmp(argv[1],"--run-once")){std::fprintf(stderr,"Usage: --run-once [--mesa-library /data/local/tmp/... --gralloc-library /data/local/tmp/...]\n");return 2;}
    alarm(18);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    stage("libraries_begin");
    void* hal = dlopen(halPath, RTLD_NOW | RTLD_LOCAL);
    void* mesa = dlopen(mesaPath, RTLD_NOW | RTLD_LOCAL);
    if (!hal || !mesa) { std::fprintf(stderr, "Library load failed: %s\n", dlerror()); return 1; }
#define LOAD(name) auto name = reinterpret_cast<decltype(&::name)>(dlsym(mesa, #name)); \
    if (!name) { std::fprintf(stderr, "Missing %s\n", #name); return 1; }
    LOAD(eglGetProcAddress) LOAD(eglInitialize) LOAD(eglChooseConfig) LOAD(eglBindAPI)
    LOAD(eglCreateContext) LOAD(eglCreatePbufferSurface) LOAD(eglMakeCurrent)
    LOAD(eglDestroySurface) LOAD(eglDestroyContext) LOAD(eglTerminate) LOAD(eglQueryString) LOAD(eglGetError)
#undef LOAD
#define GL(name) auto name = reinterpret_cast<decltype(&::name)>(eglGetProcAddress(#name)); \
    if (!name) { std::fprintf(stderr, "Missing %s\n", #name); return 1; }
    GL(glGetString) GL(glGetError) GL(glGenRenderbuffers) GL(glBindRenderbuffer)
    GL(glDeleteRenderbuffers) GL(glGenFramebuffers) GL(glBindFramebuffer)
    GL(glDeleteFramebuffers) GL(glFramebufferRenderbuffer) GL(glCheckFramebufferStatus)
    GL(glEnable) GL(glDisable) GL(glScissor) GL(glClearColor) GL(glClear)
    GL(glFlush) GL(glReadPixels)
#undef GL
    auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    auto imageStorage = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    auto createSync=reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    auto destroySync=reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    auto dupFence=reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    if(!createSync||!destroySync||!dupFence)return 1;
    auto* module = reinterpret_cast<gralloc_module_t*>(dlsym(hal, HAL_MODULE_INFO_SYM_AS_STR));
    if (!platform || !createImage || !destroyImage || !imageStorage || !module ||
        !module->common.methods || !module->common.methods->open || !module->lock || !module->unlock) return 1;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    hw_device_t* device = nullptr;
    alloc_device_t* allocator = nullptr;
    buffer_handle_t handle = nullptr;
    GLuint rb = 0, fb = 0;
    bool initialized = false, current = false, locked = false;
    constexpr int width = 720, height = 1280;
    const int result = [&]() -> int {
        framebuffer_device_t* descriptor=nullptr;
        if(framebuffer_open(&module->common,&descriptor)||!descriptor)return 1;
        const bool supported=descriptor->width==width&&descriptor->height==height&&descriptor->format==HAL_PIXEL_FORMAT_BGRA_8888&&descriptor->stride>=width;
        std::printf("STARTUP framebuffer=%dx%d format=%d stride=%d descriptor_only_no_post=true\n",descriptor->width,descriptor->height,descriptor->format,descriptor->stride);
        int descriptorClose=framebuffer_close(descriptor);if(!supported||descriptorClose)return 1;
        stage("gralloc_open_allocate");
        if (module->common.methods->open(&module->common, GRALLOC_HARDWARE_GPU0, &device) || !device) return 1;
        allocator = reinterpret_cast<alloc_device_t*>(device);
        if (!allocator->alloc || !allocator->free) return 1;
        if (!rgbaImageReaderLockCheck(module, allocator)) return 1;
        int stride = 0;
        const int usage = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE |
            GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_HW_FB;
        if (allocator->alloc(allocator, width, height, HAL_PIXEL_FORMAT_BGRA_8888, usage, &handle, &stride) || !handle) return 1;
        if (handle->numFds != 1 || handle->data[0] < 0 || stride < width || stride > 8192) return 1;
        std::printf("DMA-BUF fd=%d dimensions=%dx%d stride_pixels=%d format=ARGB8888 linear-requested\n", handle->data[0], width, height, stride);
        stage("egl_initialize");
        display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return 1;
        initialized = true;
        if (!extension(eglQueryString(display, EGL_EXTENSIONS), "EGL_EXT_image_dma_buf_import")) return 1;
        const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (!eglChooseConfig(display, configAttrs, &config, 1, &count) || count != 1 || !eglBindAPI(EGL_OPENGL_ES_API)) return 1;
        const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        const EGLint surfAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttrs);
        surface = eglCreatePbufferSurface(display, config, surfAttrs);
        if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || !eglMakeCurrent(display, surface, surface, context)) return 1;
        current = true;
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        std::printf("GL_RENDERER=%s\n", renderer ? renderer : "null");
        if (!renderer || !(std::strstr(renderer, "FD305") ||
            (std::strstr(renderer, "Adreno") && std::strstr(renderer, "305"))) ||
            !extension(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)), "GL_OES_EGL_image")) return 1;
        constexpr EGLint argb8888 = 'A' | ('R' << 8) | ('2' << 16) | ('4' << 24);
        const EGLint imageAttrs[] = {EGL_WIDTH, width, EGL_HEIGHT, height,
            EGL_LINUX_DRM_FOURCC_EXT, argb8888, EGL_DMA_BUF_PLANE0_FD_EXT, handle->data[0],
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, stride * 4, EGL_NONE};
        stage("dma_buf_import");
        image = createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, imageAttrs);
        if (image == EGL_NO_IMAGE_KHR) return 1;
        glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
        imageStorage(GL_RENDERBUFFER, image);
        glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        if (!rb || !fb || glGetError() != GL_NO_ERROR || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 1;
        const unsigned char colors[4][4] = {{255,0,0,255}, {0,255,0,255}, {0,0,255,255}, {255,255,255,255}};
        stage("quadrant_render");
        glEnable(GL_SCISSOR_TEST);
        for (int i = 0; i < 4; ++i) {
            glScissor((i % 2) * (width / 2), (i / 2) * (height / 2), width / 2, height / 2);
            glClearColor(colors[i][0] / 255.0f, colors[i][1] / 255.0f, colors[i][2] / 255.0f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        EGLSyncKHR sync=createSync(display,EGL_SYNC_NATIVE_FENCE_ANDROID,nullptr);if(sync==EGL_NO_SYNC_KHR)return 1;
        glFlush();int fence=dupFence(display,sync);destroySync(display,sync);if(fence<0)return 1;
        int waited=sync_wait(fence,5000);auto* info=sync_file_info(fence);
        bool valid=!waited&&info&&info->status>0;if(info)sync_file_info_free(info);close(fence);
        if(!valid)return 1;stage("native_fence_signaled_and_validated");
        if (glGetError() != GL_NO_ERROR) return 1;
        stage("gpu_readback");
        for (int i = 0; i < 4; ++i) {
            unsigned char pixel[4] = {};
            glReadPixels((i % 2) * (width / 2) + width / 4, (i / 2) * (height / 2) + height / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            if (glGetError() != GL_NO_ERROR || std::memcmp(pixel, colors[i], 4)) return 1;
        }
        stage("gralloc_cpu_map");
        void* mapping = nullptr;
        if (module->lock(module, handle, GRALLOC_USAGE_SW_READ_OFTEN, 0, 0, width, height, &mapping)) return 1;
        locked = true;
        if (!mapping) return 1;
        const auto* bytes = static_cast<const unsigned char*>(mapping);
        for (int i = 0; i < 4; ++i) {
            const int x0 = (i % 2) * (width / 2), y0 = (i / 2) * (height / 2);
            const int points[][2] = {{x0 + 1, y0 + 1}, {x0 + width / 2 - 2, y0 + height / 2 - 2}, {x0 + width / 4, y0 + height / 4}};
            for (const auto& point : points) {
                const auto* pixel = bytes + (static_cast<size_t>(point[1]) * stride + point[0]) * 4;
                unsigned char bgra[4];std::memcpy(bgra,colors[i],4);std::swap(bgra[0],bgra[2]);
                if (std::memcmp(pixel, bgra, 4)) {
                    std::fprintf(stderr, "CPU layout mismatch x=%d y=%d BGRA=%u,%u,%u,%u\n", point[0], point[1], pixel[0], pixel[1], pixel[2], pixel[3]);
                    return 1;
                }
            }
        }
        const int unlockResult = module->unlock(module, handle);
        locked = false;
        if (unlockResult) return 1;
        stage("shared_buffer_verified");
        return 0;
    }();
    if (result) std::fprintf(stderr, "Probe failed: EGL_error=0x%x GL_error=0x%x\n",
        eglGetError(), current ? glGetError() : 0);
    if (locked) module->unlock(module, handle);
    if (current) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (fb) glDeleteFramebuffers(1, &fb);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        if (rb) glDeleteRenderbuffers(1, &rb);
    }
    if (image != EGL_NO_IMAGE_KHR) destroyImage(display, image);
    if (current) eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    if (initialized) eglTerminate(display);
    int cleanup = 0;
    if (handle && allocator) cleanup = allocator->free(allocator, handle);
    if (device && device->close) cleanup |= device->close(device);
    dlclose(mesa); dlclose(hal);
    if (result || cleanup) { stage("FAIL"); return 1; }
    stage("PASS_no_scanout_no_fps_claim");
    return 0;
}
