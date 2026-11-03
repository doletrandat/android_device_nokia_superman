// SPDX-License-Identifier: Apache-2.0
// Explicit DMA-BUF interoperability probe; never posts a framebuffer.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <hardware/gralloc.h>
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
int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--run-once") != 0) {
        std::fprintf(stderr, "Usage: %s --run-once (720x1280, no scanout)\n", argv[0]);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    stage("libraries_begin");
    void* hal = dlopen("/vendor/lib/hw/gralloc.superman_drm.so", RTLD_NOW | RTLD_LOCAL);
    void* mesa = dlopen("/vendor/lib/egl/libGLES_mesa.so", RTLD_NOW | RTLD_LOCAL);
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
    GL(glFinish) GL(glReadPixels)
#undef GL
    auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    auto imageStorage = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
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
        stage("gralloc_open_allocate");
        if (module->common.methods->open(&module->common, GRALLOC_HARDWARE_GPU0, &device) || !device) return 1;
        allocator = reinterpret_cast<alloc_device_t*>(device);
        if (!allocator->alloc || !allocator->free) return 1;
        int stride = 0;
        const int usage = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE |
            GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;
        if (allocator->alloc(allocator, width, height, HAL_PIXEL_FORMAT_RGBA_8888, usage, &handle, &stride) || !handle) return 1;
        if (handle->numFds != 1 || handle->data[0] < 0 || stride < width || stride > 8192) return 1;
        std::printf("DMA-BUF fd=%d dimensions=%dx%d stride_pixels=%d format=ABGR8888 linear-requested\n", handle->data[0], width, height, stride);
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
        constexpr EGLint abgr8888 = 'A' | ('B' << 8) | ('2' << 16) | ('4' << 24);
        const EGLint imageAttrs[] = {EGL_WIDTH, width, EGL_HEIGHT, height,
            EGL_LINUX_DRM_FOURCC_EXT, abgr8888, EGL_DMA_BUF_PLANE0_FD_EXT, handle->data[0],
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
        glDisable(GL_SCISSOR_TEST); glFinish();
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
                if (std::memcmp(pixel, colors[i], 4)) {
                    std::fprintf(stderr, "CPU layout mismatch x=%d y=%d RGBA=%u,%u,%u,%u\n", point[0], point[1], pixel[0], pixel[1], pixel[2], pixel[3]);
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
