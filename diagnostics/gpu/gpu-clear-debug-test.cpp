// SPDX-License-Identifier: Apache-2.0
// Explicit private FBO test: no native window, composer, DRM master or scanout.
#include "offscreen-oracle.h"
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
    constexpr char text[] = "OFFSCREEN watchdog_expired; no correctness PASS\n";
    (void)write(STDERR_FILENO, text, sizeof(text) - 1);
    _exit(124);
}
void stage(const char* name) {
    timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t);
    std::printf("OFFSCREEN mono_ns=%llu stage=%s\n",
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
    GL_ENTRY(glPixelStorei) GL_ENTRY(glDrawArrays) GL_ENTRY(glReadPixels)
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
        GL_LOAD(glPixelStorei) GL_LOAD(glDrawArrays) GL_LOAD(glReadPixels)
#undef GL_LOAD
        return true;
    }
    bool error(const char* where) {
        const GLenum e = glGetError();
        if (e) std::fprintf(stderr, "OFFSCREEN GL_ERROR stage=%s error=0x%x\n", where, e);
        return e != GL_NO_ERROR;
    }
    GLuint shader(GLenum type, const char* source) {
        GLuint s = glCreateShader(type); if (!s) return 0;
        glShaderSource(s, 1, &source, nullptr); glCompileShader(s);
        GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[2048]{}; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            std::fprintf(stderr, "OFFSCREEN shader_failed=%s\n", log); glDeleteShader(s); return 0; }
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
                std::fprintf(stderr, "OFFSCREEN link_failed=%s\n", log); glDeleteProgram(p); p = 0; }
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
    if (argc != 3 || std::strcmp(argv[1], "--run") ||
        (std::strcmp(argv[2], "default") && std::strcmp(argv[2], "gmem") && std::strcmp(argv[2], "nogmem"))) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0); std::setvbuf(stderr, nullptr, _IONBF, 0);
    if (!board()) return 3;
    const char* reject[] = {"IR3_SHADER_DEBUG", "FD_MESA_GMEM", "GALLIUM_DRIVER", "MESA_LOADER_DRIVER_OVERRIDE"};
    for (const auto* key : reject) if (std::getenv(key)) return 3;
    const char* flags = !std::strcmp(argv[2], "gmem") ? "bstat,nobypass" :
        !std::strcmp(argv[2], "nogmem") ? "bstat,nogmem" : "bstat";
    if (setenv("FD_MESA_DEBUG", flags, 1)) return 3;
    struct sigaction sa{}; sa.sa_handler = watchdog; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, nullptr)) return 3;
    alarm(20);
    std::printf("CLEAR_DEBUG mode=%s FD_MESA_DEBUG=%s width=720 height=48 contexts=2 iterations=24 watchdog_s=20\n", argv[2], flags);
    stage("clear_debug_mesa_load");
    void* mesa = dlopen("/vendor/lib/egl/libGLES_mesa.so", RTLD_NOW | RTLD_LOCAL);
    if (!mesa) return 4;
#define EGL_LOAD(name) auto name = reinterpret_cast<decltype(&::name)>(dlsym(mesa, #name)); if (!name) return 4;
    EGL_LOAD(eglGetProcAddress) EGL_LOAD(eglInitialize) EGL_LOAD(eglChooseConfig) EGL_LOAD(eglBindAPI)
    EGL_LOAD(eglCreateContext) EGL_LOAD(eglCreatePbufferSurface) EGL_LOAD(eglMakeCurrent)
    EGL_LOAD(eglDestroyContext) EGL_LOAD(eglDestroySurface) EGL_LOAD(eglTerminate)
#undef EGL_LOAD
    auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!platform) return 4;
    EGLDisplay display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return 4;
    EGLContext contexts[2] = {EGL_NO_CONTEXT, EGL_NO_CONTEXT};
    EGLSurface surfaces[2] = {EGL_NO_SURFACE, EGL_NO_SURFACE};
    GLuint textures[2]{}, fbos[2]{}; Gl g; bool ready = false;
    constexpr int width=720, height=48;
    unsigned checked=0;
    int rc = [&]() -> int {
        const EGLint cfg[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
        EGLConfig config=nullptr; EGLint count=0;
        if (!eglChooseConfig(display,cfg,&config,1,&count) || count!=1 || !eglBindAPI(EGL_OPENGL_ES_API)) return 4;
        const EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE}, pb[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
        for(int i=0;i<2;i++) {
            contexts[i]=eglCreateContext(display,config,EGL_NO_CONTEXT,ctx);
            surfaces[i]=eglCreatePbufferSurface(display,config,pb);
            if(contexts[i]==EGL_NO_CONTEXT || surfaces[i]==EGL_NO_SURFACE ||
               !eglMakeCurrent(display,surfaces[i],surfaces[i],contexts[i])) return 4;
            if(!ready) { if(!g.load(eglGetProcAddress)) return 4; ready=true; }
            const char* renderer=reinterpret_cast<const char*>(g.glGetString(GL_RENDERER));
            std::printf("CLEAR_DEBUG context=%d renderer=%s\n",i,renderer?renderer:"null");
            if(!renderer || std::strcmp(renderer,"FD305")) return 3;
            g.glGenTextures(1,&textures[i]);g.glGenFramebuffers(1,&fbos[i]);
            if(!textures[i] || !fbos[i]) return 4;
            g.texture(textures[i]);g.glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            g.glBindFramebuffer(GL_FRAMEBUFFER,fbos[i]);
            g.glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[i],0);
            if(g.glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return 4;
            g.glViewport(0,0,width,height);g.glDisable(GL_DITHER);g.glDisable(GL_BLEND);
            g.glPixelStorei(GL_PACK_ALIGNMENT,1);
            if(g.error("setup")) return 4;
        }
        std::vector<unsigned char> pixels(width*height*4);
        for(int iter=0;iter<24;iter++) {
            int i=iter%2;
            if(!eglMakeCurrent(display,surfaces[i],surfaces[i],contexts[i])) return 4;
            g.glBindFramebuffer(GL_FRAMEBUFFER,fbos[i]);
            // End-point colors avoid conversion-rounding ambiguity. Alternate context and reuse target.
            unsigned char bg[4]={static_cast<unsigned char>((iter&1)?255:0),static_cast<unsigned char>((iter&2)?255:0),static_cast<unsigned char>((iter&4)?255:0),255};
            g.glDisable(GL_SCISSOR_TEST);g.glClearColor(bg[0]/255.f,bg[1]/255.f,bg[2]/255.f,1.f);g.glClear(GL_COLOR_BUFFER_BIT);
            bool partial=(iter%3)!=0;
            if(partial) {
                g.glEnable(GL_SCISSOR_TEST);g.glScissor(17,7,683,33);
                g.glClearColor(1.f-bg[0]/255.f,1.f-bg[1]/255.f,1.f-bg[2]/255.f,1.f);g.glClear(GL_COLOR_BUFFER_BIT);g.glDisable(GL_SCISSOR_TEST);
            }
            g.glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            if(g.error("readback")) return 4;
            for(int y=0;y<height;y++)for(int x=0;x<width;x++)for(int c=0;c<4;c++) {
                unsigned char expected=bg[c];
                if(c<3 && partial && x>=17 && x<700 && y>=7 && y<40) expected=255-expected;
                unsigned char got=pixels[(y*width+x)*4+c];
                if(got!=expected) {std::printf("CLEAR_DEBUG MISMATCH iter=%d x=%d y=%d channel=%d got=%u expected=%u\n",iter,x,y,c,got,expected);return 5;}
            }
            checked+=width*height;
            std::printf("CLEAR_DEBUG iteration=%d context=%d partial=%d pixels_exact=%d\n",iter,i,partial,width*height);
        }
        return 0;
    }();
    for(int i=0;i<2;i++) {
        if(contexts[i]!=EGL_NO_CONTEXT && surfaces[i]!=EGL_NO_SURFACE && ready) {
            if(eglMakeCurrent(display,surfaces[i],surfaces[i],contexts[i])) {
                if(fbos[i])g.glDeleteFramebuffers(1,&fbos[i]);if(textures[i])g.glDeleteTextures(1,&textures[i]);
            } else if(!rc)rc=4;
        }
    }
    if(!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT) && !rc)rc=4;
    for(int i=0;i<2;i++) {
        if(contexts[i]!=EGL_NO_CONTEXT && !eglDestroyContext(display,contexts[i]) && !rc)rc=4;
        if(surfaces[i]!=EGL_NO_SURFACE && !eglDestroySurface(display,surfaces[i]) && !rc)rc=4;
    }
    if(!eglTerminate(display) && !rc)rc=4;
    dlclose(mesa);alarm(0);
    std::printf("CLEAR_DEBUG exit=%d pixels_checked=%u cleanup_complete=1\n",rc,checked);
    return rc;
}
