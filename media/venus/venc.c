// SPDX-License-Identifier: GPL-2.0-only
/*
 * Freestanding V4L2 stateful ENCODE test for the Lumia 730 Venus bring-up.
 * Usage: venc /dev/videoN in.nv12 out.h264 width height bitrate_bps [h264_level_enum]
 * in.nv12: tightly packed NV12 frames. Writes the H.264 elementary stream.
 * (Prelude shared with vdec.c.)
 * Input: <len u32 LE><access unit> records (one H.264 AU per OUTPUT buffer).
 * Usage: vdec /dev/video0 in.au out.nv12|- width height
 * Writes every decoded NV12 frame (cropped to width x height) to out.nv12, or with "-"
 * prints a per-frame FNV-1a hash of the same bytes. After the last AU it sends
 * V4L2_DEC_CMD_STOP and drains until the LAST buffer.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;

static long sys(long n, long a, long b, long c, long d, long e, long f)
{
	register long r7 asm("r7") = n;
	register long r0 asm("r0") = a;
	register long r1 asm("r1") = b;
	register long r2 asm("r2") = c;
	register long r3 asm("r3") = d;
	register long r4 asm("r4") = e;
	register long r5 asm("r5") = f;
	asm volatile("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2), "r"(r3),
		     "r"(r4), "r"(r5) : "memory");
	return r0;
}
#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_close 6
#define SYS_ioctl 54
#define SYS_poll 168
#define SYS_mmap2 192
#define SYS_openat 322
#define SYS_clock_gettime 263

void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
int raise(int sig) { sys(SYS_exit, 128 + sig, 0, 0, 0, 0, 0); return 0; }

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { sys(SYS_write, 1, (long)s, slen(s), 0, 0, 0); }
static void num(long v)
{
	char b[24]; int i = 23, neg = v < 0;
	b[i] = 0; if (neg) v = -v;
	do { b[--i] = '0' + v % 10; v /= 10; } while (v);
	if (neg) b[--i] = '-';
	say(b + i);
}
static long atoi(const char *s) { long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0'; return v; }
static void die(const char *w, long rc) { say("FAIL "); say(w); say(" rc="); num(rc); say("\n"); sys(SYS_exit, 1, 0, 0, 0, 0, 0); }

/* 32-bit ARM UAPI layouts (time32 variants of the buffer ioctls). */
struct plane_pix { u32 sizeimage, bytesperline; u16 rsv[6]; };
struct fmt_mp {
	u32 type;
	u32 width, height, pixelformat, field, colorspace;
	struct plane_pix plane[8];
	u8 num_planes, flags, ycbcr, quant, xfer, rsv[7];
	u8 pad[200 - 192];
};
struct reqbufs { u32 count, type, memory, caps; u8 flags, rsv[3]; };
struct plane { u32 bytesused, length, mem_offset, data_offset, rsv[11]; };
struct buffer {
	u32 index, type, bytesused, flags, field;
	long tv_sec, tv_usec;
	u32 timecode[4];
	u32 sequence, memory;
	struct plane *planes;
	u32 length, rsv2, request_fd;
};
struct pollfd { int fd; short events, revents; };
struct event_sub { u32 type, id, flags, rsv[5]; };
/* v4l2_event_time32: the union holds __s64 fields, so it starts at offset 8. */
struct event { u32 type, pad; u8 u[64]; u32 pending, sequence; s32 ts_sec, ts_nsec; u32 id, rsv[8]; u32 tail; };

#define IOWR(nr, sz) ((3u << 30) | ((sz) << 16) | ('V' << 8) | (nr))
#define IOW(nr, sz) ((1u << 30) | ((sz) << 16) | ('V' << 8) | (nr))
#define S_FMT IOWR(5, sizeof(struct fmt_mp))
#define G_FMT IOWR(4, sizeof(struct fmt_mp))
#define REQBUFS IOWR(8, sizeof(struct reqbufs))
#define QUERYBUF IOWR(9, sizeof(struct buffer))
#define QBUF IOWR(15, sizeof(struct buffer))
#define DQBUF IOWR(17, sizeof(struct buffer))
#define STREAMON IOW(18, 4)
#define SUBSCRIBE_EVENT IOW(90, sizeof(struct event_sub))
#define DECODER_CMD IOWR(96, 72)
#define ENCODER_CMD IOWR(77, 40)
#define S_CTRL IOWR(28, 8)
#define S_PARM IOWR(22, 204)
#define S_SELECTION IOWR(95, 64)
#define DQEVENT ((2u << 30) | (sizeof(struct event) << 16) | ('V' << 8) | 89)
#define EV_SOURCE_CHANGE 5
#define T_OUT 10	/* VIDEO_OUTPUT_MPLANE: coded input */
#define T_CAP 9		/* VIDEO_CAPTURE_MPLANE: decoded frames */
#define MMAP 1
#define F_LAST 0x00100000
#define H264 0x34363248
#define NV12 0x3231564e

#define NOUT 4
#define NCAP 4
#define CID_BITRATE 0x009909cf
static u8 frame[1920 * 1088 * 3 / 2];
static u8 *outmem[NOUT], *capmem[NCAP];
static u32 outlen[NOUT], caplen[NCAP];

static long fd;
static long ioc(u32 req, void *arg) { return sys(SYS_ioctl, fd, req, (long)arg, 0, 0, 0); }

static void query_map(u32 type, u32 n, u8 **mem, u32 *len)
{
	struct buffer b; struct plane p[2]; u32 i;
	for (i = 0; i < n; i++) {
		memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
		b.index = i; b.type = type; b.memory = MMAP; b.planes = p; b.length = 2;
		if (ioc(QUERYBUF, &b)) die("querybuf", type);
		len[i] = p[0].length;
		mem[i] = (u8 *)sys(SYS_mmap2, 0, p[0].length, 3, 1, fd, p[0].mem_offset >> 12);
		if ((unsigned long)mem[i] > 0xfffff000ul) die("mmap", type);
	}
}

static u32 seq;
static long qbuf(u32 type, u32 idx, u32 used)
{
	struct buffer b; struct plane p[2];
	memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
	if (type == T_OUT) {	/* 30 fps timestamps; the encoder's rate control needs them */
		u32 us = seq++ * 33333u;
		b.tv_sec = us / 1000000u; b.tv_usec = us % 1000000u;
		b.flags = 0x4000;	/* V4L2_BUF_FLAG_TIMESTAMP_COPY */
	}
	b.index = idx; b.type = type; b.memory = MMAP; b.planes = p; b.length = 1;
	p[0].bytesused = used;
	return ioc(QBUF, &b);
}

static long now_ms(void) { long t[2]; sys(SYS_clock_gettime, 1, (long)t, 0, 0, 0, 0); return t[0] * 1000 + t[1] / 1000000; }
static void cpy4(u8 *d, const u8 *s, u32 n) { u32 *dd = (u32 *)d; const u32 *ss = (const u32 *)s; n >>= 2; while (n--) *dd++ = *ss++; }

static long in;
static u32 w, h, stride, fsz, outsize;
/* Read the next frame into an OUTPUT buffer; 0 at end of input. */
static int fill(u32 idx)
{
	long n = 0, r; u32 y;
	while (n < fsz && (r = sys(SYS_read, in, (long)frame + n, fsz - n, 0, 0, 0)) > 0) n += r;
	if (n < fsz) return 0;
	for (y = 0; y < h; y++) cpy4(outmem[idx] + y * stride, frame + y * w, w);
	for (y = 0; y < h / 2; y++)
		cpy4(outmem[idx] + stride * ((h + 31) & ~31u) + y * stride, frame + w * h + y * w, w);
	if (qbuf(T_OUT, idx, outsize)) die("qbuf out", idx);
	return 1;
}

void __attribute__((naked)) _start(void) { asm volatile("mov r0, sp\n bl main2\n"); }

void main2(long *sp)
{
	char **argv = (char **)(sp + 1);
	struct fmt_mp f; struct reqbufs rb; struct buffer b; struct plane p[2];
	struct pollfd pfd; u32 ctl[2], cmd[10], parm[51];
	long out, t0, t1, bytes = 0;
	u32 i, type, fed = 0, frames = 0, idle = 0, keyf = 0, empty = 0;
	int last = 0, eof = 0, stopped = 0;

	if (sp[0] < 7) die("usage", sp[0]);
	w = atoi(argv[4]); h = atoi(argv[5]); fsz = w * h * 3 / 2;
	if (fsz > sizeof(frame)) die("too big", fsz);
	in = sys(SYS_openat, -100, (long)argv[2], 0, 0, 0, 0);
	if (in < 0) die("open input", in);
	out = sys(SYS_openat, -100, (long)argv[3], 01 | 0100 | 01000, 0644, 0, 0);
	if (out < 0) die("open output", out);
	fd = sys(SYS_openat, -100, (long)argv[1], 2 | 04000, 0, 0, 0);
	if (fd < 0) die("open video", fd);

	memset(&f, 0, sizeof(f)); f.type = T_CAP; f.width = w; f.height = h;
	f.pixelformat = H264; f.num_planes = 1;
	if (ioc(S_FMT, &f)) die("s_fmt cap", 0);
	memset(&f, 0, sizeof(f)); f.type = T_OUT; f.width = w; f.height = h;
	f.pixelformat = NV12; f.num_planes = 1;
	if (ioc(S_FMT, &f)) die("s_fmt out", 0);
	stride = f.plane[0].bytesperline; outsize = f.plane[0].sizeimage;
	say("raw "); num(f.width); say("x"); num(f.height); say(" stride "); num(stride);
	say(" sizeimage "); num(outsize); say("\n");
	{	/* visible size = crop on the (padded) raw queue */
		u32 sel[16]; long r;
		memset(sel, 0, sizeof(sel)); sel[0] = 2; sel[1] = 0;	/* VIDEO_OUTPUT, TGT_CROP */
		sel[5] = w; sel[6] = h;
		r = ioc(S_SELECTION, sel);
		if (r || sel[5] != w || sel[6] != h) die("s_selection", r ? r : sel[5]);
	}
	memset(parm, 0, sizeof(parm)); parm[0] = T_OUT; parm[3] = 1; parm[4] = 30;	/* 30 fps */
	if (ioc(S_PARM, parm)) say("s_parm failed (ignored)\n");
	ctl[0] = CID_BITRATE; ctl[1] = atoi(argv[6]);
	if (ioc(S_CTRL, ctl)) die("bitrate", ctl[1]);
	if (sp[0] > 7) {	/* optional V4L2_MPEG_VIDEO_H264_LEVEL enum, e.g. 11 = 4.0 */
		ctl[0] = 0x00990a67; ctl[1] = atoi(argv[7]);
		if (ioc(S_CTRL, ctl)) die("level", ctl[1]);
	}

	memset(&rb, 0, sizeof(rb)); rb.count = NOUT; rb.type = T_OUT; rb.memory = MMAP;
	{ long r = ioc(REQBUFS, &rb); if (r) die("reqbufs out", r); }
	if (rb.count < NOUT) die("out count", rb.count);
	query_map(T_OUT, NOUT, outmem, outlen);
	memset(&rb, 0, sizeof(rb)); rb.count = NCAP; rb.type = T_CAP; rb.memory = MMAP;
	{ long r = ioc(REQBUFS, &rb); if (r) die("reqbufs cap", r); }
	if (rb.count < NCAP) die("cap count", rb.count);
	query_map(T_CAP, NCAP, capmem, caplen);
	for (i = 0; i < NCAP; i++) if (qbuf(T_CAP, i, 0)) die("qbuf cap", i);

	t0 = now_ms();
	for (i = 0; i < NOUT && !eof; i++) { if (fill(i)) fed++; else eof = 1; }
	type = T_OUT; if (ioc(STREAMON, &type)) die("streamon out", 0);
	type = T_CAP; if (ioc(STREAMON, &type)) die("streamon cap", 0);

	while (!last && idle < 25) {
		if (eof && !stopped) {
			memset(cmd, 0, sizeof(cmd)); cmd[0] = 1;	/* V4L2_ENC_CMD_STOP */
			if (ioc(ENCODER_CMD, cmd)) die("encoder_cmd stop", 0);
			stopped = 1;
		}
		pfd.fd = fd; pfd.events = 0x1 | 0x4; pfd.revents = 0;
		if (sys(SYS_poll, (long)&pfd, 1, 200, 0, 0, 0) <= 0) { idle++; continue; }
		idle = 0;
		memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
		b.type = T_OUT; b.memory = MMAP; b.planes = p; b.length = 2;
		if (!ioc(DQBUF, &b) && !eof) { if (fill(b.index)) fed++; else eof = 1; }
		memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
		b.type = T_CAP; b.memory = MMAP; b.planes = p; b.length = 2;
		if (!ioc(DQBUF, &b)) {
			u32 n = p[0].bytesused - p[0].data_offset;
			if (!n) empty++;
			if (n) {
				sys(SYS_write, out, (long)(capmem[b.index] + p[0].data_offset), n, 0, 0, 0);
				bytes += n; frames++;
				if (b.flags & 0x8) keyf++;	/* V4L2_BUF_FLAG_KEYFRAME */
			}
			last = !!(b.flags & F_LAST);
			if (!last && qbuf(T_CAP, b.index, 0)) die("requeue cap", b.index);
		}
	}
	t1 = now_ms();
	say("fed "); num(fed); say(" encoded "); num(frames); say(" key "); num(keyf); say(" empty "); num(empty);
	say(" bytes "); num(bytes); say(last ? " last" : " nolast"); say(" ms "); num(t1 - t0);
	say(" fps*10 "); num(t1 > t0 ? frames * 10000L / (t1 - t0) : 0); say("\n");
	sys(SYS_exit, frames && frames >= fed - 1 ? 0 : 5, 0, 0, 0, 0, 0);
}
