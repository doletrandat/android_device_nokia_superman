// SPDX-License-Identifier: Apache-2.0
// Explicit offscreen test. Never selects a renderer for Android services.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <chrono>

static void checkpoint(const char* stage) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const double ms = std::chrono::duration<double, std::milli>(now).count();
    std::printf("SUPERMAN_GPU_TEST monotonic_ms=%.3f stage=%s\n", ms, stage);
}

int main(int argc, char** argv) {
    int width = 64, height = 64;
    if (argc == 2 && std::strcmp(argv[1], "--full-resolution") == 0) {
        width = 720; height = 1280;
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [--full-resolution]\n", argv[0]);
        return 2;
    }
    std::printf("Offscreen dimensions=%dx%d; one bounded draw\n", width, height);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    checkpoint("mesa_load_begin");
    void* library = dlopen("/vendor/lib/egl/libGLES_mesa.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fprintf(stderr, "Mesa load failed: %s\n", dlerror()); return 1; }
#define LOAD(name) auto name = reinterpret_cast<decltype(&::name)>(dlsym(library, #name)); \
    if (!name) { std::fprintf(stderr, "Missing %s\n", #name); return 1; }
    LOAD(eglGetProcAddress)
    LOAD(eglInitialize)
    LOAD(eglChooseConfig)
    LOAD(eglBindAPI)
    LOAD(eglCreateContext)
    LOAD(eglCreatePbufferSurface)
    LOAD(eglMakeCurrent)
    LOAD(eglDestroySurface)
    LOAD(eglDestroyContext)
    LOAD(eglTerminate)
#undef LOAD
    checkpoint("egl_symbols_loaded");
    auto platformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!platformDisplay) { std::fprintf(stderr, "No platform display extension\n"); return 1; }
    checkpoint("egl_platform_display_begin");
    EGLDisplay display = platformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    checkpoint("egl_initialize_begin");
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
        std::fprintf(stderr, "Cannot initialize Mesa surfaceless display\n"); return 1;
    }
    checkpoint("egl_initialize_end");
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    checkpoint("egl_choose_config_begin");
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1 ||
        !eglBindAPI(EGL_OPENGL_ES_API)) { eglTerminate(display); return 1; }
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const EGLint surfaceAttributes[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    checkpoint("egl_create_context_begin");
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    checkpoint("egl_create_context_end_create_surface_begin");
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
    checkpoint("egl_create_surface_end_make_current_begin");
    int result = 1;
    if (context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE &&
        eglMakeCurrent(display, surface, surface, context)) {
#define GL_LOAD(name) auto name = reinterpret_cast<decltype(&::name)>(eglGetProcAddress(#name));
        GL_LOAD(glGetString)
        GL_LOAD(glClearColor)
        GL_LOAD(glClear)
        GL_LOAD(glReadPixels)
        GL_LOAD(glGetError)
        GL_LOAD(glCreateShader)
        GL_LOAD(glShaderSource)
        GL_LOAD(glCompileShader)
        GL_LOAD(glGetShaderiv)
        GL_LOAD(glGetShaderInfoLog)
        GL_LOAD(glDeleteShader)
        GL_LOAD(glCreateProgram)
        GL_LOAD(glAttachShader)
        GL_LOAD(glBindAttribLocation)
        GL_LOAD(glLinkProgram)
        GL_LOAD(glGetProgramiv)
        GL_LOAD(glGetProgramInfoLog)
        GL_LOAD(glUseProgram)
        GL_LOAD(glDeleteProgram)
        GL_LOAD(glGenBuffers)
        GL_LOAD(glBindBuffer)
        GL_LOAD(glBufferData)
        GL_LOAD(glDeleteBuffers)
        GL_LOAD(glVertexAttribPointer)
        GL_LOAD(glEnableVertexAttribArray)
        GL_LOAD(glViewport)
        GL_LOAD(glDrawArrays)
#undef GL_LOAD
        if (glGetString && glClearColor && glClear && glReadPixels && glGetError &&
            glCreateShader && glShaderSource && glCompileShader && glGetShaderiv &&
            glGetShaderInfoLog && glDeleteShader && glCreateProgram && glAttachShader &&
            glBindAttribLocation && glLinkProgram && glGetProgramiv && glGetProgramInfoLog &&
            glUseProgram && glDeleteProgram && glGenBuffers && glBindBuffer && glBufferData &&
            glDeleteBuffers && glVertexAttribPointer && glEnableVertexAttribArray &&
            glViewport && glDrawArrays) {
            checkpoint("egl_make_current_end_renderer_query");
            const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            std::printf("GL_RENDERER=%s\n", renderer ? renderer : "null");
            // A software renderer must never produce an acceleration PASS.
            if (renderer && (std::strstr(renderer, "FD305") ||
                             (std::strstr(renderer, "Adreno") && std::strstr(renderer, "305")))) {
                unsigned char pixel[4] = {};
                auto start = std::chrono::steady_clock::now();
                checkpoint("clear_begin");
                glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                checkpoint("clear_readback_begin");
                glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                checkpoint("clear_readback_end");
                double milliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                if (glGetError() == GL_NO_ERROR && pixel[0] == 255 &&
                    pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255) {
                    std::printf("Clear/readback verified (%.3f ms)\n", milliseconds);
                    checkpoint("shader_compile_begin");
                    auto compileShader = [&](GLenum type, const char* source) -> GLuint {
                        GLuint shader = glCreateShader(type);
                        if (!shader) return 0;
                        glShaderSource(shader, 1, &source, nullptr);
                        glCompileShader(shader);
                        GLint compiled = GL_FALSE;
                        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                        if (compiled != GL_TRUE) {
                            char log[1024] = {};
                            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
                            std::fprintf(stderr, "Shader compile failed: %s\n", log);
                            glDeleteShader(shader);
                            return 0;
                        }
                        return shader;
                    };
                    GLuint vertex = compileShader(GL_VERTEX_SHADER,
                        "attribute vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }");
                    GLuint fragment = compileShader(GL_FRAGMENT_SHADER,
                        "precision mediump float; void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }");
                    checkpoint("shader_compile_end_program_begin");
                    GLuint program = glCreateProgram();
                    if (vertex && fragment && program) {
                        glAttachShader(program, vertex);
                        glAttachShader(program, fragment);
                        glBindAttribLocation(program, 0, "position");
                        checkpoint("program_link_begin");
                        glLinkProgram(program);
                        checkpoint("program_link_end");
                        GLint linked = GL_FALSE;
                        glGetProgramiv(program, GL_LINK_STATUS, &linked);
                        if (linked == GL_TRUE) {
                            const GLfloat triangle[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
                            GLuint buffer = 0;
                            checkpoint("buffer_upload_begin");
                            glGenBuffers(1, &buffer);
                            glBindBuffer(GL_ARRAY_BUFFER, buffer);
                            glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
                            checkpoint("buffer_upload_end");
                            glUseProgram(program);
                            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
                            glEnableVertexAttribArray(0);
                            glViewport(0, 0, width, height);
                            auto drawStart = std::chrono::steady_clock::now();
                            checkpoint("triangle_draw_begin");
                            glDrawArrays(GL_TRIANGLES, 0, 3);
                            checkpoint("triangle_readback_begin");
                            bool samplesValid = true;
                            const int points[][2] = {{1, 1}, {width - 2, 1},
                                {1, height - 2}, {width - 2, height - 2},
                                {width / 2, height / 2}};
                            for (const auto& point : points) {
                                glReadPixels(point[0], point[1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                                samplesValid &= pixel[0] == 0 && pixel[1] == 255 &&
                                    pixel[2] == 0 && pixel[3] == 255;
                            }
                            checkpoint("triangle_readback_end");
                            double drawMs = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - drawStart).count();
                            GLenum error = glGetError();
                            if (buffer && samplesValid && error == GL_NO_ERROR && pixel[0] == 0 &&
                                pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255) {
                                std::printf("PASS: Adreno 305 shader draw and green-pixel readback (%.3f ms); not an FPS benchmark\n", drawMs);
                                result = 0;
                            } else {
                                std::fprintf(stderr, "Draw/readback failed: error=0x%x RGBA=%u,%u,%u,%u\n",
                                    error, pixel[0], pixel[1], pixel[2], pixel[3]);
                            }
                            checkpoint("gl_cleanup_begin");
                            glBindBuffer(GL_ARRAY_BUFFER, 0);
                            if (buffer) glDeleteBuffers(1, &buffer);
                            glUseProgram(0);
                        } else {
                            char log[1024] = {};
                            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
                            std::fprintf(stderr, "Program link failed: %s\n", log);
                        }
                    }
                    if (program) glDeleteProgram(program);
                    if (vertex) glDeleteShader(vertex);
                    if (fragment) glDeleteShader(fragment);
                }
            } else { std::fprintf(stderr, "Refusing renderer other than Adreno 305\n"); }
        }
    }
    checkpoint("egl_cleanup_begin");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    eglTerminate(display);
    dlclose(library);
    checkpoint("cleanup_end");
    return result;
}
