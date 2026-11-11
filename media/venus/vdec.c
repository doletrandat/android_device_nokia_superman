// SPDX-License-Identifier: GPL-2.0-only
/*
 * Freestanding V4L2 stateful decode test for the Lumia 730 Venus bring-up.
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
#define DQEVENT ((2u << 30) | (sizeof(struct event) << 16) | ('V' << 8) | 89)
#define EV_SOURCE_CHANGE 5
#define T_OUT 10	/* VIDEO_OUTPUT_MPLANE: coded input */
#define T_CAP 9		/* VIDEO_CAPTURE_MPLANE: decoded frames */
#define MMAP 1
#define F_LAST 0x00100000
#define H264 0x34363248
#define NV12 0x3231564e

#define NOUT 4
#define NCAP 8
static u8 file[24 << 20];
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

static long qbuf(u32 type, u32 idx, u32 used)
{
	struct buffer b; struct plane p[2];
	memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
	b.index = idx; b.type = type; b.memory = MMAP; b.planes = p; b.length = 1;
	p[0].bytesused = used;
	return ioc(QBUF, &b);
}

static u32 fnv(u32 hsh, const u8 *p, u32 n) { while (n--) hsh = (hsh ^ *p++) * 16777619u; return hsh; }
static void hex(u32 v) { char b[9]; int i; for (i = 7; i >= 0; i--, v >>= 4) b[i] = "0123456789abcdef"[v & 15]; b[8] = 0; say(b); }
static long now_ms(void) { long t[2]; sys(SYS_clock_gettime, 1, (long)t, 0, 0, 0, 0); return t[0] * 1000 + t[1] / 1000000; }
static void stop_cmd(void) { u32 c[18]; memset(c, 0, sizeof(c)); c[0] = 1; if (ioc(DECODER_CMD, c)) die("decoder_cmd stop", 0); }

void __attribute__((naked)) _start(void) { asm volatile("mov r0, sp\n bl main2\n"); }

void main2(long *sp)
{
	char **argv = (char **)(sp + 1);
	struct fmt_mp f; struct reqbufs rb; struct buffer b; struct plane p[2];
	struct pollfd pfd;
	long in, out, n, flen = 0, pos = 0;
	u32 w, h, i, type, frames = 0, fed = 0, stride = 0, sh, idle = 0;
	int last = 0, capture_on = 0, hashonly, notouch, stopped = 0;
	u32 naus = 0; long t0, t1;
	u32 ncap = 0;
	struct event_sub sub;
	struct event ev;

	if (sp[0] < 6) die("usage", sp[0]);
	w = atoi(argv[4]); h = atoi(argv[5]);
	in = sys(SYS_openat, -100, (long)argv[2], 0, 0, 0, 0);
	if (in < 0) die("open input", in);
	while ((n = sys(SYS_read, in, (long)file + flen, sizeof(file) - flen, 0, 0, 0)) > 0) flen += n;
	for (pos = 0; pos + 4 <= flen; naus++)
		pos += 4 + (file[pos] | file[pos + 1] << 8 | file[pos + 2] << 16 | file[pos + 3] << 24);
	pos = 0;
	hashonly = argv[3][0] == '-' && !argv[3][1];
	notouch = argv[3][0] == 'n' && !argv[3][1];	/* "n": count frames only (timing) */
	hashonly |= notouch;
	out = hashonly ? -1 : sys(SYS_openat, -100, (long)argv[3], 01 | 0100 | 01000, 0644, 0, 0);
	if (!hashonly && out < 0) die("open output", out);
	fd = sys(SYS_openat, -100, (long)argv[1], 2 | 04000, 0, 0, 0);	/* O_RDWR|O_NONBLOCK */
	if (fd < 0) die("open video", fd);

	memset(&f, 0, sizeof(f)); f.type = T_OUT; f.width = w; f.height = h;
	f.pixelformat = H264; f.num_planes = 1; f.plane[0].sizeimage = 1024 * 1024;
	if (ioc(S_FMT, &f)) die("s_fmt out", 0);
	memset(&rb, 0, sizeof(rb)); rb.count = NOUT; rb.type = T_OUT; rb.memory = MMAP;
	if (ioc(REQBUFS, &rb)) die("reqbufs out", 0);
	query_map(T_OUT, NOUT, outmem, outlen);
	memset(&sub, 0, sizeof(sub)); sub.type = EV_SOURCE_CHANGE;
	if (ioc(SUBSCRIBE_EVENT, &sub)) die("subscribe", 0);

	/* Stateful decode: start the coded queue, wait for the source change. */
	for (i = 0; i < NOUT && pos + 4 <= flen; i++) {
		u32 len = file[pos] | file[pos + 1] << 8 | file[pos + 2] << 16 | file[pos + 3] << 24;
		memcpy(outmem[i], file + pos + 4, len); pos += 4 + len;
		if (qbuf(T_OUT, i, len)) die("qbuf out", i);
		fed++;
	}
	type = T_OUT; if (ioc(STREAMON, &type)) die("streamon out", 0);
	t0 = now_ms();

	while (!last && idle < 50) {
		pfd.fd = fd; pfd.events = 0x1 | 0x4 | 0x2; pfd.revents = 0;	/* POLLIN|POLLOUT|POLLPRI */
		if (sys(SYS_poll, (long)&pfd, 1, 200, 0, 0, 0) <= 0) { idle++; continue; }
		idle = 0;
		if (pfd.revents & 0x2) {
			memset(&ev, 0, sizeof(ev));
			if (ioc(DQEVENT, &ev)) die("dqevent", 0);
			say("event type "); num(ev.type); say("\n");
			if (!capture_on) {
				memset(&f, 0, sizeof(f)); f.type = T_CAP;
				if (ioc(G_FMT, &f)) die("g_fmt cap", 0);
				f.pixelformat = NV12;
				if (ioc(S_FMT, &f)) die("s_fmt cap", 0);
				stride = f.plane[0].bytesperline; sh = f.plane[0].sizeimage;
				say("capture "); num(f.width); say("x"); num(f.height); say(" stride ");
				num(stride); say(" sizeimage "); num(sh); say("\n");
				memset(&rb, 0, sizeof(rb)); rb.count = NCAP; rb.type = T_CAP; rb.memory = MMAP;
				if (ioc(REQBUFS, &rb)) die("reqbufs cap", 0);
				ncap = rb.count > NCAP ? NCAP : rb.count;
				query_map(T_CAP, ncap, capmem, caplen);
				for (i = 0; i < ncap; i++) if (qbuf(T_CAP, i, 0)) die("qbuf cap", i);
				type = T_CAP; if (ioc(STREAMON, &type)) die("streamon cap", 0);
				capture_on = 1;
			}
		}
		/* Recycle consumed coded buffers with the next access unit. */
		memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
		b.type = T_OUT; b.memory = MMAP; b.planes = p; b.length = 2;
		if (!ioc(DQBUF, &b) && pos + 4 <= flen) {
			u32 len = file[pos] | file[pos + 1] << 8 | file[pos + 2] << 16 | file[pos + 3] << 24;
			memcpy(outmem[b.index], file + pos + 4, len); pos += 4 + len;
			if (qbuf(T_OUT, b.index, len)) die("requeue out", b.index);
			fed++;
		}
		if (capture_on && !stopped && fed == naus) { stop_cmd(); stopped = 1; }
		if (!capture_on)
			continue;
		memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
		b.type = T_CAP; b.memory = MMAP; b.planes = p; b.length = 2;
		if (!ioc(DQBUF, &b)) {
			if (p[0].bytesused) {
				u8 *m = capmem[b.index], *row; u32 y, hv = 2166136261u;
				for (y = 0; !notouch && y < h + h / 2; y++) {	/* luma, then chroma after the aligned luma plane */
					row = y < h ? m + y * stride : m + stride * ((h + 31) & ~31u) + (y - h) * stride;
					if (hashonly) hv = fnv(hv, row, w);
					else sys(SYS_write, out, (long)row, w, 0, 0, 0);
				}
				if (hashonly && !notouch) { say("frame "); num(frames); say(" "); hex(hv); say("\n"); }
				frames++;
			}
			last = !!(b.flags & F_LAST);
			if (!last && qbuf(T_CAP, b.index, 0)) die("requeue cap", b.index);
		}
	}
	t1 = now_ms();
	say("fed "); num(fed); say(" decoded "); num(frames); say(last ? " last" : "");
	say(" ms "); num(t1 - t0); say(" fps*10 "); num(t1 > t0 ? frames * 10000L / (t1 - t0) : 0); say("\n");
	sys(SYS_exit, frames == naus ? 0 : 5, 0, 0, 0, 0, 0);
}
