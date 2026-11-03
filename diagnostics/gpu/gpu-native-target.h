// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <ui/GraphicBuffer.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <sync/sync.h>
class NativeTarget {
#define EGL_FN(n) decltype(&::n) n=nullptr;
    EGL_FN(eglGetProcAddress) EGL_FN(eglInitialize) EGL_FN(eglChooseConfig)
    EGL_FN(eglBindAPI) EGL_FN(eglCreateContext) EGL_FN(eglCreatePbufferSurface)
    EGL_FN(eglMakeCurrent) EGL_FN(eglDestroySurface) EGL_FN(eglDestroyContext) EGL_FN(eglTerminate)
#undef EGL_FN
#define GL_FN(n) decltype(&::n) n=nullptr;
    GL_FN(glGetString) GL_FN(glGetError) GL_FN(glGenRenderbuffers) GL_FN(glBindRenderbuffer)
    GL_FN(glDeleteRenderbuffers) GL_FN(glGenFramebuffers) GL_FN(glBindFramebuffer)
    GL_FN(glDeleteFramebuffers) GL_FN(glFramebufferRenderbuffer) GL_FN(glCheckFramebufferStatus)
    GL_FN(glViewport) GL_FN(glCreateShader) GL_FN(glShaderSource) GL_FN(glCompileShader)
    GL_FN(glGetShaderiv) GL_FN(glDeleteShader) GL_FN(glCreateProgram) GL_FN(glAttachShader)
    GL_FN(glLinkProgram) GL_FN(glGetProgramiv) GL_FN(glDeleteProgram) GL_FN(glUseProgram)
    GL_FN(glGetAttribLocation) GL_FN(glGetUniformLocation) GL_FN(glUniform1i)
    GL_FN(glVertexAttribPointer) GL_FN(glEnableVertexAttribArray) GL_FN(glDisableVertexAttribArray)
    GL_FN(glDrawArrays) GL_FN(glGenTextures) GL_FN(glBindTexture) GL_FN(glTexParameteri)
    GL_FN(glDeleteTextures) GL_FN(glActiveTexture)
    GL_FN(glEnable) GL_FN(glDisable) GL_FN(glScissor) GL_FN(glClearColor) GL_FN(glClear) GL_FN(glFlush)
#undef GL_FN
    PFNEGLCREATEIMAGEKHRPROC createImage=nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage=nullptr;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC imageStorage=nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC textureStorage=nullptr;
    PFNEGLCREATESYNCKHRPROC createSync=nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroySync=nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupFence=nullptr;
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLContext context=EGL_NO_CONTEXT;
    EGLSurface surface=EGL_NO_SURFACE;
    EGLImageKHR image=EGL_NO_IMAGE_KHR;
    GLuint rb=0,fb=0,vs=0,fs=0,program=0;
    android::sp<android::GraphicBuffer> buffer;
public:
    bool initialize(const char* path) {
        void* lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);if(!lib)return false;
#define LOAD(n) n=reinterpret_cast<decltype(n)>(dlsym(lib,#n));if(!n)return false;
        LOAD(eglGetProcAddress) LOAD(eglInitialize) LOAD(eglChooseConfig) LOAD(eglBindAPI)
        LOAD(eglCreateContext) LOAD(eglCreatePbufferSurface) LOAD(eglMakeCurrent)
        LOAD(eglDestroySurface) LOAD(eglDestroyContext) LOAD(eglTerminate)
#undef LOAD
#define LOAD(n) n=reinterpret_cast<decltype(n)>(eglGetProcAddress(#n));if(!n)return false;
        LOAD(glGetString) LOAD(glGetError) LOAD(glGenRenderbuffers) LOAD(glBindRenderbuffer)
        LOAD(glDeleteRenderbuffers) LOAD(glGenFramebuffers) LOAD(glBindFramebuffer)
        LOAD(glDeleteFramebuffers) LOAD(glFramebufferRenderbuffer) LOAD(glCheckFramebufferStatus)
        LOAD(glViewport) LOAD(glCreateShader) LOAD(glShaderSource) LOAD(glCompileShader)
        LOAD(glGetShaderiv) LOAD(glDeleteShader) LOAD(glCreateProgram) LOAD(glAttachShader)
        LOAD(glLinkProgram) LOAD(glGetProgramiv) LOAD(glDeleteProgram) LOAD(glUseProgram)
        LOAD(glGetAttribLocation) LOAD(glGetUniformLocation) LOAD(glUniform1i)
        LOAD(glVertexAttribPointer) LOAD(glEnableVertexAttribArray) LOAD(glDisableVertexAttribArray)
        LOAD(glDrawArrays) LOAD(glGenTextures) LOAD(glBindTexture) LOAD(glTexParameteri)
        LOAD(glDeleteTextures) LOAD(glActiveTexture)
        LOAD(glEnable) LOAD(glDisable) LOAD(glScissor) LOAD(glClearColor) LOAD(glClear) LOAD(glFlush)
#undef LOAD
#define EXT(member, name) member=reinterpret_cast<decltype(member)>(eglGetProcAddress(name));if(!member)return false;
        EXT(createImage,"eglCreateImageKHR") EXT(destroyImage,"eglDestroyImageKHR")
        EXT(imageStorage,"glEGLImageTargetRenderbufferStorageOES") EXT(textureStorage,"glEGLImageTargetTexture2DOES") EXT(createSync,"eglCreateSyncKHR")
        EXT(destroySync,"eglDestroySyncKHR") EXT(dupFence,"eglDupNativeFenceFDANDROID")
#undef EXT
        auto platform=reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));if(!platform)return false;
        display=platform(EGL_PLATFORM_ANDROID_KHR,EGL_DEFAULT_DISPLAY,nullptr);
        EGLConfig config=nullptr;EGLint count=0;
        const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
        if(display==EGL_NO_DISPLAY || !eglInitialize(display,nullptr,nullptr) ||
           !eglChooseConfig(display,attrs,&config,1,&count) || count!=1 || !eglBindAPI(EGL_OPENGL_ES_API))return false;
        const EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE},pa[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
        context=eglCreateContext(display,config,EGL_NO_CONTEXT,ca);surface=eglCreatePbufferSurface(display,config,pa);
        if(context==EGL_NO_CONTEXT || surface==EGL_NO_SURFACE || !eglMakeCurrent(display,surface,surface,context))return false;
        const char* renderer=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        std::printf("BINDER renderer=%s pid=%d\n",renderer?renderer:"null",getpid());
        return renderer && std::strstr(renderer,"FD305");
    }
    bool bind(buffer_handle_t handle,int width,int height,int format,int stride,uint64_t usage) {
        buffer=new android::GraphicBuffer(handle,android::GraphicBuffer::WRAP_HANDLE,width,height,format,1,usage,stride);
        if(buffer->initCheck())return false;
        const EGLint attrs[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
        image=createImage(display,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,buffer->getNativeBuffer(),attrs);
        if(image==EGL_NO_IMAGE_KHR)return false;
        glGenRenderbuffers(1,&rb);glBindRenderbuffer(GL_RENDERBUFFER,rb);imageStorage(GL_RENDERBUFFER,image);
        glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,rb);
        return rb&&fb&&glGetError()==GL_NO_ERROR&&glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    }
    int render(unsigned frame) {
        const float colors[6][4]={{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,0,1},{0,1,1,1},{1,0,1,1}};
        if(frame>=6)return -1;
        glDisable(GL_SCISSOR_TEST);glClearColor(colors[frame][0],colors[frame][1],colors[frame][2],1);glClear(GL_COLOR_BUFFER_BIT);
        if(glGetError()!=GL_NO_ERROR)return -1;
        EGLSyncKHR fence=createSync(display,EGL_SYNC_NATIVE_FENCE_ANDROID,nullptr);if(fence==EGL_NO_SYNC_KHR)return -1;
        glFlush();int fd=dupFence(display,fence);destroySync(display,fence);return fd;
    }
    // Import another process's image as a texture and draw into this GPU target.
    // Wait finitely before dropping EGLImage/texture references; no CPU frame copy.
    int compose(buffer_handle_t handle,int width,int height,int format,int stride,uint64_t usage) {
        if(!program){
            const char* vertex="attribute vec2 p;varying vec2 uv;void main(){gl_Position=vec4(p,0.,1.);uv=(p+1.)*.5;}";
            const char* fragment="precision highp float;varying vec2 uv;uniform sampler2D source;void main(){gl_FragColor=texture2D(source,uv);}";
            auto shader=[&](GLenum type,const char* text){GLuint out=glCreateShader(type);glShaderSource(out,1,&text,nullptr);glCompileShader(out);GLint ok=0;glGetShaderiv(out,GL_COMPILE_STATUS,&ok);return ok?out:GLuint(0);};
            vs=shader(GL_VERTEX_SHADER,vertex);fs=shader(GL_FRAGMENT_SHADER,fragment);if(!vs||!fs)return -1;
            program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);if(!ok)return -1;
        }
        android::sp<android::GraphicBuffer> source=new android::GraphicBuffer(handle,android::GraphicBuffer::WRAP_HANDLE,width,height,format,1,usage,stride);
        if(source->initCheck())return -1;
        const EGLint attrs[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
        EGLImageKHR imported=createImage(display,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,source->getNativeBuffer(),attrs);if(imported==EGL_NO_IMAGE_KHR)return -1;
        GLuint texture=0;glGenTextures(1,&texture);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);textureStorage(GL_TEXTURE_2D,imported);
        glBindFramebuffer(GL_FRAMEBUFFER,fb);glViewport(0,0,width,height);glDisable(GL_SCISSOR_TEST);
        glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glUseProgram(program);
        GLint position=glGetAttribLocation(program,"p"),sampler=glGetUniformLocation(program,"source");if(position<0||sampler<0||glGetError()!=GL_NO_ERROR)return -1;
        const GLfloat vertices[]={-1,-1,3,-1,-1,3};glVertexAttribPointer(position,2,GL_FLOAT,GL_FALSE,0,vertices);glEnableVertexAttribArray(position);glUniform1i(sampler,0);glDrawArrays(GL_TRIANGLES,0,3);
        if(glGetError()!=GL_NO_ERROR)return -1;
        EGLSyncKHR fence=createSync(display,EGL_SYNC_NATIVE_FENCE_ANDROID,nullptr);if(fence==EGL_NO_SYNC_KHR)return -1;
        glFlush();int fd=dupFence(display,fence);destroySync(display,fence);
        if(fd<0||sync_wait(fd,5000)){if(fd>=0)::close(fd);return -1;}
        glDisableVertexAttribArray(position);glDeleteTextures(1,&texture);
        if(!destroyImage(display,imported)){::close(fd);return -1;}
        return fd;
    }
    bool releaseTarget() {
        glBindFramebuffer(GL_FRAMEBUFFER,0);if(fb)glDeleteFramebuffers(1,&fb);if(rb)glDeleteRenderbuffers(1,&rb);fb=rb=0;
        bool ok=image==EGL_NO_IMAGE_KHR||destroyImage(display,image);image=EGL_NO_IMAGE_KHR;buffer.clear();return ok;
    }
    bool close() {
        bool ok=releaseTarget();if(program)glDeleteProgram(program);if(vs)glDeleteShader(vs);if(fs)glDeleteShader(fs);program=vs=fs=0;
        if(!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT))ok=false;
        if(surface!=EGL_NO_SURFACE&&!eglDestroySurface(display,surface))ok=false;
        if(context!=EGL_NO_CONTEXT&&!eglDestroyContext(display,context))ok=false;
        if(display!=EGL_NO_DISPLAY&&!eglTerminate(display))ok=false;return ok;
    }
};
