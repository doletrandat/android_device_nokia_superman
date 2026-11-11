// SPDX-License-Identifier: GPL-2.0-only
/*
 * Freestanding V4L2 query for the Lumia 730 Venus bring-up: QUERYCAP and the
 * formats on the coded (OUTPUT_MPLANE) and raw (CAPTURE_MPLANE) queues.
 * Usage: v4l2q /dev/videoN
 */
typedef unsigned char u8;
typedef unsigned int u32;

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
#define SYS_write 4
#define SYS_ioctl 54
#define SYS_openat 322

void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = c; return d; }
int raise(int sig) { sys(SYS_exit, 128 + sig, 0, 0, 0, 0, 0); return 0; }

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { sys(SYS_write, 1, (long)s, slen(s), 0, 0, 0); }
static void hex(u32 v)
{
	char b[11] = "0x00000000";
	int i;
	for (i = 0; i < 8; i++)
		b[9 - i] = "0123456789abcdef"[(v >> (i * 4)) & 0xf];
	say(b);
}
static void fourcc(u32 v)
{
	char b[5] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24, 0 };
	say(b);
}

struct cap { char driver[16], card[32], bus[32]; u32 version, caps, device_caps, rsv[3]; };
struct fmtdesc { u32 index, type, flags; char desc[32]; u32 pixfmt, mbus, rsv[3]; };
#define QUERYCAP 0x80685600
#define ENUM_FMT 0xc0405602

static void list(long fd, u32 type, const char *what)
{
	struct fmtdesc f;
	u32 i;

	say(what);
	say(":\n");
	for (i = 0; i < 32; i++) {
		memset(&f, 0, sizeof(f));
		f.index = i;
		f.type = type;
		if (sys(SYS_ioctl, fd, ENUM_FMT, (long)&f, 0, 0, 0))
			break;
		say("  ");
		fourcc(f.pixfmt);
		say("  ");
		f.desc[31] = 0;
		say(f.desc);
		say("\n");
	}
}

void __attribute__((naked)) _start(void) { asm volatile("mov r0, sp\n bl main2\n"); }

void main2(long *sp)
{
	char **argv = (char **)(sp + 1);
	struct cap c;
	long fd;

	if (sp[0] < 2)
		sys(SYS_exit, 2, 0, 0, 0, 0, 0);
	fd = sys(SYS_openat, -100, (long)argv[1], 2, 0, 0, 0);
	if (fd < 0) { say("open failed\n"); sys(SYS_exit, 3, 0, 0, 0, 0, 0); }
	memset(&c, 0, sizeof(c));
	if (sys(SYS_ioctl, fd, QUERYCAP, (long)&c, 0, 0, 0)) { say("QUERYCAP failed\n"); sys(SYS_exit, 4, 0, 0, 0, 0, 0); }
	c.card[31] = 0; c.driver[15] = 0;
	say("driver "); say(c.driver); say(" card "); say(c.card);
	say(" device_caps "); hex(c.device_caps); say("\n");
	list(fd, 10, "coded input (OUTPUT_MPLANE)");
	list(fd, 9, "raw frames (CAPTURE_MPLANE)");
	sys(SYS_exit, 0, 0, 0, 0, 0, 0);
}
