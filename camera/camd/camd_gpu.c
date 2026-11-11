// GPU (Adreno 305, Mesa freedreno) ISP for superman-camd: RAW10 RGGB -> YUYV.
//
// The packed RAW10 frame is uploaded as an 8-bit luminance texture (bytesperline x height). A
// fragment shader renders an (OUT_W / 2) x OUT_H RGBA target where each texel is one YUYV
// macropixel (R=Y0, G=U, B=Y1, A=V): it unpacks the 10-bit samples, removes the black level,
// demosaics bilinearly, applies white-balance gains, a colour matrix, sRGB gamma and converts to
// BT.601 full-range YUV. glReadPixels returns the bytes already in YUYV order.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm/drm_fourcc.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camd_gpu.h"
#include <time.h>
static double tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
double gpu_t[3];

static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLContext ctx = EGL_NO_CONTEXT;
static EGLSurface pb = EGL_NO_SURFACE;
static GLuint prog, tex, fbo, rbo, vbo;
static GLint u_raw, u_inv, u_off, u_wb, u_sat, u_ccm, u_dim, u_scl, u_ph, u_blk, u_dcol;
unsigned gpu_phase_x, gpu_phase_y; /* set by the caller before gpu_init */
unsigned gpu_raw_w;
int gpu_black_g = 64, gpu_black_rb = 64, gpu_defect_col = -1;
static float s_offx, s_offy, s_scl = 1.0f, s_cropx, s_cropy;
static unsigned g_bpl, g_h, g_ow, g_oh;
#define MAXIMG 8
static EGLImageKHR imgs[MAXIMG];
static GLuint itex[MAXIMG];
static PFNEGLCREATEIMAGEKHRPROC p_create;
static PFNEGLDESTROYIMAGEKHRPROC p_destroy;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_target;

/* Import capture buffer `idx` (a dma-buf of bytesperline x height bytes) as an R8 texture. */
int gpu_import(unsigned idx, int fd) {
	if (idx >= MAXIMG) return -1;
	if (!p_create) {
		p_create = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
		p_destroy = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
		p_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
		if (!p_create || !p_destroy || !p_target) return -1;
	}
	const EGLint at[] = { EGL_WIDTH, (EGLint)g_bpl, EGL_HEIGHT, (EGLint)g_h,
			      EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8, EGL_DMA_BUF_PLANE0_FD_EXT, fd,
			      EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)g_bpl, EGL_NONE };
	imgs[idx] = p_create(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, 0, at);
	if (imgs[idx] == EGL_NO_IMAGE_KHR) {
		fprintf(stderr, "superman-camd: eglCreateImage(dma-buf R8 %ux%u pitch %u): 0x%x\n", g_bpl, g_h, g_bpl, eglGetError());
		return -1;
	}
	glGenTextures(1, &itex[idx]);
	glBindTexture(GL_TEXTURE_2D, itex[idx]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	p_target(GL_TEXTURE_2D, (GLeglImageOES)imgs[idx]);
	GLenum ge = glGetError();
	if (ge != GL_NO_ERROR) {
		fprintf(stderr, "superman-camd: EGLImage -> texture: 0x%x\n", ge);
		glDeleteTextures(1, &itex[idx]); itex[idx] = 0;
		p_destroy(dpy, imgs[idx]); imgs[idx] = EGL_NO_IMAGE_KHR;
		return -1;
	}
	return 0;
}

/* Output targets: the consumer's capture buffers (YUYV = RGBA8 texels of 2 pixels). */
static EGLImageKHR oimg[MAXIMG];
static GLuint orbo[MAXIMG], ofbo[MAXIMG];

int gpu_out_import(unsigned idx, int fd) {
	if (idx >= MAXIMG || !p_create) return -1;
	const EGLint at[] = { EGL_WIDTH, (EGLint)(g_ow / 2), EGL_HEIGHT, (EGLint)g_oh,
			      EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888, EGL_DMA_BUF_PLANE0_FD_EXT, fd,
			      EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)(g_ow * 2), EGL_NONE };
	oimg[idx] = p_create(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, 0, at);
	if (oimg[idx] == EGL_NO_IMAGE_KHR) {
		fprintf(stderr, "superman-camd: output eglCreateImage: 0x%x\n", eglGetError());
		return -1;
	}
	static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC p_rb;
	if (!p_rb) p_rb = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	if (!p_rb) return -1;
	glGenRenderbuffers(1, &orbo[idx]);
	glBindRenderbuffer(GL_RENDERBUFFER, orbo[idx]);
	p_rb(GL_RENDERBUFFER, (GLeglImageOES)oimg[idx]);
	glGenFramebuffers(1, &ofbo[idx]);
	glBindFramebuffer(GL_FRAMEBUFFER, ofbo[idx]);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, orbo[idx]);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	if (st != GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR) {
		fprintf(stderr, "superman-camd: output framebuffer status 0x%x\n", st);
		return -1;
	}
	return 0;
}

void gpu_out_release(void) {
	for (int i = 0; i < MAXIMG; i++) {
		if (ofbo[i]) { glDeleteFramebuffers(1, &ofbo[i]); ofbo[i] = 0; }
		if (orbo[i]) { glDeleteRenderbuffers(1, &orbo[i]); orbo[i] = 0; }
		if (oimg[i] && oimg[i] != EGL_NO_IMAGE_KHR) p_destroy(dpy, oimg[i]);
		oimg[i] = EGL_NO_IMAGE_KHR;
	}
	if (dpy != EGL_NO_DISPLAY) glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

void gpu_release_imports(void) {
	for (int i = 0; i < MAXIMG; i++) {
		if (itex[i]) { glDeleteTextures(1, &itex[i]); itex[i] = 0; }
		if (imgs[i] != EGL_NO_IMAGE_KHR && imgs[i]) { p_destroy(dpy, imgs[i]); }
		imgs[i] = EGL_NO_IMAGE_KHR;
	}
}

static const char *vs =
	"attribute vec2 p;\n"
	"void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";

/*
 * Each fragment is one YUYV macropixel: raw pixels x (even) and x+1 of row y. It fetches the 4x3
 * neighbourhood (columns x-1..x+2, rows y-1..y+1) once, using the high 8 bits of each RAW10
 * sample, and demosaics both pixels from it (bilinear, RGGB).
 */
static const char *fs =
	"precision highp float;\n"
	"uniform sampler2D raw;\n"
	"uniform vec2 inv;      /* 1 / bytesperline, 1 / height */\n"
	"uniform vec2 off;      /* crop offset in raw pixels (even) */\n"
	"uniform vec3 wb;       /* white balance gains */\n"
	"uniform float sat;     /* chroma gain */\n"
	"uniform mat3 ccm;      /* camera RGB -> sRGB */\n"
	"uniform vec2 dim;      /* raw width, height in pixels */\n"
	"float f(float b, float ty) {\n"
	"  return max(texture2D(raw, vec2((b + 0.5) * inv.x, ty)).r - 0.0627451, 0.0) * 1.06694;\n"
	"}\n"
	"vec4 row(vec4 bx, float y) {\n"
	"  float ty = (y + 0.5) * inv.y;\n"
	"  return vec4(f(bx.x, ty), f(bx.y, ty), f(bx.z, ty), f(bx.w, ty));\n"
	"}\n"
	"/* general path (scaled output): any raw pixel, 3x3 bilinear demosaic */\n"
	"uniform float scl;     /* raw pixels per output pixel (1.0 = native fast path) */\n"
	"uniform vec2 ph;       /* position of R in the 2x2 CFA (0/1, 0/1) */\n"
	"uniform vec2 blk;      /* black level (green, red/blue) as 8-bit texel values */\n"
	"float bc(float x) { float g = floor(x * 0.25); return 5.0 * g + (x - 4.0 * g); }\n"
	"uniform float dcol;    /* first of two defective (weak) columns, or -10 */\n"
	"float Q(float x, float y) {\n"
	"  float b = mod(x + ph.x + y + ph.y, 2.0) < 1.0 ? blk.y : blk.x;\n"
	"  float v = texture2D(raw, vec2((bc(x) + 0.5) * inv.x, (y + 0.5) * inv.y)).r;\n"
	"  return max(v - b, 0.0) / (1.0 - b);\n"
	"}\n"
	"float P(float x, float y) {\n"
	"  x = clamp(x, 0.0, dim.x - 3.0); y = clamp(y, 0.0, dim.y - 1.0);\n"
	"  /* a defective column reads the same-colour column two pixels right (branch-free) */\n"
	"  x += 2.0 * step(dcol, x) * step(x, dcol + 1.0);\n"
	"  return Q(x, y);\n"
	"}\n"
	"vec3 dm(float x, float y) {\n"
	"  float c = P(x, y);\n"
	"  float h = (P(x - 1.0, y) + P(x + 1.0, y)) * 0.5;\n"
	"  float v = (P(x, y - 1.0) + P(x, y + 1.0)) * 0.5;\n"
	"  float d = (P(x - 1.0, y - 1.0) + P(x + 1.0, y - 1.0) + P(x - 1.0, y + 1.0) + P(x + 1.0, y + 1.0)) * 0.25;\n"
	"  float cr = (h + v) * 0.5;\n"
	"  bool ev = mod(y + ph.y, 2.0) < 1.0, ex = mod(x + ph.x, 2.0) < 1.0;\n"
	"  /* green sites: average with the diagonal greens of the other row (Gr/Gb differ ~15% on\n"
	"   * the binned front sensor, which otherwise leaves a maze pattern) */\n"
	"  float ge = (c + d) * 0.5;\n"
	"  if (ev) return ex ? vec3(c, cr, d) : vec3(h, ge, v);\n"
	"  return ex ? vec3(v, ge, h) : vec3(d, cr, c);\n"
	"}\n"
	"vec3 srgb(vec3 l) {\n"
	"  l = clamp(l, 0.0, 1.0);\n"
	"  /* sqrt approximation of the sRGB curve (within ~2%): pow() per channel cost ~15 ms/frame */\n"
	"  return 1.138 * sqrt(l) - 0.138 * l;\n"
	"}\n"
	"/* display tone curve: lift the black point slightly and add an S-curve; plain sRGB gamma\n"
	"   leaves indoor scenes flat and grey (lifted blacks, dull highlights) */\n"
	"vec3 tone(vec3 s) {\n"
	"  s = clamp((s - 0.04) * 1.0417, 0.0, 1.0);\n"
	"  return mix(s, s * s * (3.0 - 2.0 * s), 0.4);\n"
	"}\n"
	"/* 1.4x or more downscale (the viewfinder): one RGGB quad per output pixel (4 fetches, no\n"
	"   interpolation); a 3x3 demosaic per pixel there capped the preview near 10 fps on the A305 */\n"
	"vec3 sp(float x, float y) {\n"
	"  float qx = ph.x + 2.0 * floor((x - ph.x) * 0.5), qy = ph.y + 2.0 * floor((y - ph.y) * 0.5);\n"
	"  float g = (P(qx + 1.0, qy) + P(qx, qy + 1.0)) * 0.5;\n"
	"  return vec3(P(qx, qy), g, P(qx + 1.0, qy + 1.0));\n"
	"}\n"
	"void main() {\n"
	"  vec3 a, c;\n"
	"  if (scl > 1.399) {\n"
	"    float xo = 2.0 * floor(gl_FragCoord.x), yr = off.y + floor((floor(gl_FragCoord.y) + 0.5) * scl);\n"
	"    a = sp(off.x + floor((xo + 0.5) * scl), yr);\n"
	"    c = sp(off.x + floor((xo + 1.5) * scl), yr);\n"
	"  } else if (abs(scl - 1.0) > 0.001) { /* resample (also upscales a binned fallback) */\n"
	"    float xo = 2.0 * floor(gl_FragCoord.x), yr = off.y + floor((floor(gl_FragCoord.y) + 0.5) * scl);\n"
	"    a = dm(off.x + floor((xo + 0.5) * scl), yr);\n"
	"    c = dm(off.x + floor((xo + 1.5) * scl), yr);\n"
	"  } else {\n"
	"  /* the last raw columns carry the CSI line tail: sample at least 6 pixels inside (same parity) */\n"
	"  float x = min(2.0 * floor(gl_FragCoord.x) + off.x, dim.x - 6.0 - mod(dim.x - off.x, 2.0));\n"
	"  float y = floor(gl_FragCoord.y) + off.y;\n"
	"  /* byte columns of raw pixels x-1, x, x+1, x+2 (x is the R column: even, or odd for a\n"
	"   * sensor whose CFA starts with green; the caller's offset includes that phase) */\n"
	"  vec4 bx = vec4(bc(x - 1.0), bc(x), bc(x + 1.0), bc(x + 2.0));\n"
	"  /* mirror at the borders (same Bayer colour two pixels/rows away) */\n"
	"  if (x < 1.0) bx.x = bx.z;\n"
	"  if (x + 2.0 >= dim.x) bx.w = bx.y;\n"
	"  float yu = y < 1.0 ? y + 1.0 : y - 1.0;\n"
	"  float yd = y + 1.0 >= dim.y ? y - 1.0 : y + 1.0;\n"
	"  vec4 u = row(bx, yu), m = row(bx, y), d = row(bx, yd);\n"
	"  if (mod(y - off.y, 2.0) < 1.0) {   /* R G row: x = R, x+1 = Gr */\n"
	"    a = vec3(m.y, (m.x + m.z + u.y + d.y) * 0.25, (u.x + u.z + d.x + d.z) * 0.25);\n"
	"    c = vec3((m.y + m.w) * 0.5, m.z, (u.z + d.z) * 0.5);\n"
	"  } else {                   /* G B row: x = Gb, x+1 = B */\n"
	"    a = vec3((u.y + d.y) * 0.5, m.y, (m.x + m.z) * 0.5);\n"
	"    c = vec3((u.y + u.w + d.y + d.w) * 0.25, (m.y + m.w + u.z + d.z) * 0.25, m.z);\n"
	"  }\n"
	"  }\n"
	"  /* highlight clip: a channel saturates at 1.0 raw, so clip all channels at the smallest gain;\n"
	"   * otherwise clipped green under red/blue WB gain > 1 turns highlights magenta */\n"
	"  float wc = min(min(wb.r, wb.g), wb.b);\n"
	"  a = tone(srgb(ccm * min(a * wb, vec3(wc))));\n"
	"  c = tone(srgb(ccm * min(c * wb, vec3(wc))));\n"
	"  const vec3 ky = vec3(0.299, 0.587, 0.114);\n"
	"  vec3 mm = (a + c) * 0.5;\n"
	"  /* chroma noise suppression in the deep shadows */\n"
	"  float sa = sat * smoothstep(0.04, 0.16, dot(mm, ky));\n"
	"  float cu = dot(mm, vec3(-0.168736, -0.331264, 0.5)) * sa;\n"
	"  float cv = dot(mm, vec3(0.5, -0.418688, -0.081312)) * sa;\n"
	"  gl_FragColor = vec4(dot(a, ky), cu + 0.5019608, dot(c, ky), cv + 0.5019608);\n"
	"}\n";

static GLuint shader(GLenum t, const char *src) {
	GLuint s = glCreateShader(t);
	glShaderSource(s, 1, &src, 0);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetShaderInfoLog(s, sizeof log, 0, log);
		fprintf(stderr, "superman-camd: shader: %s\n", log);
		return 0;
	}
	return s;
}

int gpu_init(unsigned bpl, unsigned h, unsigned out_w, unsigned out_h) {
	static unsigned g_px = 99, g_py = 99;
	if (ctx != EGL_NO_CONTEXT && bpl == g_bpl && h == g_h && out_w == g_ow && out_h == g_oh &&
	    g_px == gpu_phase_x && g_py == gpu_phase_y) return 0;
	g_px = gpu_phase_x; g_py = gpu_phase_y;
	gpu_fini();
	dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, 0, 0)) return -1;
	const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	EGLConfig cfg;
	EGLint n = 0;
	if (!eglChooseConfig(dpy, ca, &cfg, 1, &n) || n != 1) return -1;
	const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
	const EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	pb = eglCreatePbufferSurface(dpy, cfg, pa);
	if (ctx == EGL_NO_CONTEXT || pb == EGL_NO_SURFACE || !eglMakeCurrent(dpy, pb, pb, ctx)) return -1;

	GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
	if (!v || !f) return -1;
	prog = glCreateProgram();
	glAttachShader(prog, v);
	glAttachShader(prog, f);
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) return -1;
	glUseProgram(prog);
	u_raw = glGetUniformLocation(prog, "raw");
	u_inv = glGetUniformLocation(prog, "inv");
	u_off = glGetUniformLocation(prog, "off");
	u_wb = glGetUniformLocation(prog, "wb");
	u_sat = glGetUniformLocation(prog, "sat");
	u_ccm = glGetUniformLocation(prog, "ccm");
	u_dim = glGetUniformLocation(prog, "dim");
	u_scl = glGetUniformLocation(prog, "scl");
	u_ph = glGetUniformLocation(prog, "ph");
	u_blk = glGetUniformLocation(prog, "blk");
	u_dcol = glGetUniformLocation(prog, "dcol");

	p_create = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	p_destroy = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	p_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, bpl, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, 0);

	glGenRenderbuffers(1, &rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, rbo);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, out_w / 2, out_h);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return -1;

	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glViewport(0, 0, out_w / 2, out_h);
	glUniform1i(u_raw, 0);
	glUniform2f(u_inv, 1.0f / bpl, 1.0f / h);
	{
		/* centred crop of the raw frame with the output aspect ratio, then scale */
		float rw = gpu_raw_w ? gpu_raw_w : bpl * 4 / 5, rh = h, cw = rw, ch = rh;
		if ((float)out_w * rh > rw * (float)out_h) ch = rw * out_h / out_w; else cw = rh * out_w / out_h;
		s_scl = cw / out_w;
		/* within 15% of native: crop a centred out_w x out_h window rather than resample. The
		 * resampling path costs ~2x (1408x1056 -> 1280x720 took 115 ms vs 61 ms), and falling behind
		 * the sensor starves the RDI writer, which then stalls (frame timeouts in video mode). */
		if (s_scl >= 0.999f && s_scl < 1.15f) {
			s_scl = 1.0f;
			s_cropx = (float)((((int)rw - (int)out_w) / 2) & ~1) + gpu_phase_x;
			s_cropy = (float)((((int)rh - (int)out_h) / 2) & ~1) + gpu_phase_y;
		}
		s_offx = (float)(((int)((rw - cw) / 2)) & ~1);
		s_offy = (float)(((int)((rh - ch) / 2)) & ~1);
		glUniform2f(u_dim, rw, rh);
		glUniform1f(u_scl, s_scl);
		glUniform2f(u_ph, (float)gpu_phase_x, (float)gpu_phase_y);
		glUniform2f(u_blk, gpu_black_g / 1023.0f, gpu_black_rb / 1023.0f);
		glUniform1f(u_dcol, gpu_defect_col >= 0 ? (float)gpu_defect_col : -10.0f);
	}
	g_bpl = bpl; g_h = h; g_ow = out_w; g_oh = out_h;
	return glGetError() == GL_NO_ERROR ? 0 : -1;
}

int gpu_process(int idx, const uint8_t *raw, unsigned ox, unsigned oy, const float wb[3], float sat,
		const float ccm[9], int out_idx, uint8_t *out) {
	double t0 = tnow();
	if (idx >= 0 && idx < MAXIMG && itex[idx]) {
		glBindTexture(GL_TEXTURE_2D, itex[idx]);
	} else {
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_bpl, g_h, GL_LUMINANCE, GL_UNSIGNED_BYTE, raw);
	}
	double t1 = tnow();
	if (s_scl != 1.0f) glUniform2f(u_off, s_offx, s_offy);
	else glUniform2f(u_off, s_cropx, s_cropy); /* ox/oy (the caller's OUT_W window) are unused */
	(void)ox; (void)oy;
	glUniform3f(u_wb, wb[0], wb[1], wb[2]);
	glUniform1f(u_sat, sat);
	glUniformMatrix3fv(u_ccm, 1, GL_FALSE, ccm); /* column-major */
	int direct = out_idx >= 0 && out_idx < MAXIMG && ofbo[out_idx];
	glBindFramebuffer(GL_FRAMEBUFFER, direct ? ofbo[out_idx] : fbo);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	double t2 = tnow();
	if (!direct) glReadPixels(0, 0, g_ow / 2, g_oh, GL_RGBA, GL_UNSIGNED_BYTE, out);
	double t3 = tnow();
	gpu_t[0] += t1 - t0; gpu_t[1] += t2 - t1; gpu_t[2] += t3 - t2;
	return glGetError() == GL_NO_ERROR ? 0 : -1;
}

void gpu_fini(void) {
	if (dpy == EGL_NO_DISPLAY) return;
	gpu_release_imports();
	gpu_out_release();
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (pb != EGL_NO_SURFACE) eglDestroySurface(dpy, pb);
	if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	dpy = EGL_NO_DISPLAY; ctx = EGL_NO_CONTEXT; pb = EGL_NO_SURFACE;
	g_bpl = g_h = g_ow = g_oh = 0;
}
