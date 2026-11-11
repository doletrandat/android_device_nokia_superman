// Prototype: RGBA -> NV12 (Venus layout) conversion on the GPU (Mesa / Adreno 305) with GLES2.
// Usage: superman-gpu-convert-test [width] [height] [iterations]
// The RGBA source is an AHardwareBuffer imported with EGL_ANDROID_get_native_client_buffer (as
// screen-capture buffers will be). The NV12 destination is a gralloc YCbCr_420_888 buffer whose
// luma is rendered through an R8 dma-buf EGLImage and whose interleaved CbCr through a GR88
// dma-buf EGLImage at the chroma offset. The result is compared with libyuv::ABGRToNV12.
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <cutils/native_handle.h>
#include <drm/drm_fourcc.h>
#include <libyuv.h>
#include <vndk/hardware_buffer.h>
#include <time.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int64_t nowUs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}

#define CHECK(c, ...)                          \
    do {                                       \
        if (!(c)) {                            \
            fprintf(stderr, "FAIL " __VA_ARGS__); \
            fprintf(stderr, "\n");             \
            return 1;                          \
        }                                      \
    } while (0)

static const char* kVs =
        "attribute vec2 pos;\n"
        "varying vec2 uv;\n"
        "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
// libyuv BT.601 limited range: Y = (66R + 129G + 25B + 0x1080) >> 8 (with R,G,B in 0..255).
static const char* kFsY =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision highp float;\n"
        "varying vec2 uv;\n"
        "uniform samplerExternalOES src;\n"
        "void main() {\n"
        "  vec3 c = texture2D(src, uv).rgb;\n"
        "  float y = dot(c, vec3(66.0, 129.0, 25.0)) / 256.0 + 16.5 / 255.0;\n"
        "  gl_FragColor = vec4(y, 0.0, 0.0, 1.0);\n"
        "}\n";
// U = (112B - 74G - 38R + 0x8080) >> 8, V = (112R - 94G - 18B + 0x8080) >> 8, from the 2x2
// average (texelOff = half a source texel: linear filtering at the block corner averages it).
static const char* kFsUV =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision highp float;\n"
        "varying vec2 uv;\n"
        "uniform samplerExternalOES src;\n"
        "void main() {\n"
        "  vec3 c = texture2D(src, uv).rgb;\n"
        "  float u = dot(c, vec3(-38.0, -74.0, 112.0)) / 256.0 + 128.5 / 255.0;\n"
        "  float v = dot(c, vec3(112.0, -94.0, -18.0)) / 256.0 + 128.5 / 255.0;\n"
        "  gl_FragColor = vec4(u, v, 0.0, 1.0);\n"
        "}\n";

static GLuint program(const char* fs) {
    auto sh = [](GLenum t, const char* s) {
        GLuint h = glCreateShader(t);
        glShaderSource(h, 1, &s, nullptr);
        glCompileShader(h);
        GLint ok = 0;
        glGetShaderiv(h, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(h, sizeof(log), nullptr, log);
            fprintf(stderr, "shader: %s\n", log);
        }
        return h;
    };
    GLuint p = glCreateProgram();
    glAttachShader(p, sh(GL_VERTEX_SHADER, kVs));
    glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

static EGLImageKHR dmabufImage(EGLDisplay dpy, int fd, uint32_t fourcc, int w, int h, int off,
                               int pitch) {
    const EGLint attrs[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
                            EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, off,
                            EGL_DMA_BUF_PLANE0_PITCH_EXT, pitch, EGL_NONE};
    return eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
}

int main(int argc, char** argv) {
    const int w = argc > 1 ? atoi(argv[1]) : 720, h = argc > 2 ? atoi(argv[2]) : 1280;
    const int iters = argc > 3 ? atoi(argv[3]) : 50;

    // Source RGBA buffer with a pattern (gradients + blocks, all channels varying).
    AHardwareBuffer_Desc sd = {(uint32_t)w, (uint32_t)h, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                               AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                                       AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                                       AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
                               0, 0, 0};
    AHardwareBuffer* src = nullptr;
    CHECK(AHardwareBuffer_allocate(&sd, &src) == 0, "alloc src");
    AHardwareBuffer_describe(src, &sd);
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            p[0] = uint8_t((x * 255) / w);
            p[1] = uint8_t((y * 255) / h);
            p[2] = uint8_t(((x / 32 + y / 32) & 1) ? 230 : 20);
            p[3] = 255;
        }
    void* sp = nullptr;
    CHECK(AHardwareBuffer_lock(src, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &sp) == 0,
          "lock src");
    for (int y = 0; y < h; y++)
        memcpy(static_cast<uint8_t*>(sp) + size_t(y) * sd.stride * 4, &rgba[size_t(y) * w * 4],
               size_t(w) * 4);
    AHardwareBuffer_unlock(src, nullptr);

    // Destination NV12 (gralloc YCbCr_420_888 = Venus layout).
    AHardwareBuffer_Desc dd = {(uint32_t)w, (uint32_t)h, 1, AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420,
                               AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                                       AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
                               0, 0, 0};
    AHardwareBuffer* dst = nullptr;
    CHECK(AHardwareBuffer_allocate(&dd, &dst) == 0, "alloc dst");
    AHardwareBuffer_Planes planes;
    CHECK(AHardwareBuffer_lockPlanes(dst, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr,
                                     &planes) == 0,
          "lockPlanes dst");
    const uintptr_t base = reinterpret_cast<uintptr_t>(planes.planes[0].data);
    const int pitch = planes.planes[0].rowStride;
    const int uvOff = int(reinterpret_cast<uintptr_t>(planes.planes[1].data) - base);
    const int cstep = planes.planes[1].pixelStride;
    AHardwareBuffer_unlock(dst, nullptr);
    const int fd = AHardwareBuffer_getNativeHandle(dst)->data[0];
    printf("src stride %u | dst pitch %d uv_offset %d chroma_step %d fd %d\n", sd.stride, pitch,
           uvOff, cstep, fd);
    CHECK(cstep == 2, "dst is not semi-planar");

    // EGL / GLES2.
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    CHECK(eglInitialize(dpy, nullptr, nullptr), "eglInitialize");
    const char* ext = eglQueryString(dpy, EGL_EXTENSIONS);
    printf("EGL vendor %s | dma_buf_import %d surfaceless %d native_client %d\n",
           eglQueryString(dpy, EGL_VENDOR), !!strstr(ext, "EGL_EXT_image_dma_buf_import"),
           !!strstr(ext, "EGL_KHR_surfaceless_context"),
           !!strstr(ext, "EGL_ANDROID_get_native_client_buffer"));
    const EGLint cfgAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE,
                               EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    CHECK(eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &n) && n == 1, "eglChooseConfig");
    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    CHECK(ctx != EGL_NO_CONTEXT, "eglCreateContext");
    const EGLint pbAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface pb = eglCreatePbufferSurface(dpy, cfg, pbAttrs);
    CHECK(eglMakeCurrent(dpy, pb, pb, ctx), "eglMakeCurrent");
    printf("GL renderer %s\n", glGetString(GL_RENDERER));

    EGLImageKHR srcImg = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                           eglGetNativeClientBufferANDROID(src), nullptr);
    CHECK(srcImg != EGL_NO_IMAGE_KHR, "src image 0x%x", eglGetError());
    EGLImageKHR yImg = dmabufImage(dpy, fd, DRM_FORMAT_R8, w, h, 0, pitch);
    CHECK(yImg != EGL_NO_IMAGE_KHR, "Y image 0x%x", eglGetError());
    EGLImageKHR uvImg = dmabufImage(dpy, fd, DRM_FORMAT_GR88, w / 2, h / 2, uvOff, pitch);
    CHECK(uvImg != EGL_NO_IMAGE_KHR, "UV image 0x%x", eglGetError());

    GLuint tex[3];
    glGenTextures(3, tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex[0]);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, srcImg);
    GLuint fbo[2];
    glGenFramebuffers(2, fbo);
    EGLImageKHR dimg[2] = {yImg, uvImg};
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[1 + i]);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, dimg[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1 + i], 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        CHECK(st == GL_FRAMEBUFFER_COMPLETE, "fbo %d status 0x%x (gl 0x%x)", i, st, glGetError());
    }
    GLuint py = program(kFsY), puv = program(kFsUV);
    CHECK(py && puv, "link");
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);

    auto convert = [&]() {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex[0]);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
        glViewport(0, 0, w, h);
        glUseProgram(py);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
        glViewport(0, 0, w / 2, h / 2);
        glUseProgram(puv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
    };
    convert();
    int64_t t0 = nowUs();
    for (int i = 0; i < iters; i++) convert();
    const double msGpu = (nowUs() - t0) / 1000.0 / iters;

    // CPU reference (libyuv) and comparison.
    std::vector<uint8_t> refY(size_t(w) * h), refUV(size_t(w) * h / 2);
    t0 = nowUs();
    libyuv::ABGRToNV12(rgba.data(), w * 4, refY.data(), w, refUV.data(), w, w, h);
    const double msCpuCached = (nowUs() - t0) / 1000.0;
    CHECK(AHardwareBuffer_lockPlanes(dst, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr,
                                     &planes) == 0,
          "lockPlanes read");
    const uint8_t* gy = static_cast<const uint8_t*>(planes.planes[0].data);
    const uint8_t* guv = static_cast<const uint8_t*>(planes.planes[1].data);
    int maxY = 0, maxUV = 0;
    double sumY = 0, sumUV = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int d = std::abs(int(gy[size_t(y) * pitch + x]) - refY[size_t(y) * w + x]);
            maxY = std::max(maxY, d);
            sumY += d;
        }
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w; x++) {
            int d = std::abs(int(guv[size_t(y) * pitch + x]) - refUV[size_t(y) * w + x]);
            maxUV = std::max(maxUV, d);
            sumUV += d;
        }
    AHardwareBuffer_unlock(dst, nullptr);
    printf("GPU %.2f ms/frame (%d iters) | libyuv from cached memory %.2f ms\n", msGpu, iters,
           msCpuCached);
    printf("diff vs libyuv: Y max %d mean %.3f | UV max %d mean %.3f\n", maxY,
           sumY / (double(w) * h), maxUV, sumUV / (double(w) * h / 2));
    return 0;
}
