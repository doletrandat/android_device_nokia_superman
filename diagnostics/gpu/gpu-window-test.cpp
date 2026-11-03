// SPDX-License-Identifier: Apache-2.0
// Bounded ANativeWindow ABI test, not a Binder BufferQueue or SurfaceFlinger test.
// No framebuffer writes, modesetting, animation, clock or governor changes.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <hardware/gralloc.h>
#include <system/window.h>
#include <sync/sync.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <vector>
#include "copy_rows.h"

static double now() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static bool copySelfTest() {
    const size_t widths[] = {0, 1, 15, 16, 63, 64, 65, 127, 128, 129, 831, 832, 833, 2880};
    for (size_t row : widths) {
        constexpr size_t rows = 5;
        const size_t srcStride = row + 13, dstStride = row + 7;
        std::vector<uint8_t> src(srcStride * rows + 32), standard(dstStride * rows + 32, 0xA5);
        std::vector<uint8_t> streaming(standard);
        for (size_t x = 0; x < src.size(); ++x) src[x] = x % 251;
        superman_copy_rows(standard.data() + 1, dstStride, src.data() + 3, srcStride, row, rows);
        superman_copy_rows_streaming(streaming.data() + 1, dstStride, src.data() + 3, srcStride, row, rows);
        if (standard != streaming) return false;
    }
    std::printf("WINDOW streaming_copy_selftest=PASS cases=14 unaligned_tail_padding_guards=true\n");
    return true;
}

struct Slot {
    ANativeWindowBuffer buffer{};
    int refs = 0;
    bool dequeued = false;
    bool unsafeReuse = false;
    static void inc(android_native_base_t* b) { ++reinterpret_cast<Slot*>(b)->refs; }
    static void dec(android_native_base_t* b) { --reinterpret_cast<Slot*>(b)->refs; }
};
struct Pool {
    ANativeWindow window{};
    Slot slots[3];
    const gralloc_module_t* module = nullptr;
    alloc_device_t* allocator = nullptr;
    int cursor = 0, queued = 0, expectedFrame = 0, refs = 0;
    bool failed = false;
    std::vector<unsigned char> sink, streamingSink;
    static constexpr int width = 720, height = 1280;
    static Pool* self(const ANativeWindow* w) { return reinterpret_cast<Pool*>(w->oem[0]); }
    static void inc(android_native_base_t* b) { ++self(reinterpret_cast<ANativeWindow*>(b))->refs; }
    static void dec(android_native_base_t* b) { --self(reinterpret_cast<ANativeWindow*>(b))->refs; }
    static int query(const ANativeWindow*, int what, int* out) {
        if (!out) return -EINVAL;
        switch (what) {
        case NATIVE_WINDOW_WIDTH: case NATIVE_WINDOW_DEFAULT_WIDTH: *out = width; break;
        case NATIVE_WINDOW_HEIGHT: case NATIVE_WINDOW_DEFAULT_HEIGHT: *out = height; break;
        case NATIVE_WINDOW_FORMAT: *out = HAL_PIXEL_FORMAT_BGRA_8888; break;
        case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS: *out = 1; break;
        case NATIVE_WINDOW_MAX_BUFFER_COUNT: *out = 3; break;
        case NATIVE_WINDOW_CONCRETE_TYPE: *out = NATIVE_WINDOW_SURFACE; break;
        case NATIVE_WINDOW_IS_VALID: case NATIVE_WINDOW_LAYER_COUNT: *out = 1; break;
        case NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER: case NATIVE_WINDOW_TRANSFORM_HINT:
        case NATIVE_WINDOW_CONSUMER_USAGE_BITS: case NATIVE_WINDOW_DEFAULT_DATASPACE:
        case NATIVE_WINDOW_BUFFER_AGE: *out = 0; break;
        default: return -ENOENT;
        }
        return 0;
    }
    static int perform(ANativeWindow*, int op, ...) {
        va_list args; va_start(args, op);
        int result = 0;
        switch (op) {
        case NATIVE_WINDOW_SET_BUFFER_COUNT:
            result = va_arg(args, size_t) == 3 ? 0 : -EINVAL; break;
        case NATIVE_WINDOW_SET_USAGE64:
            // Buffers deliberately retain linear CPU-readable usage for the consumer.
            result = (va_arg(args, uint64_t) & ~uint64_t(GRALLOC_USAGE_HW_RENDER |
                GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_SW_READ_OFTEN |
                GRALLOC_USAGE_SW_WRITE_OFTEN)) ? -ENOTSUP : 0; break;
        case NATIVE_WINDOW_API_CONNECT: case NATIVE_WINDOW_API_DISCONNECT:
            result = va_arg(args, int) == NATIVE_WINDOW_API_EGL ? 0 : -EINVAL; break;
        default: result = -ENOENT;
        }
        va_end(args); return result;
    }
    static int interval(ANativeWindow*, int value) { return value == 0 ? 0 : -ENOTSUP; }
    static int dequeue(ANativeWindow* w, ANativeWindowBuffer** out, int* fence) {
        Pool* p = self(w);
        if (!out || !fence || p->failed) return -EINVAL;
        for (int i = 0; i < 3; ++i) {
            int n = (p->cursor + i) % 3;
            Slot& s = p->slots[n];
            if (!s.dequeued && !s.unsafeReuse) {
                s.dequeued = true; p->cursor = (n + 1) % 3;
                *out = &s.buffer; *fence = -1;
                std::printf("WINDOW dequeue slot=%d stride=%d\n", n, s.buffer.stride);
                return 0;
            }
        }
        return -EBUSY;
    }
    static int consumeFence(int fd) {
        if (fd < 0) return fd == -1 ? 0 : -EINVAL;
        int result = sync_wait(fd, 5000); int saved = errno; close(fd);
        return result ? -saved : 0;
    }
    int slotIndex(ANativeWindowBuffer* b) {
        for (int i = 0; i < 3; ++i) if (&slots[i].buffer == b) return i;
        return -1;
    }
    static int cancel(ANativeWindow* w, ANativeWindowBuffer* b, int fd) {
        Pool* p = self(w); int n = p->slotIndex(b);
        int result = consumeFence(fd);
        if (result) { p->failed = true; return result; }
        if (n < 0 || !p->slots[n].dequeued || p->slots[n].unsafeReuse) return -EINVAL;
        p->slots[n].dequeued = false; return 0;
    }
    static int queue(ANativeWindow* w, ANativeWindowBuffer* b, int fd) {
        Pool* p = self(w); int n = p->slotIndex(b);
        double begin = now(); int result = consumeFence(fd); double fenceEnd = now();
        if (result || n < 0 || !p->slots[n].dequeued) {
            if (result && n >= 0) p->slots[n].unsafeReuse = true;
            p->failed = true; return result ? result : -EINVAL;
        }
        void* pixels = nullptr;
        result = p->module->lock(p->module, b->handle, GRALLOC_USAGE_SW_READ_OFTEN,
                                0, 0, width, height, &pixels);
        double mapEnd = now();
        if (result || !pixels) {
            if (!result) p->module->unlock(p->module, b->handle);
            p->failed = true; return result ? result : -EFAULT;
        }
        const unsigned char colors[4][4] = {{0,0,255,255},{0,255,0,255},
                                          {255,0,0,255},{255,255,255,255}}; // BGRA
        // Android's window drawable is top-origin; shader compensates GL Y.
        for (int q = 0; q < 4; ++q) {
            const int x = (q % 2) * (width / 2) + width / 4;
            const int y = (q / 2) * (height / 2) + height / 4;
            const auto* pixel = static_cast<const unsigned char*>(pixels) +
                (size_t(y) * b->stride + x) * 4;
            const auto* expected = colors[(q + p->expectedFrame) % 4];
            if (std::memcmp(pixel, expected, 4)) {
                std::fprintf(stderr, "WINDOW mismatch frame=%d q=%d BGRA=%u,%u,%u,%u\n",
                    p->expectedFrame, q, pixel[0], pixel[1], pixel[2], pixel[3]);
                result = -EIO;
            }
        }
        double checkEnd = now();
        // Measure copy to ordinary RAM only: not a scanout bandwidth claim.
        double standardMs = 0, streamingMs = 0;
        auto standardCopy = [&]() {
            double beginCopy = now();
            superman_copy_rows(p->sink.data(), size_t(width) * 4,
                           static_cast<const unsigned char*>(pixels), size_t(b->stride) * 4,
                           size_t(width) * 4, height);
            standardMs = now() - beginCopy;
        };
        auto streamingCopy = [&]() {
            double beginCopy = now();
            superman_copy_rows_streaming(p->streamingSink.data(), size_t(width) * 4,
                           static_cast<const unsigned char*>(pixels), size_t(b->stride) * 4,
                           size_t(width) * 4, height);
            streamingMs = now() - beginCopy;
        };
        // Alternate order to avoid giving one algorithm every warmed second pass.
        if (p->queued % 2) { streamingCopy(); standardCopy(); }
        else { standardCopy(); streamingCopy(); }
        double copiesEnd = now();
        if (std::memcmp(p->sink.data(), p->streamingSink.data(), p->sink.size())) result = -EIO;
        double compareEnd = now();
        int unlock = p->module->unlock(p->module, b->handle);
        if (!result) result = unlock;
        std::printf("WINDOW frame=%d slot=%d fence_ms=%.3f map_ms=%.3f check_ms=%.3f ram_copy_ms=%.3f streaming_copy_ms=%.3f compare_ms=%.3f unlock_ms=%.3f result=%d\n",
            p->expectedFrame, n, fenceEnd-begin, mapEnd-fenceEnd, checkEnd-mapEnd,
            standardMs, streamingMs, compareEnd-copiesEnd, now()-compareEnd, result);
        if (result) { p->failed = true; return result; }
        p->slots[n].dequeued = false; ++p->queued; return 0;
    }
    int initialize(const gralloc_module_t* m) {
        module = m; window.oem[0] = reinterpret_cast<intptr_t>(this);
        window.common.incRef = inc; window.common.decRef = dec;
        window.query = query; window.perform = perform; window.setSwapInterval = interval;
        window.dequeueBuffer = dequeue; window.queueBuffer = queue; window.cancelBuffer = cancel;
        sink.resize(size_t(width) * height * 4);
        streamingSink.resize(sink.size());
        hw_device_t* dev = nullptr;
        int error = module->common.methods->open(&module->common, GRALLOC_HARDWARE_GPU0, &dev);
        if (error || !dev) return error ? error : -ENODEV;
        allocator = reinterpret_cast<alloc_device_t*>(dev);
        for (Slot& s : slots) {
            s.buffer.width = width; s.buffer.height = height;
            s.buffer.layerCount = 1; s.buffer.format = HAL_PIXEL_FORMAT_BGRA_8888;
            s.buffer.usage = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE |
                            GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;
            s.buffer.usage_deprecated = static_cast<int>(s.buffer.usage);
            s.buffer.common.incRef = Slot::inc; s.buffer.common.decRef = Slot::dec;
            error = allocator->alloc(allocator, width, height, s.buffer.format,
                s.buffer.usage_deprecated, &s.buffer.handle, &s.buffer.stride);
            if (error || !s.buffer.handle || s.buffer.stride < width) return error ? error : -EINVAL;
        }
        return 0;
    }
    int cleanup() {
        int result = refs ? -EBUSY : 0;
        for (Slot& s : slots) {
            if (s.refs || s.dequeued || s.unsafeReuse) result = -EBUSY;
            // Never free a buffer still retained by Mesa or an unsignalled fence.
            if (s.buffer.handle && !s.refs && !s.dequeued && !s.unsafeReuse)
                if (allocator->free(allocator, s.buffer.handle)) result = -EIO;
        }
        if (allocator && result != -EBUSY && allocator->common.close(&allocator->common)) result = -EIO;
        return result;
    }
};
int main(int argc, char** argv) {
    if ((argc != 2 && argc != 4) || std::strcmp(argv[1], "--run-six-frames") ||
        (argc == 4 && (std::strcmp(argv[2], "--mesa-library") ||
         std::strncmp(argv[3], "/data/local/tmp/", 16)))) {
        std::fprintf(stderr, "Usage: %s --run-six-frames [--mesa-library /data/local/tmp/.../libGLES_mesa.so] (Android EGL, no scanout)\n", argv[0]); return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!copySelfTest()) return 1;
    void* hal = dlopen("/vendor/lib/hw/gralloc.superman_drm.so", RTLD_NOW | RTLD_LOCAL);
    const char* mesaPath = argc == 4 ? argv[3] : "/vendor/lib/egl/libGLES_mesa.so";
    std::printf("WINDOW Mesa_library=%s bounded_frames=6 no_scanout=true\n", mesaPath);
    void* mesa = dlopen(mesaPath, RTLD_NOW | RTLD_LOCAL);
    if (!hal || !mesa) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
#define LOAD(name) auto name = reinterpret_cast<decltype(&::name)>(dlsym(mesa, #name)); if (!name) return 1;
    LOAD(eglGetProcAddress) LOAD(eglInitialize) LOAD(eglGetConfigs) LOAD(eglGetConfigAttrib)
    LOAD(eglBindAPI) LOAD(eglCreateContext) LOAD(eglCreateWindowSurface) LOAD(eglMakeCurrent)
    LOAD(eglSwapBuffers) LOAD(eglSwapInterval) LOAD(eglDestroySurface) LOAD(eglDestroyContext)
    LOAD(eglTerminate) LOAD(eglGetError)
#undef LOAD
#define GL(name) auto name = reinterpret_cast<decltype(&::name)>(eglGetProcAddress(#name)); if (!name) return 1;
    GL(glGetString) GL(glGetError) GL(glCreateShader) GL(glShaderSource) GL(glCompileShader)
    GL(glGetShaderiv) GL(glGetShaderInfoLog) GL(glDeleteShader) GL(glCreateProgram)
    GL(glAttachShader) GL(glLinkProgram) GL(glGetProgramiv) GL(glDeleteProgram) GL(glUseProgram)
    GL(glGetAttribLocation) GL(glVertexAttribPointer) GL(glEnableVertexAttribArray)
    GL(glDisableVertexAttribArray) GL(glGetUniformLocation) GL(glUniform1f) GL(glViewport) GL(glDrawArrays)
#undef GL
    auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    auto* module = reinterpret_cast<gralloc_module_t*>(dlsym(hal, HAL_MODULE_INFO_SYM_AS_STR));
    if (!platform || !module || !module->lock || !module->unlock) return 1;
    Pool pool; EGLDisplay display = EGL_NO_DISPLAY; EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT; bool initialized = false, current = false;
    GLuint vs = 0, fs = 0, program = 0;
    int result = [&]() {
        if (pool.initialize(module)) return 1;
        display = platform(EGL_PLATFORM_ANDROID_KHR, EGL_DEFAULT_DISPLAY, nullptr);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return 1;
        initialized = true;
        EGLConfig configs[128], config = nullptr; EGLint count = 0;
        if (!eglGetConfigs(display, configs, 128, &count)) return 1;
        for (int i = 0; i < count; ++i) {
            EGLint visual = 0, type = 0, render = 0;
            eglGetConfigAttrib(display, configs[i], EGL_NATIVE_VISUAL_ID, &visual);
            eglGetConfigAttrib(display, configs[i], EGL_SURFACE_TYPE, &type);
            eglGetConfigAttrib(display, configs[i], EGL_RENDERABLE_TYPE, &render);
            if (visual == HAL_PIXEL_FORMAT_BGRA_8888 && (type & EGL_WINDOW_BIT) && (render & EGL_OPENGL_ES2_BIT)) { config = configs[i]; break; }
        }
        if (!config || !eglBindAPI(EGL_OPENGL_ES_API)) { std::fprintf(stderr, "No BGRA Android-window config\n"); return 1; }
        const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, attrs);
        surface = eglCreateWindowSurface(display, config, &pool.window, nullptr);
        if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || !eglMakeCurrent(display, surface, surface, context)) return 1;
        current = true;
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        std::printf("WINDOW GL_RENDERER=%s dimensions=720x1280 buffers=3 format=BGRA8888\n", renderer ? renderer : "null");
        if (!renderer || !std::strstr(renderer, "FD305") || !eglSwapInterval(display, 0)) return 1;
        auto compile = [&](GLenum type, const char* src) {
            GLuint shader = glCreateShader(type); glShaderSource(shader, 1, &src, nullptr); glCompileShader(shader);
            GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (!ok) { char log[1024] = {}; glGetShaderInfoLog(shader, sizeof(log), nullptr, log); std::fprintf(stderr, "%s\n", log); glDeleteShader(shader); return GLuint(0); }
            return shader;
        };
        vs = compile(GL_VERTEX_SHADER, "attribute vec2 p; void main(){gl_Position=vec4(p,0.,1.);}");
        fs = compile(GL_FRAGMENT_SHADER, "precision highp float; uniform float frame; void main(){float q=step(360.,gl_FragCoord.x)+2.*step(640.,1280.-gl_FragCoord.y); q=mod(q+frame,4.); vec3 c=q<.5?vec3(1,0,0):q<1.5?vec3(0,1,0):q<2.5?vec3(0,0,1):vec3(1,1,1); gl_FragColor=vec4(c,1.);}");
        if (!vs || !fs) return 1;
        program = glCreateProgram(); glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program);
        GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked); if (!linked) return 1;
        glUseProgram(program); GLint position = glGetAttribLocation(program, "p"), frame = glGetUniformLocation(program, "frame");
        if (position < 0 || frame < 0) return 1;
        const GLfloat vertices[] = {-1,-1, 3,-1, -1,3};
        glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, vertices); glEnableVertexAttribArray(position);
        glViewport(0, 0, Pool::width, Pool::height);
        for (int i = 0; i < 6; ++i) {
            pool.expectedFrame = i % 4; double begin = now(); glUniform1f(frame, float(i % 4)); glDrawArrays(GL_TRIANGLES, 0, 3);
            if (glGetError() != GL_NO_ERROR || !eglSwapBuffers(display, surface) || pool.failed || pool.queued != i+1) return 1;
            std::printf("WINDOW swap frame=%d wall_ms=%.3f pixel_check=pass\n", i, now()-begin);
        }
        glDisableVertexAttribArray(position); return 0;
    }();
    if (result) std::fprintf(stderr, "WINDOW failed EGL_error=0x%x GL_error=0x%x queued=%d\n", eglGetError(), current ? glGetError() : 0, pool.queued);
    if (current) { if (program) glDeleteProgram(program); if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }
    if (surface != EGL_NO_SURFACE && !eglDestroySurface(display, surface)) result = 1;
    if (context != EGL_NO_CONTEXT && !eglDestroyContext(display, context)) result = 1;
    if (initialized && !eglTerminate(display)) result = 1;
    if (pool.cleanup()) result = 1;
    std::printf("WINDOW result=%s frames=%d no_scanout=true no_fps_claim=true\n", result ? "FAIL" : "PASS", pool.queued);
    return result;
}
