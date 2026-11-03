// SPDX-License-Identifier: Apache-2.0
// Explicit private FBO test: no native window, composer, DRM master or scanout.
#include "offscreen-oracle.h"
#include "context-cycle-ipc.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void watchdog(int) {
    constexpr char text[] = "GPU_CYCLE watchdog_expired; no correctness PASS\n";
    (void)write(STDERR_FILENO, text, sizeof(text) - 1);
    _exit(124);
}
void stage(const char* name) {
    timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t);
    std::printf("GPU_CYCLE mono_ns=%llu stage=%s\n",
        static_cast<unsigned long long>(t.tv_sec) * 1000000000ull + t.tv_nsec, name);
}
bool board() {
    char data[128]{};
    const int fd = open("/proc/device-tree/compatible", O_RDONLY | O_CLOEXEC);
    const ssize_t n = fd < 0 ? -1 : read(fd, data, sizeof(data));
    if (fd >= 0) close(fd);
    constexpr char match[] = "nokia,superman\0qcom,msm8226\0";
    return n == static_cast<ssize_t>(sizeof(match) - 1) && !std::memcmp(data, match, sizeof(match) - 1);
}
struct Gl {
#define GL_ENTRY(name) decltype(&::name) name = nullptr;
    GL_ENTRY(glGetString) GL_ENTRY(glGetError) GL_ENTRY(glGenTextures)
    GL_ENTRY(glBindTexture) GL_ENTRY(glTexImage2D) GL_ENTRY(glTexSubImage2D)
    GL_ENTRY(glTexParameteri) GL_ENTRY(glDeleteTextures) GL_ENTRY(glActiveTexture)
    GL_ENTRY(glGenFramebuffers) GL_ENTRY(glBindFramebuffer) GL_ENTRY(glFramebufferTexture2D)
    GL_ENTRY(glCheckFramebufferStatus) GL_ENTRY(glDeleteFramebuffers)
    GL_ENTRY(glCreateShader) GL_ENTRY(glShaderSource) GL_ENTRY(glCompileShader)
    GL_ENTRY(glGetShaderiv) GL_ENTRY(glGetShaderInfoLog) GL_ENTRY(glDeleteShader)
    GL_ENTRY(glCreateProgram) GL_ENTRY(glAttachShader) GL_ENTRY(glBindAttribLocation)
    GL_ENTRY(glLinkProgram) GL_ENTRY(glGetProgramiv) GL_ENTRY(glGetProgramInfoLog)
    GL_ENTRY(glDeleteProgram) GL_ENTRY(glUseProgram) GL_ENTRY(glGetUniformLocation)
    GL_ENTRY(glUniform1i) GL_ENTRY(glUniform3f) GL_ENTRY(glGenBuffers)
    GL_ENTRY(glBindBuffer) GL_ENTRY(glBufferData) GL_ENTRY(glDeleteBuffers)
    GL_ENTRY(glVertexAttribPointer) GL_ENTRY(glEnableVertexAttribArray)
    GL_ENTRY(glViewport) GL_ENTRY(glEnable) GL_ENTRY(glDisable) GL_ENTRY(glScissor)
    GL_ENTRY(glBlendFunc) GL_ENTRY(glClearColor) GL_ENTRY(glClear)
    GL_ENTRY(glPixelStorei) GL_ENTRY(glDrawArrays) GL_ENTRY(glReadPixels) GL_ENTRY(glFlush)
#undef GL_ENTRY
    bool load(decltype(&::eglGetProcAddress) get) {
#define GL_LOAD(name) name = reinterpret_cast<decltype(name)>(get(#name)); if (!name) return false;
        GL_LOAD(glGetString) GL_LOAD(glGetError) GL_LOAD(glGenTextures)
        GL_LOAD(glBindTexture) GL_LOAD(glTexImage2D) GL_LOAD(glTexSubImage2D)
        GL_LOAD(glTexParameteri) GL_LOAD(glDeleteTextures) GL_LOAD(glActiveTexture)
        GL_LOAD(glGenFramebuffers) GL_LOAD(glBindFramebuffer) GL_LOAD(glFramebufferTexture2D)
        GL_LOAD(glCheckFramebufferStatus) GL_LOAD(glDeleteFramebuffers)
        GL_LOAD(glCreateShader) GL_LOAD(glShaderSource) GL_LOAD(glCompileShader)
        GL_LOAD(glGetShaderiv) GL_LOAD(glGetShaderInfoLog) GL_LOAD(glDeleteShader)
        GL_LOAD(glCreateProgram) GL_LOAD(glAttachShader) GL_LOAD(glBindAttribLocation)
        GL_LOAD(glLinkProgram) GL_LOAD(glGetProgramiv) GL_LOAD(glGetProgramInfoLog)
        GL_LOAD(glDeleteProgram) GL_LOAD(glUseProgram) GL_LOAD(glGetUniformLocation)
        GL_LOAD(glUniform1i) GL_LOAD(glUniform3f) GL_LOAD(glGenBuffers)
        GL_LOAD(glBindBuffer) GL_LOAD(glBufferData) GL_LOAD(glDeleteBuffers)
        GL_LOAD(glVertexAttribPointer) GL_LOAD(glEnableVertexAttribArray)
        GL_LOAD(glViewport) GL_LOAD(glEnable) GL_LOAD(glDisable) GL_LOAD(glScissor)
        GL_LOAD(glBlendFunc) GL_LOAD(glClearColor) GL_LOAD(glClear)
        GL_LOAD(glPixelStorei) GL_LOAD(glDrawArrays) GL_LOAD(glReadPixels) GL_LOAD(glFlush)
#undef GL_LOAD
        return true;
    }
    bool error(const char* where) {
        const GLenum e = glGetError();
        if (e) std::fprintf(stderr, "GPU_CYCLE GL_ERROR stage=%s error=0x%x\n", where, e);
        return e != GL_NO_ERROR;
    }
    GLuint shader(GLenum type, const char* source) {
        GLuint s = glCreateShader(type); if (!s) return 0;
        glShaderSource(s, 1, &source, nullptr); glCompileShader(s);
        GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[2048]{}; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            std::fprintf(stderr, "GPU_CYCLE shader_failed=%s\n", log); glDeleteShader(s); return 0; }
        return s;
    }
    GLuint program(bool copy) {
        GLuint vs = shader(GL_VERTEX_SHADER,
            "attribute vec2 p; attribute vec2 uv; varying highp vec2 t;"
            "void main(){gl_Position=vec4(p,0.,1.);t=uv+vec2(0.000244140625);}");
        const char* fsText = copy ?
            "precision mediump float; varying highp vec2 t; uniform sampler2D image;"
            "void main(){gl_FragColor=texture2D(image,t);}" :
            "precision mediump float; varying highp vec2 t; uniform sampler2D image; uniform vec3 color;"
            "void main(){float a=texture2D(image,t).a;gl_FragColor=vec4(color*a,a);}";
        GLuint fs = shader(GL_FRAGMENT_SHADER, fsText);
        GLuint p = vs && fs ? glCreateProgram() : 0;
        if (p) {
            glAttachShader(p, vs); glAttachShader(p, fs);
            glBindAttribLocation(p, 0, "p"); glBindAttribLocation(p, 1, "uv"); glLinkProgram(p);
            GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
            if (!ok) { char log[2048]{}; glGetProgramInfoLog(p, sizeof(log), nullptr, log);
                std::fprintf(stderr, "GPU_CYCLE link_failed=%s\n", log); glDeleteProgram(p); p = 0; }
        }
        if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs);
        return p;
    }
    void texture(GLuint name) {
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
};
} // namespace

int main(int argc, char** argv) {
    const bool worker = argc == 10 && !std::strcmp(argv[1], "--worker") &&
        !std::strcmp(argv[5], "--ipc") && !std::strcmp(argv[8], "--id");
    const bool arguments = ((argc == 5 && !std::strcmp(argv[1], "--run")) || worker) &&
        (!std::strcmp(argv[2], "default") || !std::strcmp(argv[2], "nogmem") || !std::strcmp(argv[2], "nobin")) &&
        !std::strcmp(argv[3], "--size") && (!std::strcmp(argv[4], "small") || !std::strcmp(argv[4], "full"));
    if (!arguments) {
        std::fprintf(stderr, "Usage: %s --run default|nogmem|nobin --size small|full\n", argv[0]); return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0); std::setvbuf(stderr, nullptr, _IONBF, 0);
    if (!board()) { std::fprintf(stderr, "GPU_CYCLE rejected_board\n"); return 3; }
    if (!worker) return cycle::coordinator(argv);
    if (!cycle::init_worker(argv[6], argv[7], argv[9])) return 2;
    std::printf("GPU_CYCLE worker-start worker=%d pid=%d\n", cycle::worker, getpid());
    // Process-local policy only, before Mesa loads or reads its debug options.
    if (std::getenv("IR3_SHADER_DEBUG") || std::getenv("FD_MESA_GMEM") || std::getenv("GALLIUM_DRIVER") ||
        std::getenv("MESA_LOADER_DRIVER_OVERRIDE")) {
        std::fprintf(stderr, "GPU_CYCLE rejected_inherited_gpu_override\n"); return 3;
    }
    if (!std::strcmp(argv[2], "default")) unsetenv("FD_MESA_DEBUG");
    else if (setenv("FD_MESA_DEBUG", argv[2], 1)) return 3;
    struct sigaction sa{}; sa.sa_handler = watchdog; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, nullptr)) return 3;
    alarm(30);
    const bool full = !std::strcmp(argv[4], "full");
    const int width = full ? 720 : 128, height = full ? 1280 : 128;
    const int frames = full ? 12 : 36;
    std::printf("GPU_CYCLE mode=%s width=%d height=%d operations=%d watchdog_s=30 version=2\n", argv[2], width, height, frames);
    stage("mesa_load");
    void* mesa = dlopen("/vendor/lib/egl/libGLES_mesa.so", RTLD_NOW | RTLD_LOCAL);
    if (!mesa) { std::fprintf(stderr, "GPU_CYCLE dlopen_failed=%s\n", dlerror()); return 4; }
#define EGL_LOAD(name) auto name = reinterpret_cast<decltype(&::name)>(dlsym(mesa, #name)); if (!name) return 4;
    EGL_LOAD(eglGetProcAddress) EGL_LOAD(eglInitialize) EGL_LOAD(eglChooseConfig) EGL_LOAD(eglBindAPI)
    EGL_LOAD(eglCreateContext) EGL_LOAD(eglCreatePbufferSurface) EGL_LOAD(eglMakeCurrent)
    EGL_LOAD(eglDestroyContext) EGL_LOAD(eglDestroySurface) EGL_LOAD(eglTerminate) EGL_LOAD(eglGetError)
#undef EGL_LOAD
    auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!platform) return 4;
    EGLDisplay display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
        std::fprintf(stderr, "GPU_CYCLE initialize_failed=0x%x\n", eglGetError()); return 4;
    }
    EGLContext context = EGL_NO_CONTEXT; EGLSurface surface = EGL_NO_SURFACE;
    Gl g; bool glReady = false;
    GLuint textures[5]{}, fbos[4]{}, programs[2]{}, vbo = 0;
    int result = [&]() -> int {
        const EGLint cfg[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLConfig config = nullptr; EGLint count = 0;
        if (!eglChooseConfig(display, cfg, &config, 1, &count) || count != 1 || !eglBindAPI(EGL_OPENGL_ES_API)) return 4;
        const EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx);
        surface = eglCreatePbufferSurface(display, config, pb);
        if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || !eglMakeCurrent(display, surface, surface, context)) return 4;
        if (!g.load(eglGetProcAddress)) return 4; glReady = true;
        const char* renderer = reinterpret_cast<const char*>(g.glGetString(GL_RENDERER));
        const char* version = reinterpret_cast<const char*>(g.glGetString(GL_VERSION));
        std::printf("GPU_CYCLE renderer=%s gl_version=%s\n", renderer ? renderer : "null", version ? version : "null");
        if (!renderer || std::strcmp(renderer, "FD305")) return 3;
        stage("private_buffers_initialize");
        const offscreen::Pixel background{static_cast<uint8_t>(16 + cycle::worker * 16),
            static_cast<uint8_t>(32 + cycle::worker * 16), static_cast<uint8_t>(64 + cycle::worker * 16), 255};
        const size_t pixels = static_cast<size_t>(width) * height;
        std::vector<offscreen::Pixel> expected[3], actual(pixels);
        for (auto& e : expected) e.assign(pixels, background);
        std::vector<uint8_t> atlas(offscreen::kAtlas * offscreen::kAtlas), patch;
        for (int y = 0; y < offscreen::kAtlas; ++y) for (int x = 0; x < offscreen::kAtlas; ++x)
            atlas[y * offscreen::kAtlas + x] = offscreen::glyph(x, y, 0);
        g.glPixelStorei(GL_UNPACK_ALIGNMENT, 1); g.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        g.glActiveTexture(GL_TEXTURE0); g.glGenTextures(5, textures); g.glGenFramebuffers(4, fbos);
        if (!textures[0] || !textures[4] || !fbos[0] || !fbos[3]) return 4;
        g.texture(textures[4]);
        g.glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, offscreen::kAtlas, offscreen::kAtlas, 0, GL_ALPHA, GL_UNSIGNED_BYTE, atlas.data());
        for (int i = 0; i < 4; ++i) {
            g.texture(textures[i]);
            g.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            g.glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
            g.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
            if (g.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 4;
            g.glClearColor(background.r / 255.f, background.g / 255.f, background.b / 255.f, 1.f);
            g.glClear(GL_COLOR_BUFFER_BIT);
        }
        programs[0] = g.program(false); programs[1] = g.program(true);
        if (!programs[0] || !programs[1] || g.error("initialize")) return 4;
        const GLfloat vertices[] = {-1,-1,0,0, 1,-1,1,0, -1,1,0,1, 1,1,1,1};
        g.glGenBuffers(1, &vbo); if (!vbo) return 4;
        g.glBindBuffer(GL_ARRAY_BUFFER, vbo); g.glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        g.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
        g.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
        g.glEnableVertexAttribArray(0); g.glEnableVertexAttribArray(1);
        g.glViewport(0, 0, width, height); g.glDisable(GL_DITHER); g.glDisable(GL_DEPTH_TEST); g.glDisable(GL_STENCIL_TEST);
        const GLint color = g.glGetUniformLocation(programs[0], "color");
        GLint sampler[2] = {g.glGetUniformLocation(programs[0], "image"), g.glGetUniformLocation(programs[1], "image")};
        if (color < 0 || sampler[0] < 0 || sampler[1] < 0 || g.error("geometry")) return 4;
        size_t checked = 0; int readbacks = 0;
        auto verify = [&](int fbo, int expectedIndex, int frame, const char* kind) -> bool {
            stage("private_readback_begin");
            g.glBindFramebuffer(GL_FRAMEBUFFER, fbos[fbo]);
            g.glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
            if (g.error("private_readback")) return false;
            const auto c = offscreen::compare(actual, expected[expectedIndex]);
            checked += c.pixels; ++readbacks;
            std::printf("GPU_CYCLE check worker=%d frame=%d target=%d kind=%s pixels=%zu mismatches=%zu max_error=%d actual_hash=%08x expected_hash=%08x\n",
                cycle::worker, frame, expectedIndex, kind, c.pixels, c.mismatches, c.max_error, offscreen::hash(actual), offscreen::hash(expected[expectedIndex]));
            if (c.mismatches) {
                const auto p = actual[c.first], e = expected[expectedIndex][c.first];
                std::fprintf(stderr, "GPU_CYCLE mismatch x=%zu y=%zu actual=%u,%u,%u,%u expected=%u,%u,%u,%u tolerance=3\n",
                    c.first % width, c.first / width, p.r, p.g, p.b, p.a, e.r, e.g, e.b, e.a);
                return false;
            }
            return true;
        };
        cycle::clients();
        if (!cycle::barrier('R')) return 7;
        stage("update_draw_groups");
        for (int frame = 0; frame < frames; ++frame) {
            const int target = frame % 3, channel = (frame / 3 + cycle::worker) % 3, phase = frame + 1 + 7 * cycle::worker;
            const bool fullUpload = frame % 2 == 0;
            const int x0 = fullUpload ? 0 : (frame * 7) % 49, y0 = fullUpload ? 0 : (frame * 5) % 49;
            const int extent = fullUpload ? offscreen::kAtlas : 16;
            patch.resize(extent * extent);
            for (int y = 0; y < extent; ++y) for (int x = 0; x < extent; ++x) {
                const uint8_t value = offscreen::glyph(x + x0, y + y0, phase);
                patch[y * extent + x] = value; atlas[(y + y0) * offscreen::kAtlas + x + x0] = value;
            }
            g.glBindTexture(GL_TEXTURE_2D, textures[4]);
            g.glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, extent, extent, GL_ALPHA, GL_UNSIGNED_BYTE, patch.data());
            g.glBindFramebuffer(GL_FRAMEBUFFER, fbos[target]);
            const auto rect = offscreen::damage(frame, width, height);
            g.glEnable(GL_SCISSOR_TEST); g.glScissor(rect.x, rect.y, rect.w, rect.h);
            const bool clear = frame % 3 != 1;
            if (clear) g.glClear(GL_COLOR_BUFFER_BIT);
            g.glUseProgram(programs[0]); g.glUniform1i(sampler[0], 0);
            g.glUniform3f(color, channel == 0 ? 1.f : 0.f, channel == 1 ? 1.f : 0.f, channel == 2 ? 1.f : 0.f);
            g.glEnable(GL_BLEND); g.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            g.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); g.glFlush();
            if (g.error("update_draw")) return 4;
            offscreen::apply(expected[target], atlas, width, height, rect, background, channel, clear);
            if (frame % 6 == 5) {
                // Sample a freshly rendered FBO without a finish/readback first.
                g.glBindFramebuffer(GL_FRAMEBUFFER, fbos[3]); g.glDisable(GL_SCISSOR_TEST); g.glDisable(GL_BLEND);
                g.glBindTexture(GL_TEXTURE_2D, textures[target]); g.glUseProgram(programs[1]); g.glUniform1i(sampler[1], 0);
                g.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                if (g.error("render_to_texture")) return 4;
                if (!verify(3, target, frame, "sampled-fbo")) return 5;
                for (int i = 0; i < 3; ++i) if (!verify(i, i, frame, "reused-fbo")) return 5;
                if (!cycle::barrier('I')) return 7;
                for (int i = 0; i < 3; ++i) if (!verify(i, i, frame, "retained-after-idle")) return 5;
                // Submit fresh GPU work after the shared idle interval, using old resources.
                g.glBindFramebuffer(GL_FRAMEBUFFER, fbos[3]);
                g.glBindTexture(GL_TEXTURE_2D, textures[target]); g.glUseProgram(programs[1]);
                g.glUniform1i(sampler[1], 0); g.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); g.glFlush();
                if (g.error("resume_copy") || !verify(3, target, frame, "gpu-copy-after-idle")) return 5;
            }
        }
        std::printf("GPU_CYCLE verified mode=%s operations=%d readbacks=%d pixels=%zu tolerance=3 private_buffers_only=1 not_fps=1\n",
            argv[2], frames, readbacks, checked);
        return 0;
    }();
    stage("cleanup");
    if (glReady) {
        g.glBindFramebuffer(GL_FRAMEBUFFER, 0); g.glBindTexture(GL_TEXTURE_2D, 0); g.glBindBuffer(GL_ARRAY_BUFFER, 0); g.glUseProgram(0);
        if (vbo) g.glDeleteBuffers(1, &vbo);
        g.glDeleteFramebuffers(4, fbos); g.glDeleteTextures(5, textures);
        for (GLuint p : programs) if (p) g.glDeleteProgram(p);
    }
    bool cleanup = eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) cleanup = eglDestroySurface(display, surface) && cleanup;
    if (context != EGL_NO_CONTEXT) cleanup = eglDestroyContext(display, context) && cleanup;
    cleanup = eglTerminate(display) && cleanup;
    if (dlclose(mesa)) cleanup = false;
    if (!cleanup && !result) result = 6;
    alarm(0);
    if (!result) std::printf("GPU_CYCLE PASS worker=%d mode=%s cleanup_complete=1\n", cycle::worker, argv[2]);
    std::printf("GPU_CYCLE exit=%d\n", result);
    return result;
}
