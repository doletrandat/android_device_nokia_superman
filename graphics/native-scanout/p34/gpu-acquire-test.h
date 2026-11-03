// SPDX-License-Identifier: Apache-2.0
// Explicit laboratory GPU producer. No Android window or consumer release.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <time.h>
#include "gpu-test/acquire-gate.h"

static double gpu_now_ms() {
    timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
static bool gpu_extension(const char *list, const char *name) {
    if (!list) return false;
    std::istringstream stream(list); std::string token;
    while (stream >> token) if (token == name) return true;
    return false;
}
struct GpuProducer {
    void *library = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    bool initialized = false, current = false;
    GLuint program = 0, vertex = 0, fragment = 0;
    GLint position = -1, frame_uniform = -1;
    EGLImageKHR image[6] = {};
    GLuint texture[6] = {}, framebuffer[6] = {};
#define GPU_FN(name) decltype(&::name) name = nullptr;
    GPU_FN(eglGetProcAddress) GPU_FN(eglInitialize) GPU_FN(eglChooseConfig)
    GPU_FN(eglQueryString) GPU_FN(eglBindAPI) GPU_FN(eglCreateContext)
    GPU_FN(eglCreatePbufferSurface) GPU_FN(eglMakeCurrent) GPU_FN(eglDestroySurface)
    GPU_FN(eglDestroyContext) GPU_FN(eglTerminate) GPU_FN(eglGetError)
    GPU_FN(glGetString) GPU_FN(glGetError) GPU_FN(glGenTextures) GPU_FN(glBindTexture)
    GPU_FN(glTexParameteri) GPU_FN(glDeleteTextures) GPU_FN(glGenFramebuffers)
    GPU_FN(glBindFramebuffer) GPU_FN(glFramebufferTexture2D) GPU_FN(glCheckFramebufferStatus)
    GPU_FN(glDeleteFramebuffers) GPU_FN(glCreateShader) GPU_FN(glShaderSource)
    GPU_FN(glCompileShader) GPU_FN(glGetShaderiv) GPU_FN(glGetShaderInfoLog)
    GPU_FN(glDeleteShader) GPU_FN(glCreateProgram) GPU_FN(glAttachShader)
    GPU_FN(glLinkProgram) GPU_FN(glGetProgramiv) GPU_FN(glDeleteProgram)
    GPU_FN(glUseProgram) GPU_FN(glGetAttribLocation) GPU_FN(glGetUniformLocation)
    GPU_FN(glUniform1f) GPU_FN(glVertexAttribPointer) GPU_FN(glEnableVertexAttribArray)
    GPU_FN(glViewport) GPU_FN(glDrawArrays) GPU_FN(glFlush)
#undef GPU_FN
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform = nullptr;
    PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
    PFNEGLCREATESYNCKHRPROC create_sync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture = nullptr;
    bool initialize() {
        library = dlopen("/vendor/lib/egl/libGLES_mesa.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) { fprintf(stderr, "NATIVE_GPU_ERROR stage=mesa_load detail=%s\n", dlerror()); return false; }
#define GPU_EGL(name) name = reinterpret_cast<decltype(name)>(dlsym(library, #name)); if (!name) return missing(#name);
        GPU_EGL(eglGetProcAddress) GPU_EGL(eglInitialize) GPU_EGL(eglChooseConfig)
        GPU_EGL(eglQueryString) GPU_EGL(eglBindAPI) GPU_EGL(eglCreateContext)
        GPU_EGL(eglCreatePbufferSurface) GPU_EGL(eglMakeCurrent) GPU_EGL(eglDestroySurface)
        GPU_EGL(eglDestroyContext) GPU_EGL(eglTerminate) GPU_EGL(eglGetError)
#undef GPU_EGL
#define GPU_PROC(field, name) field = reinterpret_cast<decltype(field)>(eglGetProcAddress(name)); if (!field) return missing(name);
        GPU_PROC(platform, "eglGetPlatformDisplayEXT")
        GPU_PROC(create_image, "eglCreateImageKHR") GPU_PROC(destroy_image, "eglDestroyImageKHR")
        GPU_PROC(create_sync, "eglCreateSyncKHR") GPU_PROC(destroy_sync, "eglDestroySyncKHR")
        GPU_PROC(dup_fence, "eglDupNativeFenceFDANDROID") GPU_PROC(image_texture, "glEGLImageTargetTexture2DOES")
#define GPU_GL(name) GPU_PROC(name, #name)
        GPU_GL(glGetString) GPU_GL(glGetError) GPU_GL(glGenTextures) GPU_GL(glBindTexture)
        GPU_GL(glTexParameteri) GPU_GL(glDeleteTextures) GPU_GL(glGenFramebuffers)
        GPU_GL(glBindFramebuffer) GPU_GL(glFramebufferTexture2D) GPU_GL(glCheckFramebufferStatus)
        GPU_GL(glDeleteFramebuffers) GPU_GL(glCreateShader) GPU_GL(glShaderSource)
        GPU_GL(glCompileShader) GPU_GL(glGetShaderiv) GPU_GL(glGetShaderInfoLog)
        GPU_GL(glDeleteShader) GPU_GL(glCreateProgram) GPU_GL(glAttachShader)
        GPU_GL(glLinkProgram) GPU_GL(glGetProgramiv) GPU_GL(glDeleteProgram)
        GPU_GL(glUseProgram) GPU_GL(glGetAttribLocation) GPU_GL(glGetUniformLocation)
        GPU_GL(glUniform1f) GPU_GL(glVertexAttribPointer) GPU_GL(glEnableVertexAttribArray)
        GPU_GL(glViewport) GPU_GL(glDrawArrays) GPU_GL(glFlush)
#undef GPU_GL
#undef GPU_PROC
        display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return error("egl_initialize");
        initialized = true;
        const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
        if (!gpu_extension(extensions, "EGL_EXT_image_dma_buf_import") ||
            !gpu_extension(extensions, "EGL_ANDROID_native_fence_sync")) return error("required_egl_extensions");
        const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLConfig config = nullptr; EGLint count = 0;
        if (!eglChooseConfig(display, attrs, &config, 1, &count) || count != 1 || !eglBindAPI(EGL_OPENGL_ES_API)) return error("egl_config");
        const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        const EGLint sa[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ca);
        surface = eglCreatePbufferSurface(display, config, sa);
        if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || !eglMakeCurrent(display, surface, surface, context)) return error("egl_context");
        current = true;
        const char *renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char *gl_extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        fprintf(stderr, "NATIVE_GPU renderer=%s explicit_dma_buf_FBO=1 native_fence=1\n", renderer ? renderer : "null");
        if (!renderer || !strstr(renderer, "FD305") || !gpu_extension(gl_extensions, "GL_OES_EGL_image")) return error("gpu_renderer_extensions");
        auto compile = [&](GLenum type, const char *source) -> GLuint {
            GLuint s = glCreateShader(type); if (!s) return 0;
            glShaderSource(s, 1, &source, nullptr); glCompileShader(s);
            GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) { char log[1024] = {}; glGetShaderInfoLog(s, sizeof(log), nullptr, log); fprintf(stderr, "NATIVE_GPU shader=%s\n", log); glDeleteShader(s); return 0; }
            return s;
        };
        vertex = compile(GL_VERTEX_SHADER, "attribute vec2 p; void main(){gl_Position=vec4(p,0.,1.);}");
        fragment = compile(GL_FRAGMENT_SHADER, "precision highp float; uniform float frame; void main(){float q=mod(floor(gl_FragCoord.x/180.)+frame,4.); vec3 c=q<.5?vec3(1,0,0):q<1.5?vec3(0,1,0):q<2.5?vec3(0,0,1):vec3(1,1,1); gl_FragColor=vec4(c,1.);}");
        if (!vertex || !fragment) return error("shader_compile");
        program = glCreateProgram(); if (!program) return error("program_create");
        glAttachShader(program, vertex); glAttachShader(program, fragment); glLinkProgram(program);
        GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) return error("program_link");
        glUseProgram(program); position = glGetAttribLocation(program, "p"); frame_uniform = glGetUniformLocation(program, "frame");
        return position >= 0 && frame_uniform >= 0 && glGetError() == GL_NO_ERROR;
    }
    bool missing(const char *name) { fprintf(stderr, "NATIVE_GPU_ERROR stage=symbol name=%s\n", name); return false; }
    bool error(const char *name) { fprintf(stderr, "NATIVE_GPU_ERROR stage=%s egl=0x%x gl=0x%x\n", name, eglGetError ? eglGetError() : 0, current && glGetError ? glGetError() : 0); return false; }
    bool import(unsigned i, int prime, uint32_t pitch) {
        if (i >= 6 || image[i] || texture[i] || framebuffer[i]) return false;
        const EGLint attrs[] = {EGL_WIDTH, 720, EGL_HEIGHT, 1280, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, prime, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(pitch), EGL_NONE};
        image[i] = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
        if (image[i] == EGL_NO_IMAGE_KHR) return error("dma_buf_image");
        glGenTextures(1, &texture[i]); glBindTexture(GL_TEXTURE_2D, texture[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        image_texture(GL_TEXTURE_2D, image[i]);
        glGenFramebuffers(1, &framebuffer[i]); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture[i], 0);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
    }
    // Only a signaled later physical latch permits retiring a scanout target.
    bool release(unsigned i) {
        if (i >= 6 || !current) return false;
        glDeleteFramebuffers(1, &framebuffer[i]);
        glDeleteTextures(1, &texture[i]);
        if (glGetError() != GL_NO_ERROR) return error("retired_target_GL_cleanup");
        framebuffer[i] = texture[i] = 0;
        if (image[i] != EGL_NO_IMAGE_KHR && !destroy_image(display, image[i]))
            return error("retired_target_image_cleanup");
        image[i] = EGL_NO_IMAGE_KHR;
        return true;
    }
    int draw(unsigned i, unsigned frame) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer[i]); glUseProgram(program);
        static const GLfloat vertices[] = {-1,-1, 3,-1, -1,3};
        glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, vertices); glEnableVertexAttribArray(position);
        glUniform1f(frame_uniform, static_cast<float>(frame % 4)); glViewport(0, 0, 720, 1280);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (glGetError() != GL_NO_ERROR) { error("draw"); return -1; }
        const EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE};
        EGLSyncKHR sync = create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
        if (sync == EGL_NO_SYNC_KHR) { error("gpu_fence_create"); return -1; }
        glFlush(); // Required before exporting the native fence FD.
        int fd = dup_fence(display, sync);
        bool ok = destroy_sync(display, sync) && glGetError() == GL_NO_ERROR;
        if (fd < 0 || !ok) { if (fd >= 0) close(fd); error("gpu_fence_export"); return -1; }
        return fd;
    }
    bool cleanup() {
        bool ok = true;
        if (current) {
            glDeleteFramebuffers(6, framebuffer); glDeleteTextures(6, texture);
            if (program) glDeleteProgram(program);
            if (vertex) glDeleteShader(vertex);
            if (fragment) glDeleteShader(fragment);
            if (glGetError() != GL_NO_ERROR) ok = false;
        }
        if (initialized) for (auto im : image) if (im != EGL_NO_IMAGE_KHR && !destroy_image(display, im)) ok = false;
        if (current && !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) ok = false;
        if (surface != EGL_NO_SURFACE && !eglDestroySurface(display, surface)) ok = false;
        if (context != EGL_NO_CONTEXT && !eglDestroyContext(display, context)) ok = false;
        if (initialized && !eglTerminate(display)) ok = false;
        return ok;
    }
};
static bool gpu_pixels(Target &target, unsigned frame) {
    uint32_t pitch = 0; void *data = nullptr;
    auto *pixels = static_cast<uint8_t*>(gbm_bo_map(target.bo, 0, 0, 720, 1280, GBM_BO_TRANSFER_READ, &pitch, &data));
    if (!pixels) return false;
    bool ok = pitch == gbm_bo_get_stride(target.bo) && pitch >= 2880 && pitch % 4 == 0 && pitch <= 65535;
    const uint32_t colors[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
    if (ok) for (unsigned y : {64U,640U,1216U}) for (unsigned band = 0; band < 4; ++band) {
        unsigned x = 45 + 180 * band;
        uint32_t actual = reinterpret_cast<uint32_t*>(pixels + y * pitch)[x];
        uint32_t expected = colors[(band + frame) % 4];
        if (actual != expected) { fprintf(stderr, "NATIVE_GPU_ERROR stage=pixel x=%u y=%u actual=%08x expected=%08x\n", x, y, actual, expected); ok = false; }
    }
    gbm_bo_unmap(target.bo, data); return ok;
}
static int gpu_flip_test(int fd, const drmModeModeInfo &mode, uint32_t crtc, uint32_t conn, uint32_t pid) {
    int render = -1, gate_device = -1, acquire = -1, gate = -1, merged = -1, result = 0;
    gbm_device *gbm = nullptr; Target target[3]; GpuProducer gpu;
    uint32_t blob = 0; bool attempted = false, restored = false, delayed_pass = false;
    auto commit = [&](uint32_t fb, bool test, bool restore, int in_fence, bool delayed) -> int {
        auto *req = drmModeAtomicAlloc(); if (!req) return -ENOMEM;
        int out = -1, ret = 0;
        auto add = [&](uint32_t obj, uint32_t type, const char *name, uint64_t value) {
            if (ret) return;
            uint32_t p = property(fd, obj, type, name);
            if (!p || drmModeAtomicAddProperty(req, obj, p, value) < 0) ret = -EINVAL;
        };
        add(conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", restore ? 0 : crtc);
        add(crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", restore ? 0 : 1);
        add(crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", restore ? 0 : blob);
        if (!test) add(crtc, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR", reinterpret_cast<uintptr_t>(&out));
        add(pid, DRM_MODE_OBJECT_PLANE, "FB_ID", fb);
        add(pid, DRM_MODE_OBJECT_PLANE, "CRTC_ID", restore ? 0 : crtc);
        if (!restore) {
            add(pid, DRM_MODE_OBJECT_PLANE, "SRC_X", 0); add(pid, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
            add(pid, DRM_MODE_OBJECT_PLANE, "SRC_W", 720U << 16); add(pid, DRM_MODE_OBJECT_PLANE, "SRC_H", 1280U << 16);
            add(pid, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0); add(pid, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
            add(pid, DRM_MODE_OBJECT_PLANE, "CRTC_W", 720); add(pid, DRM_MODE_OBJECT_PLANE, "CRTC_H", 1280);
            if (!test && in_fence < 0) ret = -EINVAL;
            if (!ret && delayed) {
                superman_acquire_gate request{SUPERMAN_GATE_DELAY_MS, -1};
                if (ioctl(gate_device, SUPERMAN_ACQUIRE_GATE, &request)) ret = -errno;
                else {
                    gate = request.fence_fd;
                    double created = gpu_now_ms();
                    merged = sync_merge("superman-gpu-gate", in_fence, gate);
                    if (gate < 0 || merged < 0) ret = -EIO;
                    else if (!sync_wait(gate, 0) || errno != ETIME || !sync_wait(merged, 0) || errno != ETIME || gpu_now_ms() - created > 50) ret = -EIO;
                    else in_fence = merged;
                }
            }
            add(pid, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", static_cast<uint64_t>(static_cast<int64_t>(test ? -1 : in_fence)));
        }
        double begin = gpu_now_ms();
        if (!ret) {
            if (!test && !restore) attempted = true;
            ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET | (test ? DRM_MODE_ATOMIC_TEST_ONLY : 0), nullptr);
        }
        double elapsed = gpu_now_ms() - begin;
        drmModeAtomicFree(req);
        if (!ret && !test && (out < 0 || sync_wait(out, 1000))) ret = -ETIMEDOUT;
        if (!ret && delayed) {
            if (elapsed < 350 || sync_wait(gate, 0) || sync_wait(merged, 0)) ret = -EIO;
            else delayed_pass = true;
            fprintf(stderr, "NATIVE_ACQUIRE delayed_gpu_plus_software_gate=%s commit_ms=%.3f software_delay_ms=500 gpu_latency_claim=false\n", delayed_pass ? "passed" : "failed", elapsed);
        }
        if (out >= 0) close(out);
        fprintf(stderr, "NATIVE_GPU_COMMIT phase=%s fb=%u result=%d in_fence=%d output=%s\n", restore ? "restore" : test ? "test_only" : "flip", fb, ret, in_fence, test ? "not_requested" : !ret ? "signaled" : "unconfirmed");
        return ret;
    };
    auto run = [&]() -> int {
        gate_device = open("/dev/superman-acquire-gate", O_RDWR | O_CLOEXEC);
        if (gate_device < 0) return probe_error("gate_open", 13, errno);
        render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (render < 0 || !version(render, "msm")) return probe_error("gpu_render_open", 13, errno);
        gbm = gbm_create_device(render); if (!gbm || !gpu.initialize()) return 13;
        for (unsigned i = 0; i < 3; ++i) {
            auto &t = target[i];
            t.bo = gbm_bo_create(gbm, 720, 1280, GBM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
            if (!t.bo || gbm_bo_get_modifier(t.bo) != DRM_FORMAT_MOD_LINEAR) return 14;
            uint32_t pitch = gbm_bo_get_stride(t.bo);
            if (pitch < 2880 || pitch % 4 || pitch > 65535) return 14;
            t.prime = gbm_bo_get_fd(t.bo);
            if (t.prime < 0 || !gpu.import(i, t.prime, pitch)) return 14;
            if (drmPrimeFDToHandle(fd, t.prime, &t.handle)) return 14;
            uint32_t handles[4] = {t.handle}, pitches[4] = {pitch}, offsets[4] = {};
            if (drmModeAddFB2(fd, 720, 1280, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &t.fb, 0)) return 14;
            fprintf(stderr, "NATIVE_GPU_TARGET target=%u stride=%u PRIME_import=passed FBO=complete producer=GPU\n", i, pitch);
        }
        if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &blob)) return 15;
        if (commit(target[0].fb, true, false, -1, false)) return 15;
        for (unsigned frame = 0; frame < 6; ++frame) {
            unsigned i = frame % 3;
            acquire = gpu.draw(i, frame);
            if (acquire < 0) return 16;
            // Verify the producer before scanout. The actual exported GPU FD is
            // still passed to KMS; the controlled delay is a separate fence.
            if (sync_wait(acquire, 1000) || !gpu_pixels(target[i], frame)) return 16;
            fprintf(stderr, "NATIVE_GPU_DRAW frame=%u target=%u pixels=12-pass GPU_fence=signaled CPU_writes=false\n", frame, i);
            if (commit(target[i].fb, false, false, acquire, frame == 0)) return 17;
            close(acquire); acquire = -1;
            if (merged >= 0) { close(merged); merged = -1; }
            if (gate >= 0) { close(gate); gate = -1; }
            printf("NATIVE_GPU_FLIP frame=%u target=%u GPU_acquire=explicit latch_fence=signaled\n", frame, i); fflush(stdout);
        }
        return delayed_pass ? 0 : 18;
    };
    result = run();
    if (attempted) restored = !commit(0, false, true, -1, false);
    for (int f : {acquire, merged, gate, gate_device}) if (f >= 0) close(f);
    if (!gpu.cleanup()) result = 19;
    for (auto &t : target) {
        if (t.fb && drmModeRmFB(fd, t.fb)) result = 19;
        if (t.handle) { drm_gem_close c{}; c.handle = t.handle; if (drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c)) result = 19; }
        if (t.prime >= 0) close(t.prime);
        if (t.bo) gbm_bo_destroy(t.bo);
    }
    if (blob && drmModeDestroyPropertyBlob(fd, blob)) result = 19;
    if (gbm) gbm_device_destroy(gbm);
    if (render >= 0) close(render);
    if (gpu.library && dlclose(gpu.library)) result = 19;
    if (attempted && !restored) result = 11;
    printf("NATIVE_GPU_TEST result=%d original_restore=%s producer=GPU explicit_acquire=true delayed_gate=%s UI_held=true\n", result, attempted ? restored ? "confirmed" : "unconfirmed" : "not_needed", delayed_pass ? "passed" : "failed"); fflush(stdout);
    return result;
}
