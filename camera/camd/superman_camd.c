// superman-camd: camera bridge for the Lumia 730 (MSM8226 CAMSS + MIPI CCS sensors).
//
// Configures the media pipeline  ccs scaler -> csiphyN -> csidN -> ispifN -> vfe0_rdiN -> video,
// streams RAW10 and converts it on the GPU (Adreno 305, zero-copy dma-buf in and out; CPU fallback):
// black level, demosaic, white balance, gamma, YUV. Frames go to the superman-camfeed bridge, whose
// capture node the Android external camera provider reads like a webcam. The sensor only runs
// while a client streams. AE/AWB statistics come from a sparse CPU subsample of the raw frame.
//
//   superman-camd [--cam rear|front] [--feed /dev/superman-camfeedN]   bridge daemon
//   superman-camd --file OUT.yuyv [N] [--cam front]                     capture N frames, keep the last
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

#ifdef CAMD_GPU
#include "camd_gpu.h"
#include <android/log.h>
#include <sys/system_properties.h>
#endif
#include "superman_camfeed.h"

/* Maximum output = the sensor window (derived at runtime, see pipeline_setup): a 4:3 window of the
 * (binned) image whose width is a multiple of 128, so the RAW10 pitch is 32-byte aligned for the
 * GPU dma-buf import. Rear sensor: 1408x1056 from the 2x2-binned 1544x1076. */
static unsigned OUT_W = 1408, OUT_H = 1056;
static unsigned bayer_px, bayer_py; /* offset of the R pixel within the 2x2 pattern */
#define MBUS_SRGGB10 0x300f
#define BLACK 64
#define NBUF 4
#define NTHREADS 4

static void logm(const char *fmt, ...) {
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	fprintf(stderr, "superman-camd: %s\n", buf);
#ifdef CAMD_GPU
	__android_log_write(ANDROID_LOG_INFO, "superman-camd", buf);
#endif
	int k = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (k >= 0) { dprintf(k, "superman-camd: %s\n", buf); close(k); }
}

static int use_gpu, cur_idx = -1, gpu_zero_copy, cur_out = -1, out_zero;
static unsigned str_w = 1408, str_h = 1056; /* size the client streams */

static uint64_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000ull + t.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------- media setup */
struct ent { unsigned id; char name[32]; unsigned major, minor; };
static struct ent ents[64];
static int nents;

static int find_ent(const char *prefix) {
	for (int i = 0; i < nents; i++)
		if (!strncmp(ents[i].name, prefix, strlen(prefix))) return i;
	return -1;
}

static int devnode(int i, char *path, size_t n) {
	/* map major:minor to /dev via sysfs */
	char p[128], buf[32];
	DIR *d = opendir("/sys/class/video4linux");
	struct dirent *de;
	if (!d) return -1;
	while ((de = readdir(d))) {
		snprintf(p, sizeof p, "/sys/class/video4linux/%s/dev", de->d_name);
		int fd = open(p, O_RDONLY);
		if (fd < 0) continue;
		int r = read(fd, buf, sizeof buf - 1);
		close(fd);
		if (r <= 0) continue;
		buf[r] = 0;
		unsigned ma, mi;
		if (sscanf(buf, "%u:%u", &ma, &mi) == 2 && ma == ents[i].major && mi == ents[i].minor) {
			snprintf(path, n, "/dev/%s", de->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

static int media_fd = -1;

struct cam_cfg {
	const char *name, *phy, *csid, *ispif, *rdi, *video, *i2c, *feed;
	unsigned raw_pitch; /* real RAW10 bytes per line when the sensor sends more than it reports */
	unsigned raw_pitch_full; /* the same for the unbinned (full-resolution) readout */
	int black_g, black_rb; /* 10-bit black levels (green, red/blue) */
	int ae_latency;        /* frames before an exposure/gain change shows in the stats */
	int defect_col, defect_n;           /* bad columns in the binned raw image (-1: none) */
	int defect_col_full, defect_n_full; /* the same unbinned */
	int always_full;                    /* never bin (binning ruins this sensor's green channel) */
};
static const struct cam_cfg cams[] = {
	{ "rear", "msm_csiphy0", "msm_csid0", "msm_ispif0", "msm_vfe0_rdi0", "msm_vfe0_video0", "4-0012",
	  "/dev/superman-camfeed", 0, 0, 64, 64,
	  /* exposure writes show ~3-4 frames later: with 0, AE hunted (exposure 8 <-> 62 every ~10 frames) */
	  4, -1, 0, -1, 0, 0 },
	{ "front", "msm_csiphy1", "msm_csid1", "msm_ispif1", "msm_vfe0_rdi1", "msm_vfe0_video1", "4-0011",
	  "/dev/superman-camfeed1",
	  /* binned, the front sensor sends 1312-pixel lines (1640 B) for a reported 1300; the RDI
	   * writer stores them back to back, so read the buffer with that pitch */
	  1640,
	  /* unbinned it sends 3264-byte lines for a reported 2600 px / 3250 B (found from a raw dump) */
	  3264,
	  /* binned: green clips at the 64 pedestal (10% of a dark frame reads exactly 64) while red
	   * and blue sit ~72; green is ~2x as sensitive as red/blue, which AWB absorbs */
	  64, 72,
	  /* exposure/gain writes show up 4 frames later; adjusting every frame made AE oscillate */
	  4,
	  /* binned columns 590/591 read low (near black in the dark) and 592/593 run hot in the dark
	   * (this unit's sensor) */
	  590, 4,
	  /* unbinned: columns 1184/1185 read ~20 codes low */
	  1184, 2,
	  /* binned, green is clamped at the pedestal and jumps above a scene-dependent threshold
	   * (green cast, noisy shadows); unbinned the colour is right, and the 32-byte aligned
	   * 3264 pitch lets the GPU import the frame (binned needed an upload), so the frame rate
	   * is about the same */
	  1 },
};
static const struct cam_cfg *cam = &cams[0];

/* entity "<prefix>... <i2c>" of this camera's sensor */
static int find_sensor_ent(const char *what) {
	for (int i = 0; i < nents; i++)
		if (strstr(ents[i].name, what) && strstr(ents[i].name, cam->i2c)) return i;
	return -1;
}
static char sd_csiphy[32], sd_csid[32], sd_ispif[32], sd_rdi[32], sd_scaler[32], sd_pa[32], sd_binner[32], vid_rdi[32];

static int setup_link(int src, unsigned spad, int sink, unsigned kpad, int en) {
	struct media_link_desc l;
	memset(&l, 0, sizeof l);
	l.source.entity = ents[src].id; l.source.index = spad; l.source.flags = MEDIA_PAD_FL_SOURCE;
	l.sink.entity = ents[sink].id; l.sink.index = kpad; l.sink.flags = MEDIA_PAD_FL_SINK;
	l.flags = en ? MEDIA_LNK_FL_ENABLED : 0;
	if (ioctl(media_fd, MEDIA_IOC_SETUP_LINK, &l) < 0) {
		logm("link %s -> %s: %s", ents[src].name, ents[sink].name, strerror(errno));
		return -1;
	}
	return 0;
}

static int media_open(void) {
	char p[32];
	for (int m = 0; m < 4 && media_fd < 0; m++) {
		snprintf(p, sizeof p, "/dev/media%d", m);
		int fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		struct media_device_info di;
		if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &di) == 0 && !strcmp(di.driver, "qcom-camss")) media_fd = fd;
		else close(fd);
	}
	if (media_fd < 0) { logm("no qcom-camss media device"); return -1; }
	struct media_entity_desc e;
	memset(&e, 0, sizeof e);
	nents = 0;
	for (e.id = MEDIA_ENT_ID_FLAG_NEXT; nents < 64 && ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &e) == 0;
	     e.id |= MEDIA_ENT_ID_FLAG_NEXT) {
		ents[nents].id = e.id;
		snprintf(ents[nents].name, sizeof ents[nents].name, "%s", e.name);
		ents[nents].major = e.dev.major; ents[nents].minor = e.dev.minor;
		nents++;
	}
	struct { const char *n; char *out; } map[] = {
		{cam->phy, sd_csiphy}, {cam->csid, sd_csid}, {cam->ispif, sd_ispif},
		{cam->rdi, sd_rdi}, {cam->video, vid_rdi},
	};
	for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
		int k = find_ent(map[i].n);
		if (k < 0 || devnode(k, map[i].out, 32)) { logm("entity %s missing", map[i].n); return -1; }
	}
	int k = find_sensor_ent(" scaler");
	if (k < 0 || devnode(k, sd_scaler, 32)) { logm("sensor scaler missing"); return -1; }
	k = find_sensor_ent(" binner");
	if (k < 0 || devnode(k, sd_binner, 32)) { logm("sensor binner missing"); return -1; }
	k = find_sensor_ent(" pixel_array");
	if (k < 0 || devnode(k, sd_pa, 32)) { logm("sensor pixel array missing"); return -1; }
	return 0;
}

static int sd_fmt(const char *dev, unsigned pad, unsigned code, unsigned w, unsigned h) {
	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) return -1;
	struct v4l2_subdev_format f;
	memset(&f, 0, sizeof f);
	f.which = V4L2_SUBDEV_FORMAT_ACTIVE; f.pad = pad;
	int r = ioctl(fd, VIDIOC_SUBDEV_G_FMT, &f);
	f.format.code = code; f.format.width = w; f.format.height = h; f.format.field = V4L2_FIELD_NONE;
	if (r == 0) r = ioctl(fd, VIDIOC_SUBDEV_S_FMT, &f);
	close(fd);
	if (r < 0 || f.format.width != w || f.format.height != h || f.format.code != code) {
		logm("%s pad %u fmt %ux%u/%x -> %ux%u/%x (%s)", dev, pad, w, h, code, f.format.width,
		     f.format.height, f.format.code, r < 0 ? strerror(errno) : "adjusted");
		return -1;
	}
	return 0;
}

static int sd_sel(const char *dev, unsigned pad, unsigned target, int x, int y, unsigned w, unsigned h,
		  struct v4l2_rect *res) {
	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) return -1;
	struct v4l2_subdev_selection s;
	memset(&s, 0, sizeof s);
	s.which = V4L2_SUBDEV_FORMAT_ACTIVE; s.pad = pad; s.target = target;
	s.r.left = x; s.r.top = y; s.r.width = w; s.r.height = h;
	int r = ioctl(fd, VIDIOC_SUBDEV_S_SELECTION, &s);
	close(fd);
	if (res) *res = s.r;
	return r;
}

static int pa_fd = -1;
static int ctl_set(unsigned id, int v) {
	struct v4l2_control c = { .id = id, .value = v };
	return ioctl(pa_fd, VIDIOC_S_CTRL, &c);
}
static int ctl_range(unsigned id, int *min, int *max) {
	struct v4l2_queryctrl q = { .id = id };
	if (ioctl(pa_fd, VIDIOC_QUERYCTRL, &q) < 0) return -1;
	*min = q.minimum; *max = q.maximum;
	return 0;
}

static unsigned raw_w, raw_h; /* RDI frame size */
/* Full-resolution (unbinned) readout: chosen per session when the client asks for more than the
 * binned window (photo mode at the largest picture size); CAMD_NOBIN forces it. */
static int full_mode;
static int no_full;    /* this start: the unbinned buffers could not be allocated, bin instead */
static int cap_enomem; /* the last REQBUFS failed for lack of contiguous memory */
static unsigned req_w, req_h; /* size the client asked for, before clamping */
static int last_session_full = -1;
static unsigned nat_w, nat_h; /* native pixel array (known after the first pipeline setup) */

/* the binned 4:3 window for a native size (same rule as pipeline_setup) */
static void binned_window(unsigned nw, unsigned nh, unsigned *w, unsigned *h) {
	unsigned hw = nw / 2, hh = nh / 2, c = hw < hh * 4 / 3 ? hw : hh * 4 / 3;
	*w = c / 128 * 128; *h = *w * 3 / 4;
	while (*h > hh && *w > 128) { *w -= 128; *h = *w * 3 / 4; }
}

static unsigned mbus_code = MBUS_SRGGB10;

/* 10-bit Bayer codes and the position of R in the 2x2 pattern */
static const struct { unsigned code, px, py; } bayer10[] = {
	{ 0x300f, 0, 0 }, /* SRGGB10 */ { 0x300a, 1, 0 }, /* SGRBG10 */
	{ 0x300e, 0, 1 }, /* SGBRG10 */ { 0x3007, 1, 1 }, /* SBGGR10 */
};

static int pipeline_setup(void) {
	int phy = find_ent(cam->phy), csid = find_ent(cam->csid), isp = find_ent(cam->ispif),
	    rdi = find_ent(cam->rdi);
	if (setup_link(phy, 1, csid, 0, 1) || setup_link(csid, 1, isp, 0, 1) || setup_link(isp, 1, rdi, 0, 1))
		return -1;
	struct v4l2_rect r;
	/* native pixel array */
	unsigned nw = 0, nh = 0;
	{
		int fd = open(sd_pa, O_RDWR | O_CLOEXEC);
		struct v4l2_subdev_selection sl;
		memset(&sl, 0, sizeof sl);
		sl.which = V4L2_SUBDEV_FORMAT_ACTIVE; sl.pad = 0; sl.target = V4L2_SEL_TGT_CROP_BOUNDS;
		if (fd >= 0 && ioctl(fd, VIDIOC_SUBDEV_G_SELECTION, &sl) == 0) { nw = sl.r.width; nh = sl.r.height; }
		if (fd >= 0) close(fd);
	}
	if (!nw || !nh) { logm("pixel array size unknown"); return -1; }
	/* a 10-bit Bayer output code (sets the CFA phase) */
	{
		int fd = open(sd_scaler, O_RDWR | O_CLOEXEC);
		struct v4l2_subdev_mbus_code_enum c;
		mbus_code = 0;
		for (unsigned i = 0; fd >= 0 && i < 32 && !mbus_code; i++) {
			memset(&c, 0, sizeof c);
			c.pad = 1; c.index = i; c.which = V4L2_SUBDEV_FORMAT_ACTIVE;
			if (ioctl(fd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &c) < 0) break;
			for (unsigned k = 0; k < 4; k++)
				if (c.code == bayer10[k].code) { mbus_code = c.code; bayer_px = bayer10[k].px; bayer_py = bayer10[k].py; }
		}
		if (fd >= 0) close(fd);
		if (!mbus_code) { logm("no 10-bit Bayer output"); return -1; }
	}
	/* start from the full array: earlier sessions leave crops/formats in the sensor subdevs */
	sd_sel(sd_pa, 0, V4L2_SEL_TGT_CROP, 0, 0, nw, nh, &r);
	{
		int bfd = open(sd_binner, O_RDWR | O_CLOEXEC);
		struct v4l2_subdev_format bf;
		memset(&bf, 0, sizeof bf);
		bf.which = V4L2_SUBDEV_FORMAT_ACTIVE; bf.pad = 0;
		ioctl(bfd, VIDIOC_SUBDEV_G_FMT, &bf);
		bf.format.width = nw; bf.format.height = nh;
		ioctl(bfd, VIDIOC_SUBDEV_S_FMT, &bf);
		close(bfd);
	}
	sd_fmt(sd_scaler, 1, mbus_code, nw, nh); /* code only; the size follows compose */
	/* 2x2 binning when the sensor can (shorter readout -> higher frame rate) */
	unsigned bw = nw, bh = nh;
	full_mode = (getenv("CAMD_NOBIN") != NULL || cam->always_full) && !no_full;
	nat_w = nw; nat_h = nh;
	if (!full_mode && !no_full && req_w && req_h) {
		/* go unbinned when the client wants more than the binned window */
		unsigned w, h;
		binned_window(nw, nh, &w, &h);
		if (req_w > w || req_h > h) full_mode = 1;
	}
	if (full_mode) sd_sel(sd_binner, 0, V4L2_SEL_TGT_COMPOSE, 0, 0, nw, nh, &r);
	else if (sd_sel(sd_binner, 0, V4L2_SEL_TGT_COMPOSE, 0, 0, nw / 2, nh / 2, &r) == 0 && r.width == nw / 2 &&
	    r.height == nh / 2) { bw = r.width; bh = r.height; }
	else sd_sel(sd_binner, 0, V4L2_SEL_TGT_COMPOSE, 0, 0, nw, nh, &r);
	{
		int fd = open(sd_scaler, O_RDWR | O_CLOEXEC);
		struct v4l2_subdev_format f;
		memset(&f, 0, sizeof f);
		f.which = V4L2_SUBDEV_FORMAT_ACTIVE; f.pad = 0;
		ioctl(fd, VIDIOC_SUBDEV_G_FMT, &f);
		f.format.width = bw; f.format.height = bh;
		int e = ioctl(fd, VIDIOC_SUBDEV_S_FMT, &f);
		close(fd);
		if (e < 0) { logm("scaler sink: %s", strerror(errno)); return -1; }
	}
	/* 4:3 window: width a multiple of 128 (aligned RAW10 pitch), no scaling */
	unsigned cw = bw < bh * 4 / 3 ? bw : bh * 4 / 3;
	unsigned ow = cw / 128 * 128, oh = ow * 3 / 4;
	while (oh > bh && ow > 128) { ow -= 128; oh = ow * 3 / 4; }
	OUT_W = ow; OUT_H = oh;
	if (sd_sel(sd_scaler, 0, V4L2_SEL_TGT_CROP, ((bw - ow) / 2) & ~1u, ((bh - oh) / 2) & ~1u, ow, oh, &r) == 0) {
		if (sd_sel(sd_scaler, 0, V4L2_SEL_TGT_COMPOSE, 0, 0, ow, oh, &r) < 0)
			{ logm("sensor compose: %s", strerror(errno)); return -1; }
	} else {
		/* No scaler stage (front sensor). A pixel-array crop is ignored by this sensor in binned
		 * mode (it keeps sending full binned lines), so stream the whole binned image and take
		 * the OUT_W x OUT_H window on the GPU. */
		int fd = open(sd_scaler, O_RDWR | O_CLOEXEC);
		struct v4l2_subdev_format sf;
		memset(&sf, 0, sizeof sf);
		sf.which = V4L2_SUBDEV_FORMAT_ACTIVE; sf.pad = 0;
		ioctl(fd, VIDIOC_SUBDEV_G_FMT, &sf);
		sf.format.width = bw; sf.format.height = bh;
		ioctl(fd, VIDIOC_SUBDEV_S_FMT, &sf);
		close(fd);
	}
	{
		int fd = open(sd_scaler, O_RDWR | O_CLOEXEC);
		struct v4l2_subdev_format f;
		memset(&f, 0, sizeof f);
		f.which = V4L2_SUBDEV_FORMAT_ACTIVE; f.pad = 1;
		ioctl(fd, VIDIOC_SUBDEV_G_FMT, &f);
		if (f.format.code != mbus_code) { f.format.code = mbus_code; ioctl(fd, VIDIOC_SUBDEV_S_FMT, &f); }
		close(fd);
		raw_w = f.format.width; raw_h = f.format.height;
	}
	if (raw_w < OUT_W || raw_h < OUT_H) { logm("sensor gave %ux%u < %ux%u", raw_w, raw_h, OUT_W, OUT_H); return -1; }
	const char *chain[] = { sd_csiphy, sd_csid, sd_ispif, sd_rdi };
	for (int i = 0; i < 4; i++)
		if (sd_fmt(chain[i], 0, mbus_code, raw_w, raw_h) < 0) return -1;
	logm("%s: native %ux%u%s, pipeline %ux%u RAW10 code 0x%x", cam->name, nw, nh, bw != nw ? " binned 2x2" : "",
	     raw_w, raw_h, mbus_code);
	return 0;
}

/* ---------------------------------------------------------------- capture */
static int cap_fd = -1;
static void *cap_map[NBUF];
static unsigned cap_len[NBUF], cap_bpl;

/* Capture buffers are allocated once, for the largest frame this camera produces (the unbinned
 * window), and kept for the life of the process: CMA fills up with pinned pages after some use, so
 * a fresh multi-megabyte allocation per session (or per still) eventually fails. The RDI writer
 * stores whatever frame arrives, and qcom-camss (larger_fmt_ok) accepts the larger buffer format;
 * each session only sets the real line pitch and height. */
static unsigned cap_count;

static unsigned bayer_fourcc(void) {
	static const unsigned fcc[] = { V4L2_PIX_FMT_SRGGB10P, V4L2_PIX_FMT_SGRBG10P, V4L2_PIX_FMT_SGBRG10P,
					V4L2_PIX_FMT_SBGGR10P };
	return fcc[(bayer_px + 2 * bayer_py) & 3];
}

static void capture_free(void) {
	if (cap_fd < 0) return;
	for (int i = 0; i < NBUF; i++)
		if (cap_map[i]) { munmap(cap_map[i], cap_len[i]); cap_map[i] = 0; }
	struct v4l2_requestbuffers rb = { .count = 0, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP };
	ioctl(cap_fd, VIDIOC_REQBUFS, &rb);
	close(cap_fd);
	cap_fd = -1;
	cap_count = 0;
}

static int capture_alloc(unsigned w, unsigned h) {
	cap_fd = open(vid_rdi, O_RDWR | O_CLOEXEC);
	if (cap_fd < 0) { logm("open %s: %s", vid_rdi, strerror(errno)); return -1; }
	struct v4l2_format f;
	memset(&f, 0, sizeof f);
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f.fmt.pix_mp.width = w; f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.pixelformat = bayer_fourcc();
	f.fmt.pix_mp.num_planes = 1;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (ioctl(cap_fd, VIDIOC_S_FMT, &f) < 0) { logm("S_FMT: %s", strerror(errno)); capture_free(); return -1; }
	struct v4l2_requestbuffers rb = { .count = NBUF, .type = f.type, .memory = V4L2_MEMORY_MMAP };
	cap_enomem = 0;
	if (ioctl(cap_fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
		cap_enomem = errno == ENOMEM;
		logm("REQBUFS %ux%u: %s", w, h, strerror(errno));
		capture_free();
		return -1;
	}
	for (unsigned i = 0; i < rb.count && i < NBUF; i++) {
		struct v4l2_buffer b; struct v4l2_plane pl[1];
		memset(&b, 0, sizeof b); memset(pl, 0, sizeof pl);
		b.type = f.type; b.memory = V4L2_MEMORY_MMAP; b.index = i; b.m.planes = pl; b.length = 1;
		if (ioctl(cap_fd, VIDIOC_QUERYBUF, &b) < 0) { capture_free(); return -1; }
		cap_len[i] = pl[0].length;
		cap_map[i] = mmap(0, cap_len[i], PROT_READ | PROT_WRITE, MAP_SHARED, cap_fd, pl[0].m.mem_offset);
		if (cap_map[i] == MAP_FAILED) { cap_map[i] = 0; capture_free(); return -1; }
		cap_count = i + 1;
	}
	logm("%s: %u capture buffers of %u bytes (%ux%u)", cam->name, cap_count, cap_len[0], w, h);
	return 0;
}

static int capture_start(void) {
	/* buffers too small for this session (e.g. allocated before an unbinned request): start over */
	unsigned need = (((raw_w * 5 / 4) + 7) & ~7u) * raw_h;
	if (cap_fd >= 0 && cap_len[0] < need) capture_free();
	if (cap_fd < 0 && capture_alloc(raw_w, raw_h) < 0) return -1;
	/* the real line pitch of this mode */
	unsigned pitch = full_mode ? cam->raw_pitch_full : cam->raw_pitch;
	cap_bpl = pitch ? pitch : ((raw_w * 5 / 4) + 7) & ~7u;
	unsigned lines = cap_len[0] / cap_bpl;
	if (pitch) logm("%s: raw pitch %u, %u whole lines in the buffer", cam->name, pitch, lines);
	if (lines < raw_h) raw_h = lines;
	for (unsigned i = 0; i < cap_count; i++) {
		struct v4l2_buffer b; struct v4l2_plane pl[1];
		memset(&b, 0, sizeof b); memset(pl, 0, sizeof pl);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP; b.index = i; b.m.planes = pl; b.length = 1;
		if (ioctl(cap_fd, VIDIOC_QBUF, &b) < 0) { logm("QBUF: %s", strerror(errno)); return -1; }
	}
	return 0;
}

static int capture_streamon(void) {
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(cap_fd, VIDIOC_STREAMON, &t) < 0) {
		logm("STREAMON: %s", strerror(errno));
		/* e.g. the kept buffers' format no longer matches (-EPIPE): reallocate next time */
		int e = errno;
		ioctl(cap_fd, VIDIOC_STREAMOFF, &t);
		if (e == EPIPE) capture_free();
		return -1;
	}
	return 0;
}

static void capture_stop(void) {
	if (cap_fd < 0) return;
#ifdef CAMD_GPU
	if (gpu_zero_copy) { gpu_release_imports(); gpu_zero_copy = 0; }
#endif
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ioctl(cap_fd, VIDIOC_STREAMOFF, &t); /* returns every buffer; they stay allocated */
	if (getenv("CAMD_NOKEEP")) capture_free();
}

/* returns buffer index, or -1 on timeout/error */
static int capture_get(int timeout_ms, struct v4l2_buffer *b, struct v4l2_plane *pl) {
	struct pollfd p = { .fd = cap_fd, .events = POLLIN };
	if (poll(&p, 1, timeout_ms) <= 0) return -1;
	memset(b, 0, sizeof *b); memset(pl, 0, sizeof *pl);
	b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b->memory = V4L2_MEMORY_MMAP; b->m.planes = pl; b->length = 1;
	if (ioctl(cap_fd, VIDIOC_DQBUF, b) < 0) return -1;
	/* Behind the sensor? Take the newest frame and give the older ones straight back: if done
	 * frames pile up here, the VFE runs out of buffers, drops to its single-buffer mode (which
	 * writes only occasional frames) and the session stalls. */
	for (;;) {
		struct pollfd q = { .fd = cap_fd, .events = POLLIN };
		if (poll(&q, 1, 0) <= 0 || !(q.revents & POLLIN)) break;
		struct v4l2_buffer n; struct v4l2_plane np[1];
		memset(&n, 0, sizeof n); memset(np, 0, sizeof np);
		n.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; n.memory = V4L2_MEMORY_MMAP; n.m.planes = np; n.length = 1;
		if (ioctl(cap_fd, VIDIOC_DQBUF, &n) < 0) break;
		ioctl(cap_fd, VIDIOC_QBUF, b);
		*b = n; *pl = np[0]; b->m.planes = pl;
	}
	return b->index;
}
static void capture_put(struct v4l2_buffer *b) { ioctl(cap_fd, VIDIOC_QBUF, b); }

/* ---------------------------------------------------------------- ISP */
/* white balance gains (Q8), exposure state */
static int wb_r = 512, wb_g = 256, wb_b = 400;
static uint8_t gamma_lut[4096]; /* linear 12-bit -> sRGB-ish 8-bit */
static int sat_q8 = 330;        /* chroma gain: sensor colours are weak without a CCM */

static void init_lut(void) {
	for (int i = 0; i < 4096; i++) {
		double x = i / 4095.0;
		double y = x < 0.0031308 ? 12.92 * x : 1.055 * __builtin_pow(x, 1 / 2.4) - 0.055;
		int v = (int)(y * 255 + 0.5);
		gamma_lut[i] = v > 255 ? 255 : v;
	}
}

struct job { const uint8_t *in; uint8_t *out; unsigned y0, y1, ox, oy; uint64_t sum[3]; };

/* unpack one RAW10 row to 16-bit with black level removed */
static void unpack_row(const uint8_t *src, uint16_t *dst, unsigned w) {
	for (unsigned x = 0; x < w; x += 4) {
		const uint8_t *g = src + (x >> 2) * 5;
		unsigned lo = g[4];
		for (int s = 0; s < 4; s++) {
			int v = ((g[s] << 2) | ((lo >> (2 * s)) & 3)) - BLACK;
			dst[x + s] = v < 0 ? 0 : v;
		}
	}
}

static inline uint8_t clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* per-channel 10-bit linear -> 8-bit gamma tables with the WB gain folded in (rebuilt per frame) */
static uint8_t lut_r[1024], lut_g[1024], lut_b[1024];
static void build_luts(void) {
	for (int i = 0; i < 1024; i++) {
		int r = (i * wb_r) >> 6, g = (i * wb_g) >> 6, b = (i * wb_b) >> 6;
		lut_r[i] = gamma_lut[r > 4095 ? 4095 : r];
		lut_g[i] = gamma_lut[g > 4095 ? 4095 : g];
		lut_b[i] = gamma_lut[b > 4095 ? 4095 : b];
	}
}

/* two RGB pixels -> one YUYV macropixel (BT.601 full range, chroma gain sat_q8) */
static inline void emit2(uint8_t *o, int r0, int g0, int b0, int r1, int g1, int b1) {
	int pr0 = lut_r[r0], pg0 = lut_g[g0], pb0 = lut_b[b0], pr1 = lut_r[r1], pg1 = lut_g[g1], pb1 = lut_b[b1];
	int sr = pr0 + pr1, sg = pg0 + pg1, sb = pb0 + pb1; /* 2x sums */
	o[0] = (77 * pr0 + 150 * pg0 + 29 * pb0) >> 8;
	o[2] = (77 * pr1 + 150 * pg1 + 29 * pb1) >> 8;
	int u = (-43 * sr - 85 * sg + 128 * sb) >> 9, v = (128 * sr - 107 * sg - 21 * sb) >> 9;
	o[1] = clamp255(128 + ((u * sat_q8) >> 8));
	o[3] = clamp255(128 + ((v * sat_q8) >> 8));
}

static inline int m1023(int v) { return v > 1023 ? 1023 : v; }

static void *isp_worker(void *arg) {
	struct job *j = arg;
	unsigned W = raw_w, ox = j->ox;
	uint16_t *buf = malloc(4 * (W + 8) * sizeof(uint16_t)), *rows[4];
	for (int i = 0; i < 4; i++) rows[i] = buf + i * (W + 8);
	j->sum[0] = j->sum[1] = j->sum[2] = 0;
	/* raw rows y-1 .. y+2 around output quad row pair (y even, R row first) */
	unsigned y = j->y0 + j->oy;
	for (int k = 0; k < 4; k++) unpack_row(j->in + (y - 1 + k) * cap_bpl, rows[k], W);
	for (unsigned oy = j->y0; oy < j->y1; oy += 2, y += 2) {
		if (oy != j->y0) {
			uint16_t *t0 = rows[0], *t1 = rows[1];
			rows[0] = rows[2]; rows[1] = rows[3]; rows[2] = t0; rows[3] = t1;
			unsigned y2 = y + 1 < raw_h ? y + 1 : raw_h - 1, y3 = y + 2 < raw_h ? y + 2 : raw_h - 1;
			unpack_row(j->in + y2 * cap_bpl, rows[2], W);
			unpack_row(j->in + y3 * cap_bpl, rows[3], W);
		}
		const uint16_t *a = rows[0], *m = rows[1], *n = rows[2], *d = rows[3]; /* y-1, y(R), y+1(B), y+2 */
		uint8_t *o0 = j->out + oy * OUT_W * 2, *o1 = o0 + OUT_W * 2;
		int stat = !(oy & 7);
		for (unsigned x = ox; x < ox + OUT_W; x += 2) {
			/* quad: R(x,y) G(x+1,y) / G(x,y+1) B(x+1,y+1) */
			int R = m[x], Gr = m[x + 1], Gb = n[x], B = n[x + 1];
			int rR = R, gR = (m[x - 1] + Gr + a[x] + Gb) >> 2, bR = (a[x - 1] + a[x + 1] + n[x - 1] + B) >> 2;
			int rG1 = (R + m[x + 2]) >> 1, gG1 = Gr, bG1 = (a[x + 1] + B) >> 1;
			int rG2 = (R + d[x]) >> 1, gG2 = Gb, bG2 = (n[x - 1] + B) >> 1;
			int rB = (R + m[x + 2] + d[x] + d[x + 2]) >> 2, gB = (Gb + n[x + 2] + Gr + d[x + 1]) >> 2, bB = B;
			if (stat && !(x & 7)) { j->sum[0] += R; j->sum[1] += Gr; j->sum[2] += B; }
			emit2(o0, m1023(rR), m1023(gR), m1023(bR), m1023(rG1), m1023(gG1), m1023(bG1));
			emit2(o1, m1023(rG2), m1023(gG2), m1023(bG2), m1023(rB), m1023(gB), m1023(bB));
			o0 += 4; o1 += 4;
		}
	}
	free(buf);
	return 0;
}

/* AE target: mean green (10-bit linear, black removed) after WB */
#define AE_TARGET 175 /* raised from 150 with the GPU tone curve, which darkens the lower mid-tones */
static int exp_min, exp_max, exp_cur, gain_min, gain_max, gain_cur;
static int frame_base, vblank_min, vblank_cap, vblank_cur; /* lines; exposure max = frame_base + vblank - margin */
#define EXP_MARGIN 8
static double dgain = 1.0, want_exp; /* digital gain (GPU); total exposure AE wanted */

static void isp_update(const uint64_t *s);

/* AWB/AE statistics from a sparse subsample of the raw frame (every 8th quad). */
static void raw_stats(const uint8_t *in, unsigned ox, unsigned oy, uint64_t s[3]) {
	s[0] = s[1] = s[2] = 0;
	for (unsigned y = oy; y < oy + OUT_H; y += 8) {
		const uint8_t *r0 = in + y * cap_bpl, *r1 = r0 + cap_bpl;
		for (unsigned x = ox; x < ox + OUT_W; x += 8) {
			/* R at (x, y), G at (x + 1, y), B at (x + 1, y + 1); high 8 bits of each RAW10 sample */
#define RB(row, px) ((row)[((px) >> 2) * 5 + ((px) & 3)] << 2)
			int R = RB(r0, x) - cam->black_rb, G = RB(r0, x + 1) - cam->black_g, B = RB(r1, x + 1) - cam->black_rb;
#undef RB
			s[0] += R > 0 ? R : 0; s[1] += G > 0 ? G : 0; s[2] += B > 0 ? B : 0;
		}
	}
}

static void isp_frame(const uint8_t *in, uint8_t *out) {
	pthread_t th[NTHREADS];
	struct job jobs[NTHREADS];
	unsigned ox = (((raw_w - OUT_W) / 2) & ~1u) + bayer_px, oy = (((raw_h - OUT_H) / 2) & ~1u) + bayer_py;
	if (!use_gpu) { /* CPU path: needs a 2-pixel margin and assumes RGGB at (ox, oy) */
		if (ox < 2) ox += 2;
		if (oy < 2) oy += 2;
	}
	int dc = full_mode ? cam->defect_col_full : cam->defect_col, dn = full_mode ? cam->defect_n_full : cam->defect_n;
	if (dc >= 0) { /* bad columns c0..c0+n-1: mean of the clean same-colour columns either side */
		unsigned c0 = dc, n = dn;
#define BI(px) (((px) >> 2) * 5 + ((px) & 3))
		for (unsigned y = 0; y < raw_h; y++) {
			uint8_t *row = (uint8_t *)in + y * cap_bpl;
			for (unsigned c = c0; c < c0 + n; c++) {
				unsigned p = (c - c0) & 1;
				row[BI(c)] = (row[BI(c0 - 2 + p)] + row[BI(c0 + n + p)] + 1) >> 1;
			}
		}
#undef BI
	}
	{ /* debug: CAMD_DUMPRAW=path writes the 30th raw frame as camd sees it */
		static int nf; const char *dp = getenv("CAMD_DUMPRAW");
		if (dp && ++nf == 30) { FILE *df = fopen(dp, "wb"); if (df) { fwrite(in, 1, getenv("CAMD_DUMPALL") ? cap_len[cur_idx] : (size_t)cap_bpl * raw_h, df); fclose(df); }
			logm("dumped raw %ux%u ox %u oy %u exp %d gain %d", cap_bpl, raw_h, ox, oy, exp_cur, gain_cur); }
	}
	if (use_gpu) {
		uint64_t st[3];
		raw_stats(in, ox, oy, st);
		static const float ccm[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		const float k = 959.0f / (64.0f * 4095.0f);
		float kd = k * (float)dgain;
		float wb[3] = { wb_r * kd, wb_g * kd, wb_b * kd };
		if (gpu_process(gpu_zero_copy ? cur_idx : -1, in, ox, oy, wb, sat_q8 / 256.0f, ccm, cur_out, out) == 0) {
			isp_update(st);
			return;
		}
		logm("gpu ISP failed; using the CPU");
		use_gpu = 0;
	}
	build_luts();
	for (int i = 0; i < NTHREADS; i++) {
		jobs[i] = (struct job){ .in = in, .out = out, .y0 = (OUT_H * i / NTHREADS) & ~1u,
					.y1 = (OUT_H * (i + 1) / NTHREADS) & ~1u, .ox = ox, .oy = oy };
		pthread_create(&th[i], 0, isp_worker, &jobs[i]);
	}
	uint64_t s[3] = { 0, 0, 0 };
	for (int i = 0; i < NTHREADS; i++) {
		pthread_join(th[i], 0);
		for (int c = 0; c < 3; c++) s[c] += jobs[i].sum[c];
	}
	isp_update(s);
}

static void isp_update(const uint64_t *s) {
	/* grey-world AWB, smoothed */
	if (s[0] && s[1] && s[2]) {
		int tr = (int)((s[1] * 256) / s[0]), tb = (int)((s[1] * 256) / s[2]);
		if (tr < 200) tr = 200; if (tr > 1024) tr = 1024;
		if (tb < 200) tb = 200; if (tb > 1024) tb = 1024;
		wb_r += (tr - wb_r) / 4;
		wb_b += (tb - wb_b) / 4;
	}
	/* AE on mean green */
	unsigned n = ((OUT_W + 7) / 8) * ((OUT_H + 7) / 8);
	int mean = n ? (int)(s[1] / n) : AE_TARGET;
	if (getenv("CAMD_AEDBG")) logm("ae: mean g %d r %d b %d exp %d gain %d dg %.2f", mean, n ? (int)(s[0] / n) : 0, n ? (int)(s[2] / n) : 0, exp_cur, gain_cur, dgain);
	static int ae_wait;
	if (ae_wait > 0) { ae_wait--; return; }
	if (pa_fd >= 0 && (mean < AE_TARGET * 4 / 5 || mean > AE_TARGET * 5 / 4)) {
		/* total = exposure * gain factor; gain register g -> 256 / (256 - g) */
		double gf = 256.0 / (256 - gain_cur) * dgain;
		double cur = exp_cur < 1 ? 1 : exp_cur;
		double want = cur * gf * AE_TARGET / (mean < 4 ? 4 : mean);
		double step = want / (cur * gf);
		if (step > 2.0) want = cur * gf * 2.0;
		if (step < 0.5) want = cur * gf * 0.5;
		/* Exposure up to this session's frame length, then analogue gain, then up to 2x digital
		 * gain on the GPU. The frame length (VBLANK) is only chosen at stream start: changing it
		 * mid-stream can stall the sensor. */
		int e = (int)want, g = 0;
		int emax = frame_base + vblank_cur - EXP_MARGIN - 16; /* exposure == frame length stalls the sensor */
		double dg = 1.0;
		if (e > emax) {
			e = emax;
			double need = want / emax; /* required gain factor */
			g = (int)(256 - 256 / need);
			if (g > gain_max) {
				g = gain_max;
				dg = need / (256.0 / (256 - gain_max));
			}
			if (g < gain_min) g = gain_min;
		}
		if (dg > 2.0) dg = 2.0;
		if (e < exp_min) e = exp_min;
		want_exp = want; /* remembered for the next session's frame length */
		if (e != exp_cur || g != gain_cur) ae_wait = cam->ae_latency;
		if (e != exp_cur) { ctl_set(V4L2_CID_EXPOSURE, e); exp_cur = e; }
		if (g != gain_cur) { ctl_set(V4L2_CID_ANALOGUE_GAIN, g); gain_cur = g; }
		dgain = dg;
	}
}


/* ---------------------------------------------------------------- continuous AF */
/* Contrast detection on the raw frame: lens via V4L2_CID_FOCUS_ABSOLUTE (ccs, SMIA++ actuator). */
static int af_avail, af_pos = -1, af_state, af_wait, af_best_pos, af_lo, af_hi, af_step, af_bad;
static double af_best, af_ref;
enum { AF_IDLE, AF_COARSE, AF_FINE, AF_FOCUSED };
#define AF_MAX 1023
#define AF_SETTLE 2 /* frames after a move before measuring */

static void af_move(int pos) {
	if (pos < 0) pos = 0;
	if (pos > AF_MAX) pos = AF_MAX;
	if (pos != af_pos && ctl_set(V4L2_CID_FOCUS_ABSOLUTE, pos) == 0) af_pos = pos;
	af_wait = AF_SETTLE;
}

/* green gradient energy in the central third, normalised by brightness */
static double af_metric(const uint8_t *in) {
	unsigned x0 = OUT_W / 3, x1 = 2 * OUT_W / 3, y0 = OUT_H / 3, y1 = 2 * OUT_H / 3;
	uint64_t g = 0, lum = 0;
	for (unsigned y = (y0 & ~1u) + bayer_py; y < y1; y += 4) {
		const uint8_t *row = in + y * cap_bpl;
		for (unsigned x = (x0 & ~3u); x + 8 < x1; x += 4) {
			const uint8_t *q = row + (x >> 2) * 5; /* 4 pixels: q[0..3] (high bytes) */
			int a = q[0], b = q[2], c = q[5];       /* same colour: x, x+2, x+4 */
			g += (unsigned)abs(a - b) + (unsigned)abs(b - c);
			lum += a;
		}
	}
	return lum ? (double)g / (double)lum : 0;
}

static void af_start(void) {
	struct v4l2_queryctrl q = { .id = V4L2_CID_FOCUS_ABSOLUTE };
	af_avail = pa_fd >= 0 && ioctl(pa_fd, VIDIOC_QUERYCTRL, &q) == 0;
#ifdef CAMD_GPU
	/* Opt-in until the lens is shown to move (vendor.superman.camera.af=1): the SMIA++ lens
	 * registers latch, but no focus change has been measured yet. */
	{
		char v[PROP_VALUE_MAX] = "";
		__system_property_get("vendor.superman.camera.af", v);
		if (strcmp(v, "1")) af_avail = 0;
	}
#endif
	af_pos = -1;
	af_state = af_avail ? AF_COARSE : AF_IDLE;
	if (!af_avail) return;
	af_lo = 0; af_hi = AF_MAX; af_step = 85; af_best = -1;
	af_move(af_lo);
	logm("autofocus: continuous, lens 0..%d", AF_MAX);
}

static void af_frame(const uint8_t *in) {
	if (!af_avail || af_state == AF_IDLE) return;
	if (af_wait > 0) { af_wait--; return; }
	double m = af_metric(in);
	switch (af_state) {
	case AF_COARSE:
	case AF_FINE:
		if (m > af_best) { af_best = m; af_best_pos = af_pos; }
		if (af_pos + af_step <= af_hi) { af_move(af_pos + af_step); break; }
		if (af_state == AF_COARSE) {
			af_state = AF_FINE;
			af_lo = af_best_pos - af_step; af_hi = af_best_pos + af_step; af_step = 20;
			if (af_lo < 0) af_lo = 0;
			if (af_hi > AF_MAX) af_hi = AF_MAX;
			af_best = -1;
			af_move(af_lo);
			break;
		}
		af_move(af_best_pos);
		af_state = AF_FOCUSED; af_ref = af_best; af_bad = 0;
		logm("autofocus: lens %d (sharpness %.4f)", af_best_pos, af_best);
		break;
	case AF_FOCUSED:
		/* refocus when the scene stays clearly softer than at the focus point */
		if (m < af_ref * 0.6) { if (++af_bad >= 10) { af_state = AF_COARSE; af_lo = 0; af_hi = AF_MAX; af_step = 85; af_best = -1; af_move(0); } }
		else { af_bad = 0; if (m > af_ref) af_ref = m; else af_ref = af_ref * 0.98 + m * 0.02; }
		break;
	}
}

/* ---------------------------------------------------------------- camera on/off */
static uint8_t *out_frame;
static int camera_on;

static int session_wants_full(void) {
	if ((getenv("CAMD_NOBIN") || cam->always_full) && !no_full) return 1;
	if (no_full || !req_w || !req_h || !nat_w) return 0;
	unsigned w, h;
	binned_window(nat_w, nat_h, &w, &h);
	return req_w > w || req_h > h;
}

static int camera_start(void) {
	if (media_fd < 0 && media_open() < 0) return -1;
	/* A binned -> unbinned switch streams nothing (VFE SOF timeout) unless the sensor has been
	 * powered off in between (some state survives a stream stop); the loader sets a 100 ms
	 * runtime-PM autosuspend delay, so wait for the sensor to suspend first. */
	if (last_session_full == 0 && session_wants_full()) {
		char pth[96], st[16];
		snprintf(pth, sizeof pth, "/sys/bus/i2c/devices/%s/power/runtime_status", cam->i2c);
		uint64_t t0 = now_ms();
		for (;;) {
			int fd = open(pth, O_RDONLY | O_CLOEXEC), n = 0;
			if (fd >= 0) { n = read(fd, st, sizeof st - 1); close(fd); }
			if (n <= 0 || !strncmp(st, "suspended", 9) || now_ms() - t0 > 2500) break;
			usleep(20000);
		}
		logm("%s: sensor power-cycle before unbinned readout took %llu ms", cam->name, (unsigned long long)(now_ms() - t0));
	}
	if (pipeline_setup() < 0) return -1;
	last_session_full = full_mode;
	/* the client size can never exceed this sensor's window (file mode keeps the default) */
	/* (a binned fallback keeps the client size: the GPU upscales) */
	if ((str_w > OUT_W || str_h > OUT_H) && !no_full) { str_w = OUT_W; str_h = OUT_H; }
	{
		static size_t have;
		size_t need = (size_t)(OUT_W > str_w ? OUT_W : str_w) * (OUT_H > str_h ? OUT_H : str_h) * 2;
		if (need > have) {
			uint8_t *n = realloc(out_frame, need);
			if (!n) return -1;
			out_frame = n; have = need;
		}
	}
	pa_fd = open(sd_pa, O_RDWR | O_CLOEXEC);
	if (pa_fd < 0) return -1;
	/* Fastest link (the PLL, and so the line time, follows it), then shortest frame. */
	{
		int fd = open(sd_scaler, O_RDWR | O_CLOEXEC);
		struct v4l2_queryctrl q = { .id = V4L2_CID_LINK_FREQ };
		if (fd >= 0 && ioctl(fd, VIDIOC_QUERYCTRL, &q) == 0) {
			/* Windows Phone caps the rear CSI link at 342 MHz (registry RFC\CameraClocks\MaxCsiFreqMhz):
			 * use the fastest menu entry within that */
			int pick = q.minimum;
			for (int k = q.minimum; k <= q.maximum; k++) {
				struct v4l2_querymenu m = { .id = V4L2_CID_LINK_FREQ, .index = k };
				if (ioctl(fd, VIDIOC_QUERYMENU, &m) == 0 && m.value <= 342000000LL) pick = k;
			}
			q.maximum = pick;
			/* set another entry first: an unchanged value skips the driver's PLL update, and a
			 * binned -> unbinned change then streams nothing (VFE SOF timeout) */
			for (int k = q.minimum; k <= q.maximum; k++)
				if (k != pick) {
					struct v4l2_control o = { .id = V4L2_CID_LINK_FREQ, .value = k };
					if (ioctl(fd, VIDIOC_S_CTRL, &o) == 0) break;
				}
			struct v4l2_control c = { .id = V4L2_CID_LINK_FREQ, .value = q.maximum };
			if (ioctl(fd, VIDIOC_S_CTRL, &c) < 0) logm("link freq %d: %s", q.maximum, strerror(errno));
		}
		if (fd >= 0) close(fd);
	}
	{
		int vmax;
		ctl_range(V4L2_CID_VBLANK, &vblank_min, &vmax);
		ctl_set(V4L2_CID_VBLANK, vblank_min);
		ctl_range(V4L2_CID_EXPOSURE, &exp_min, &exp_max);
		frame_base = exp_max - vblank_min + EXP_MARGIN; /* ccs: exposure max = frame length - margin */
		/* frame length for this session: long enough for what the last session wanted (up to ~2x) */
		int vb = vblank_min;
		if (want_exp > frame_base + vblank_min - EXP_MARGIN) vb = (int)want_exp - frame_base + EXP_MARGIN;
		/* Frame length stays at the minimum: stretching it in the dark (CAMD_LONGVB=1, up to ~2x)
		 * halved the viewfinder rate, and the session that started the 2026-11-08 restart loop had
		 * a stretched frame. Dark scenes use analogue + digital gain instead. */
		vblank_cap = getenv("CAMD_LONGVB") ? frame_base + 2 * vblank_min : vblank_min;
		if (vb > vblank_cap) vb = vblank_cap;
		vblank_cur = vb;
		if (vb != vblank_min) ctl_set(V4L2_CID_VBLANK, vb);
		ctl_range(V4L2_CID_EXPOSURE, &exp_min, &exp_max);
	}
	ctl_range(V4L2_CID_ANALOGUE_GAIN, &gain_min, &gain_max);
	if (exp_cur <= exp_min || exp_cur > exp_max - 16) exp_cur = exp_max / 2;
	if (gain_cur > gain_max) gain_cur = 0;
	ctl_set(V4L2_CID_EXPOSURE, exp_cur);
	ctl_set(V4L2_CID_ANALOGUE_GAIN, gain_cur);
	if (capture_start() < 0) {
		capture_stop(); close(pa_fd); pa_fd = -1;
		if (cap_enomem && full_mode && !no_full) {
			/* CMA could not supply the unbinned buffers: a binned session beats no camera */
			logm("%s: no memory for full-resolution buffers, binning this session", cam->name);
			no_full = 1;
			int r = camera_start();
			no_full = 0;
			return r;
		}
		return -1;
	}
#ifdef CAMD_GPU
	gpu_phase_x = bayer_px; gpu_phase_y = bayer_py;
	gpu_raw_w = raw_w; gpu_black_g = cam->black_g; gpu_black_rb = cam->black_rb;
	gpu_defect_col = -1; /* repaired on the CPU in isp_frame */
	if (getenv("CAMD_PHASE")) { int p = atoi(getenv("CAMD_PHASE")); gpu_phase_x = p & 1; gpu_phase_y = p >> 1 & 1; }
	use_gpu = gpu_init(cap_bpl, raw_h, str_w, str_h) == 0;
	if (!use_gpu) logm("GPU ISP unavailable; using the CPU");
	gpu_zero_copy = 0;
	if (use_gpu) {
		int ok = 1;
		for (int i = 0; i < NBUF && ok; i++) {
			struct v4l2_exportbuffer eb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .index = i, .plane = 0,
							.flags = O_RDONLY | O_CLOEXEC };
			if (ioctl(cap_fd, VIDIOC_EXPBUF, &eb) < 0) { ok = 0; break; }
			if (gpu_import(i, eb.fd) < 0) ok = 0;
			close(eb.fd);
		}
		if (!ok) gpu_release_imports();
		gpu_zero_copy = ok;
		logm("GPU ISP, %s input", ok ? "zero-copy dma-buf" : "uploaded");
	}
#endif
	/* Stream only once the GPU is ready: shader compilation would starve the capture queue. */
	if (capture_streamon() < 0) { capture_stop(); close(pa_fd); pa_fd = -1; return -1; }
	camera_on = 1;
	af_start();
	logm("camera on (exposure %d..%d, gain %d..%d)", exp_min, exp_max, gain_min, gain_max);
	return 0;
}

static void camera_stop(void) {
	capture_stop();
	if (pa_fd >= 0) { close(pa_fd); pa_fd = -1; }
	camera_on = 0;
	logm("camera off");
}

/* ---------------------------------------------------------------- feed (superman-camfeed) */
static int feed_fd = -1;

static unsigned st_drops; static uint64_t st_isp, st_wait; /* per fps window: dropped frames, ms in ISP / in capture wait */
static void fps_tick(void) {
	static uint64_t t0;
	static unsigned n;
	uint64_t t = now_ms();
	if (!t0) t0 = t;
	if (++n >= 150 || t - t0 >= 10000) {
		logm("%.1f fps (exposure %d gain %d wb r%d b%d) isp %.0f ms wait %.0f ms drops %u", n * 1000.0 / (t - t0 ? t - t0 : 1), exp_cur, gain_cur, wb_r, wb_b,
		     (double)st_isp / n, (double)st_wait / n, st_drops);
		st_isp = st_wait = 0; st_drops = 0;
		t0 = t; n = 0;
	}
}

/* Zero-copy output: render straight into the bridge's capture buffers. */
static void feed_out_release(void) {
#ifdef CAMD_GPU
	if (out_zero) gpu_out_release();
#endif
	out_zero = 0;
}

static void feed_out_setup(void) {
	out_zero = 0;
#ifdef CAMD_GPU
	if (!use_gpu) return;
	unsigned i = 0, count = 0;
	int ok = 1;
	do {
		struct camfeed_expbuf e = { .index = i };
		if (ioctl(feed_fd, CAMFEED_IOC_EXPBUF, &e) < 0) { ok = 0; break; }
		count = e.count;
		if (e.size != str_w * str_h * 2 || gpu_out_import(i, e.fd) < 0) ok = 0;
		close(e.fd);
	} while (ok && ++i < count);
	if (!ok) gpu_out_release();
	out_zero = ok && count;
	logm("output: %s (%u buffers)", out_zero ? "zero-copy into the bridge" : "copied", count);
#endif
}

/* returns 1/0 for a pending streaming state change, -1 if none */
static int feed_state(int timeout_ms) {
	struct pollfd p = { .fd = feed_fd, .events = POLLIN };
	if (poll(&p, 1, timeout_ms) <= 0 || !(p.revents & POLLIN)) return -1;
	char c;
	if (read(feed_fd, &c, 1) != 1) return -1;
	return c == '1';
}

static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

static int run_frames(int count, const char *file) {
	if (camera_start() < 0) return 1;
	/* debug: CAMD_FOCUS=<0..1023> drives the lens (and logs the centre sharpness) */
	const char *fe = getenv("CAMD_FOCUS");
	double sharp = 0;
	if (fe) logm("focus %d: %s", atoi(fe), ctl_set(V4L2_CID_FOCUS_ABSOLUTE, atoi(fe)) == 0 ? "set" : "FAILED");
	uint64_t t0 = now_ms();
	int got = 0;
	uint64_t isp_ms = 0;
	while (got < count) {
		struct v4l2_buffer b; struct v4l2_plane pl[1];
		int i = capture_get(2000, &b, pl);
		if (i < 0) { logm("frame timeout"); break; }
		uint64_t t1 = now_ms();
		cur_idx = i;
		af_frame(cap_map[i]);
		if (fe) sharp = af_metric(cap_map[i]);
		if (getenv("CAMD_AFDBG") && got % 5 == 0) logm("frame %d sharpness %.5f", got, af_metric(cap_map[i]));
		isp_frame(cap_map[i], out_frame);
		isp_ms += now_ms() - t1;
		capture_put(&b);
		got++;
	}
	uint64_t dt = now_ms() - t0;
	logm("isp %llu ms/frame", (unsigned long long)(got ? isp_ms / got : 0));
	if (fe) logm("focus %d sharpness %.5f", atoi(fe), sharp);
#ifdef CAMD_GPU
	{ extern double gpu_t[3]; if (got) logm("gpu upload %.1f draw %.1f read %.1f ms", gpu_t[0] / got, gpu_t[1] / got, gpu_t[2] / got); }
#endif
	logm("%d frames in %llu ms; exposure %d gain %d wb r%d b%d", got, (unsigned long long)dt, exp_cur,
	     gain_cur, wb_r, wb_b);
	camera_stop();
	if (got && file) {
		FILE *f = fopen(file, "wb");
		if (f) { fwrite(out_frame, 1, (size_t)str_w * str_h * 2, f); fclose(f); }
	}
	return got ? 0 : 2;
}

int main(int argc, char **argv) {
	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);
	signal(SIGPIPE, SIG_IGN);
	init_lut();
	out_frame = malloc(OUT_W * OUT_H * 2);
	if (argc >= 3 && !strcmp(argv[1], "--file")) {
		if (argc >= 6 && !strcmp(argv[4], "--cam") && !strcmp(argv[5], "front")) cam = &cams[1];
		/* debug: CAMD_SIZE=WxH renders that client size (and picks binned/unbinned like a client would) */
		if (getenv("CAMD_SIZE") && sscanf(getenv("CAMD_SIZE"), "%ux%u", &str_w, &str_h) == 2) { req_w = str_w; req_h = str_h; }
		return run_frames(argc > 3 ? atoi(argv[3]) : 30, argv[2]);
	}

	const char *feed = NULL;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--cam")) {
			for (unsigned k = 0; k < sizeof cams / sizeof cams[0]; k++)
				if (!strcmp(argv[i + 1], cams[k].name)) cam = &cams[k];
		} else if (!strcmp(argv[i], "--feed")) feed = argv[i + 1];
	}
	if (!feed) feed = cam->feed;
	for (int n = 0; (feed_fd = open(feed, O_RDWR | O_CLOEXEC)) < 0; n++) {
		if (n == 50) { logm("open %s: %s", feed, strerror(errno)); return 1; }
		usleep(100000);
	}
	/* allocate the capture buffers now, while CMA is fresh (largest = unbinned window) */
	if (!getenv("CAMD_NOKEEP") && media_open() == 0) {
		req_w = req_h = 65535;
		if (pipeline_setup() == 0) capture_alloc(raw_w, raw_h);
		req_w = req_h = 0;
	}
	logm("waiting for clients on %s (%dx%d YUYV)", feed, OUT_W, OUT_H);
	int want = 0, fails = 0, out_tried = 0;
	unsigned session_frames = 0;
	while (!quit) {
		int st = feed_state(camera_on ? 0 : 1000);
		if (st >= 0) {
			/* Any event is a new session: a stop+start pair can coalesce into "1" again, with new
			 * buffers and maybe a new size, so restart from scratch. */
			logm("client streaming %d -> %d", want, st);
			if (camera_on) { feed_out_release(); camera_stop(); }
			want = st; fails = 0; out_tried = 0;
		}
		if (want && !camera_on) {
			session_frames = 0;
			struct camfeed_fmt fm;
			if (ioctl(feed_fd, CAMFEED_IOC_GETFMT, &fm) == 0 && fm.width && fm.height &&
			    fm.width <= 4096 && fm.height <= 4096) { str_w = req_w = fm.width; str_h = req_h = fm.height; }
			else { str_w = OUT_W; str_h = OUT_H; req_w = req_h = 0; }
			logm("client format %ux%u", str_w, str_h);
			if (camera_start() < 0) {
				camera_stop();
				if (++fails > 5) { logm("camera start keeps failing; waiting for the next client"); want = 0; }
				usleep(500000);
				continue;
			}
		}
		if (!want && camera_on) { feed_out_release(); camera_stop(); }
		if (!camera_on) continue;
		if (want && !out_zero && use_gpu && !out_tried) { feed_out_setup(); out_tried = 1; }
		struct v4l2_buffer b; struct v4l2_plane pl[1];
		/* the first frame after a start can take over a second (sensor PLL/exposure settling) */
		uint64_t tw0 = now_ms();
		int i = capture_get(session_frames ? 1000 : 3000, &b, pl);
		st_wait += now_ms() - tw0;
		if (i >= 0) session_frames++;
		if (i < 0) { logm("frame timeout after %u frames, restarting camera", session_frames); feed_out_release(); camera_stop(); out_tried = 0; continue; }
		cur_idx = i;
		cur_out = -1;
		if (out_zero) {
			struct camfeed_buf a = { .timeout_ms = 30 };
			if (ioctl(feed_fd, CAMFEED_IOC_ACQUIRE, &a) < 0) { capture_put(&b); st_drops++; continue; } /* no empty buffer: drop */
			cur_out = a.index;
			af_frame(cap_map[i]);
			uint64_t ti0 = now_ms();
			isp_frame(cap_map[i], out_frame);
			st_isp += now_ms() - ti0;
			capture_put(&b);
			struct camfeed_buf d = { .index = a.index, .ok = use_gpu };
			ioctl(feed_fd, CAMFEED_IOC_DONE, &d);
			fps_tick();
			if (!use_gpu) { feed_out_release(); } /* GPU failed: back to copies */
			continue;
		}
		af_frame(cap_map[i]);
		isp_frame(cap_map[i], out_frame);
		capture_put(&b);
		if ((use_gpu || (str_w == OUT_W && str_h == OUT_H)) && write(feed_fd, out_frame, str_w * str_h * 2) < 0)
			logm("feed write: %s", strerror(errno));
		fps_tick();
	}
	if (camera_on) camera_stop();
	return 0;
}
