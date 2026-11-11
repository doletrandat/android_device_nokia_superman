// SPDX-License-Identifier: Apache-2.0
/*
 * superman-gpt: re-partition the Lumia 730 eMMC for an internal Android install (layout "option B").
 *
 * Only the Windows Phone "Data" partition's space is reused; every other partition (firmware, calibration, DPP,
 * EFIESP, MainOS ...) keeps its entry byte for byte, which keeps a stock FFU / WPinternals restore possible.
 *   entry 27 "Data" -> "cache"          64 MiB  ext2 LUMIA_BOOT with boot.img (the name lk2nd fsboot scans on the eMMC)
 *   entry 30 (free) -> "android_system"  2 GiB  ext4 system
 *   entry 31 (free) -> "android_data"   rest    ext4 userdata
 * Unique GUIDs are UUIDv5(disk GUID, "superman-android/<name>"), so a re-plan of the same phone gives the same table.
 *
 *   superman-gpt status  DISK          layout=windows|android|other, and the four entries involved
 *   superman-gpt plan    DISK DIR      validate the Windows Phone GPT, write DIR/{old,new}-{primary,backup}.bin and
 *                                      DIR/plan.env (sector numbers, shell syntax); writes nothing to DISK
 *   superman-gpt write   DISK DIR      DISK's GPT must still equal DIR/old-*; write DIR/new-*, flush, read back
 *   superman-gpt restore DISK DIR      DISK's GPT must equal DIR/new-*; write DIR/old-* back, flush, read back
 *   superman-gpt loop    DISK FIRST LAST   attach a loop device to sectors FIRST..LAST of DISK, print its path
 *                                      (64-bit offsets: toybox losetup fails above 2 GiB on 32-bit Android)
 * primary = LBA 0..33 (protective MBR, header, 32 entry sectors), backup = the last 33 LBAs. 512-byte sectors.
 * Builds with the kernel's nolibc (static ARM, no Android libraries) or with a normal libc for host tests.
 */
#ifndef NOLIBC
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#include <linux/loop.h>

#define SECTOR 512
#define PRI_SECTORS 34
#define BAK_SECTORS 33
#define MiB_SECTORS 2048ULL

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

static u8 pri[PRI_SECTORS * SECTOR], bak[BAK_SECTORS * SECTOR];
static u8 npri[PRI_SECTORS * SECTOR], nbak[BAK_SECTORS * SECTOR];
static u8 chk[PRI_SECTORS * SECTOR];
static u64 total;                       /* disk size in sectors */

static char *cat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
static void out(const char *s) { write(1, s, strlen(s)); }
static int fail(const char *s) { write(2, "superman-gpt: ", 14); write(2, s, strlen(s)); write(2, "\n", 1); return 1; }
static void outu(u64 v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); out(b + i); }

/* ---- 64-bit positioned I/O (ARM nolibc has a 32-bit off_t) ---- */
static int seek64(int fd, u64 off, int whence, u64 *res)
{
#ifdef NOLIBC
	long long r = 0;
	long rc = my_syscall5(__NR__llseek, fd, (long)(off >> 32), (long)(off & 0xffffffffu), &r, whence);
	if (rc) return -1;
	if (res) *res = (u64)r;
	return 0;
#else
	off_t r = lseek(fd, (off_t)off, whence);
	if (r < 0) return -1;
	if (res) *res = (u64)r;
	return 0;
#endif
}
static int rd(int fd, u64 lba, u8 *buf, u32 n)
{
	u32 got = 0;
	if (seek64(fd, lba * SECTOR, SEEK_SET, 0)) return -1;
	while (got < n) { long r = read(fd, buf + got, n - got); if (r <= 0) return -1; got += r; }
	return 0;
}
static int wr(int fd, u64 lba, const u8 *buf, u32 n)
{
	u32 put = 0;
	if (seek64(fd, lba * SECTOR, SEEK_SET, 0)) return -1;
	while (put < n) { long r = write(fd, buf + put, n - put); if (r <= 0) return -1; put += r; }
	return 0;
}
static void flush(int fd)
{
#ifdef NOLIBC
	my_syscall1(__NR_fsync, fd);
#else
	fsync(fd);
#endif
	ioctl(fd, 0x1261, 0);           /* BLKFLSBUF: drop the cache so the read-back comes from the device */
}

/* ---- CRC32 (IEEE) and SHA-1 (for UUIDv5) ---- */
static u32 crc32(const u8 *p, u32 n)
{
	u32 c = 0xffffffffu, k;
	while (n--) { c ^= *p++; for (k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1; }
	return ~c;
}
static u32 rol(u32 x, int n) { return x << n | x >> (32 - n); }
static void sha1(const u8 *msg, u32 len, u8 *dig)
{
	u32 h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
	u8 blk[64]; u32 w[80], i, j, done = 0, total_blocks = (len + 8) / 64 + 1;
	for (j = 0; j < total_blocks; j++) {
		for (i = 0; i < 64; i++) {
			u32 pos = j * 64 + i;
			blk[i] = pos < len ? msg[pos] : pos == len ? 0x80 : 0;
		}
		if (j == total_blocks - 1) {
			u64 bits = (u64)len * 8;
			for (i = 0; i < 8; i++) blk[63 - i] = (u8)(bits >> (8 * i));
		}
		for (i = 0; i < 16; i++) w[i] = (u32)blk[4 * i] << 24 | blk[4 * i + 1] << 16 | blk[4 * i + 2] << 8 | blk[4 * i + 3];
		for (i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f, k, t;
		for (i = 0; i < 80; i++) {
			if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
			else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
			else { f = b ^ c ^ d; k = 0xca62c1d6; }
			t = rol(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol(b, 30); b = a; a = t;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
	}
	(void)done;
	for (i = 0; i < 20; i++) dig[i] = (u8)(h[i / 4] >> (24 - 8 * (i % 4)));
}
/* GUID bytes in GPT (little-endian "bytes_le") <-> RFC 4122 big-endian order: swap the first three fields */
static void le_be(const u8 *in, u8 *o)
{
	o[0] = in[3]; o[1] = in[2]; o[2] = in[1]; o[3] = in[0]; o[4] = in[5]; o[5] = in[4]; o[6] = in[7]; o[7] = in[6];
	memcpy(o + 8, in + 8, 8);
}
static void uuid5(const u8 *ns_le, const char *name, u8 *res_le)
{
	u8 buf[16 + 64], dig[20], be[16];
	u32 n = strlen(name);
	le_be(ns_le, buf); memcpy(buf + 16, name, n);
	sha1(buf, 16 + n, dig);
	memcpy(be, dig, 16);
	be[6] = (be[6] & 0x0f) | 0x50; be[8] = (be[8] & 0x3f) | 0x80;
	le_be(be, res_le);              /* the swap is its own inverse */
}

/* ---- GPT parsing ---- */
static u32 g32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u64 g64(const u8 *p) { return g32(p) | (u64)g32(p + 4) << 32; }
static void p32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void p64(u8 *p, u64 v) { p32(p, (u32)v); p32(p + 4, (u32)(v >> 32)); }

struct hdr { u64 my, alt, first, last, elba; u32 count, rev; const u8 *disk; };
static int parse_hdr(const u8 *h, const u8 *ents, struct hdr *o)
{
	u8 tmp[92];
	if (memcmp(h, "EFI PART", 8) || g32(h + 12) != 92 || g32(h + 84) != 128) return -1;
	memcpy(tmp, h, 92); p32(tmp + 16, 0);
	if (crc32(tmp, 92) != g32(h + 16)) return -1;
	o->rev = g32(h + 8); o->my = g64(h + 24); o->alt = g64(h + 32); o->first = g64(h + 40); o->last = g64(h + 48);
	o->disk = h + 56; o->elba = g64(h + 72); o->count = g32(h + 80);
	if (o->count == 0 || o->count > 128) return -1;
	if (crc32(ents, o->count * 128) != g32(h + 88)) return -1;
	return 0;
}
static const u8 *ent(const u8 *ents, int idx) { return ents + (idx - 1) * 128; }
static int is_empty(const u8 *e) { int i; for (i = 0; i < 16; i++) if (e[i]) return 0; return 1; }
static int name_is(const u8 *e, const char *s)
{
	u32 i, n = strlen(s);
	for (i = 0; i < 36; i++) {
		unsigned c = e[56 + 2 * i] | e[57 + 2 * i] << 8;
		if (i < n ? c != (u8)s[i] : c != 0) return 0;
		if (i >= n) break;
	}
	return 1;
}
static void put_name(u8 *e, const char *s) { u32 i; memset(e + 56, 0, 72); for (i = 0; s[i]; i++) e[56 + 2 * i] = s[i]; }

static int load(const char *disk, int flags, int *pfd)
{
	int fd = open(disk, flags);
	if (fd < 0) return fail("cannot open the disk");
	if (seek64(fd, 0, SEEK_END, &total) || total % SECTOR) { close(fd); return fail("cannot get the disk size"); }
	total /= SECTOR;
	if (total < 4 * 1024 * 1024 || rd(fd, 0, pri, sizeof pri) || rd(fd, total - BAK_SECTORS, bak, sizeof bak)) {
		close(fd); return fail("cannot read the partition tables");
	}
	*pfd = fd;
	return 0;
}
/* both copies valid, at the right places, with identical entries */
static int check_tables(const u8 *p, const u8 *b, struct hdr *hp)
{
	struct hdr hb;
	if (parse_hdr(p + SECTOR, p + 2 * SECTOR, hp)) return fail("primary GPT header or entries invalid");
	if (parse_hdr(b + 32 * SECTOR, b, &hb)) return fail("backup GPT header or entries invalid");
	if (hp->my != 1 || hp->alt != total - 1 || hp->elba != 2) return fail("primary GPT not where expected");
	if (hb.my != total - 1 || hb.alt != 1 || hb.elba != total - BAK_SECTORS) return fail("backup GPT not where expected");
	if (memcmp(p + 2 * SECTOR, b, 32 * SECTOR) || memcmp(hp->disk, hb.disk, 16) || hp->count != hb.count)
		return fail("primary and backup GPT differ");
	if (p[510] != 0x55 || p[511] != 0xaa || p[450] != 0xee) return fail("no protective MBR");
	return 0;
}
static const char *layout(const u8 *ents)
{
	if (name_is(ent(ents, 26), "MainOS") && name_is(ent(ents, 27), "Data") && is_empty(ent(ents, 30)) && is_empty(ent(ents, 31)))
		return "windows";
	if (name_is(ent(ents, 26), "MainOS") && name_is(ent(ents, 27), "cache") && name_is(ent(ents, 30), "android_system")
	    && name_is(ent(ents, 31), "android_data"))
		return "android";
	return "other";
}
static void build_hdr(u8 *dst, const struct hdr *h, const u8 *ents, u64 my, u64 alt, u64 elba)
{
	memset(dst, 0, SECTOR);
	memcpy(dst, "EFI PART", 8); p32(dst + 8, h->rev); p32(dst + 12, 92);
	p64(dst + 24, my); p64(dst + 32, alt); p64(dst + 40, h->first); p64(dst + 48, h->last);
	memcpy(dst + 56, h->disk, 16); p64(dst + 72, elba); p32(dst + 80, h->count); p32(dst + 84, 128);
	p32(dst + 88, crc32(ents, h->count * 128));
	p32(dst + 16, crc32(dst, 92));
}

static int save(const char *dir, const char *name, const u8 *buf, u32 n)
{
	char path[512]; u32 l = strlen(dir);
	if (l + strlen(name) + 2 > sizeof path) return fail("path too long");
	memcpy(path, dir, l); path[l] = '/'; strcpy(path + l + 1, name);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || write(fd, buf, n) != (long)n) return fail("cannot write the plan files");
	close(fd);
	return 0;
}
static int loadf(const char *dir, const char *name, u8 *buf, u32 n)
{
	char path[512]; u32 l = strlen(dir), got = 0;
	if (l + strlen(name) + 2 > sizeof path) return fail("path too long");
	memcpy(path, dir, l); path[l] = '/'; strcpy(path + l + 1, name);
	int fd = open(path, O_RDONLY);
	if (fd < 0) return fail("cannot read the plan files (run plan first)");
	while (got < n) { long r = read(fd, buf + got, n - got); if (r <= 0) break; got += r; }
	close(fd);
	return got == n ? 0 : fail("plan file has the wrong size");
}

static int cmd_status(const char *disk)
{
	int fd; struct hdr h;
	if (load(disk, O_RDONLY, &fd)) return 1;
	close(fd);
	if (check_tables(pri, bak, &h)) return 1;
	out("layout="); out(layout(pri + 2 * SECTOR)); out("\ndisk_sectors="); outu(total);
	out("\nprotective_mbr="); outu(g32(pri + 446 + 8)); out("+"); outu(g32(pri + 446 + 12));
	out(g32(pri + 446 + 8) + (u64)g32(pri + 446 + 12) > total ? " (past the end: lk2nd ignores the GPT)\n" : " (ok)\n");
	static const int idx[] = { 26, 27, 30, 31 };
	for (int i = 0; i < 4; i++) {
		const u8 *e = ent(pri + 2 * SECTOR, idx[i]);
		char nm[37]; int k;
		for (k = 0; k < 36 && (e[56 + 2 * k] | e[57 + 2 * k]); k++) nm[k] = e[56 + 2 * k];
		nm[k] = 0;
		out("entry "); outu(idx[i]); out(" "); out(is_empty(e) ? "(empty)" : nm);
		if (!is_empty(e)) { out(" "); outu(g64(e + 32)); out("-"); outu(g64(e + 40)); }
		out("\n");
	}
	return 0;
}

static int cmd_plan(const char *disk, const char *dir)
{
	int fd, i; struct hdr h;
	if (load(disk, O_RDONLY, &fd)) return 1;
	close(fd);
	if (check_tables(pri, bak, &h)) return 1;
	const u8 *ents = pri + 2 * SECTOR;
	if (strcmp(layout(ents), "windows")) return fail("not the Windows Phone layout (MainOS 26, Data 27, 30/31 free)");
	const u8 *data = ent(ents, 27);
	u64 dfirst = g64(data + 32), dlast = g64(data + 40);
	if (dlast != h.last || dfirst % MiB_SECTORS) return fail("Data is not the last, 1 MiB-aligned partition");
	for (i = 1; i <= (int)h.count; i++) {
		const u8 *e = ent(ents, i);
		if (i == 27 || is_empty(e)) continue;
		if (g64(e + 40) >= dfirst) return fail("another partition lies inside or after Data");
	}
	u64 bfirst = dfirst, blast = bfirst + 64 * MiB_SECTORS - 1;
	u64 sfirst = blast + 1, slast = sfirst + 2048 * MiB_SECTORS - 1;
	u64 afirst = slast + 1, alast = dlast;
	if (alast < afirst + 1024 * MiB_SECTORS) return fail("Data is too small for the Android layout");

	static const u8 linux_fs[16] = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };
	static u8 nents[32 * SECTOR];
	memcpy(nents, ents, sizeof nents);
	struct { int idx; const char *name; u64 f, l; } plan[3] = {
		{ 27, "cache", bfirst, blast }, { 30, "android_system", sfirst, slast }, { 31, "android_data", afirst, alast } };
	for (i = 0; i < 3; i++) {
		u8 *e = nents + (plan[i].idx - 1) * 128;
		char nm[64] = "superman-android/";
		cat(nm, plan[i].name);
		memset(e, 0, 128);
		memcpy(e, linux_fs, 16);
		uuid5(h.disk, nm, e + 16);
		p64(e + 32, plan[i].f); p64(e + 40, plan[i].l);
		put_name(e, plan[i].name);
	}
	memcpy(npri, pri, SECTOR);
	/* Protective MBR: Windows Phone writes start 1 + length = disk size, one sector past the end. lk2nd's
	 * partition_publish() rejects such an entry ("range does not fit the device"), never reads the GPT and so
	 * finds no eMMC partition to boot from. Use the UEFI value, min(disk size - 1, 0xffffffff). */
	{
		u64 want = total - 1 > 0xffffffffULL ? 0xffffffffULL : total - 1;
		if (g32(npri + 446 + 8) == 1 && g32(npri + 446 + 12) > want) p32(npri + 446 + 12, (u32)want);
	}
	build_hdr(npri + SECTOR, &h, nents, 1, total - 1, 2);
	memcpy(npri + 2 * SECTOR, nents, 32 * SECTOR);
	memcpy(nbak, nents, 32 * SECTOR);
	build_hdr(nbak + 32 * SECTOR, &h, nents, total - 1, 1, total - BAK_SECTORS);

	/* prove the result: valid, and only entries 27/30/31 differ */
	struct hdr nh;
	if (check_tables(npri, nbak, &nh) || strcmp(layout(npri + 2 * SECTOR), "android")) return fail("internal error: new table invalid");
	for (i = 1; i <= (int)h.count; i++)
		if (i != 27 && i != 30 && i != 31 && memcmp(ent(ents, i), ent(nents, i), 128)) return fail("internal error: other entry changed");
	if (nh.first != h.first || nh.last != h.last || memcmp(nh.disk, h.disk, 16)) return fail("internal error: header changed");

	char env[512] = "";
	const char *k[] = { "DISK_SECTORS=", "BOOT_FIRST=", "BOOT_LAST=", "SYSTEM_FIRST=", "SYSTEM_LAST=", "DATA_FIRST=", "DATA_LAST=", "OLD_DATA_FIRST=", "OLD_DATA_LAST=" };
	u64 v[] = { total, bfirst, blast, sfirst, slast, afirst, alast, dfirst, dlast };
	for (i = 0; i < 9; i++) {
		char num[24]; int j = 23; u64 x = v[i]; num[j] = 0;
		do { num[--j] = '0' + x % 10; x /= 10; } while (x);
		cat(env, k[i]); cat(env, num + j); cat(env, "\n");
	}
	if (save(dir, "old-primary.bin", pri, sizeof pri) || save(dir, "old-backup.bin", bak, sizeof bak) ||
	    save(dir, "new-primary.bin", npri, sizeof npri) || save(dir, "new-backup.bin", nbak, sizeof nbak) ||
	    save(dir, "plan.env", (const u8 *)env, strlen(env)))
		return 1;
	out(env);
	return 0;
}

/* write: current == from-*, then put to-* on the disk and read back */
static int swap_tables(const char *disk, const char *dir, const char *from, const char *to)
{
	static u8 fp[sizeof pri], fb[sizeof bak];
	char n1[32], n2[32];
	int fd;
	strcpy(n1, from); cat(n1, "-primary.bin"); strcpy(n2, from); cat(n2, "-backup.bin");
	if (loadf(dir, n1, fp, sizeof fp) || loadf(dir, n2, fb, sizeof fb)) return 1;
	strcpy(n1, to); cat(n1, "-primary.bin"); strcpy(n2, to); cat(n2, "-backup.bin");
	if (loadf(dir, n1, npri, sizeof npri) || loadf(dir, n2, nbak, sizeof nbak)) return 1;
	if (load(disk, O_RDWR, &fd)) return 1;
	struct hdr h;
	if (check_tables(npri, nbak, &h)) { close(fd); return fail("plan files are not a valid GPT for this disk"); }
	if (!memcmp(pri, npri, sizeof pri) && !memcmp(bak, nbak, sizeof bak)) { close(fd); out("already written, nothing to do\n"); return 0; }
	if (memcmp(pri, fp, sizeof pri) || memcmp(bak, fb, sizeof bak)) { close(fd); return fail("the disk's GPT changed since the plan: refusing"); }
	if (wr(fd, 0, npri, sizeof npri)) { close(fd); return fail("WRITE FAILED (primary)"); }
	flush(fd);
	if (wr(fd, total - BAK_SECTORS, nbak, sizeof nbak)) { close(fd); return fail("WRITE FAILED (backup)"); }
	flush(fd);
	if (rd(fd, 0, chk, sizeof npri) || memcmp(chk, npri, sizeof npri)) { close(fd); return fail("READ-BACK MISMATCH (primary)"); }
	if (rd(fd, total - BAK_SECTORS, chk, sizeof nbak) || memcmp(chk, nbak, sizeof nbak)) { close(fd); return fail("READ-BACK MISMATCH (backup)"); }
	close(fd);
	out("written and read back: "); out(to); out(" tables\n");
	return 0;
}

static u64 num(const char *s, int *bad)
{
	u64 v = 0;
	if (!*s) *bad = 1;
	for (; *s; s++) { if (*s < '0' || *s > '9') { *bad = 1; return 0; } v = v * 10 + (*s - '0'); }
	return v;
}
static int cmd_loop(const char *disk, const char *a, const char *b)
{
	int bad = 0, fd, ctl, lfd, n;
	u64 first = num(a, &bad), last = num(b, &bad), size;
	if (bad || last < first) return fail("bad sector range");
	fd = open(disk, O_RDWR);
	if (fd < 0 || seek64(fd, 0, SEEK_END, &size)) return fail("cannot open the disk");
	if ((last + 1) * SECTOR > size) return fail("range beyond the end of the disk");
	ctl = open("/dev/loop-control", O_RDWR);
	if (ctl < 0) return fail("cannot open /dev/loop-control");
	n = ioctl(ctl, LOOP_CTL_GET_FREE, 0);
	close(ctl);
	if (n < 0) return fail("no free loop device");
	char path[32] = "/dev/block/loop", num_s[12]; int i = 11; num_s[i] = 0;
	do { num_s[--i] = '0' + n % 10; n /= 10; } while (n);
	cat(path, num_s + i);
	lfd = open(path, O_RDWR);
	if (lfd < 0) return fail("cannot open the loop device");
	if (ioctl(lfd, LOOP_SET_FD, fd)) return fail("LOOP_SET_FD failed");
	struct loop_info64 li;
	memset(&li, 0, sizeof li);
	li.lo_offset = first * SECTOR;
	li.lo_sizelimit = (last - first + 1) * SECTOR;
	if (ioctl(lfd, LOOP_SET_STATUS64, &li)) { ioctl(lfd, LOOP_CLR_FD, 0); return fail("LOOP_SET_STATUS64 failed"); }
	u64 got = 0;
	if (seek64(lfd, 0, SEEK_END, &got) || got != li.lo_sizelimit) { ioctl(lfd, LOOP_CLR_FD, 0); return fail("loop device has the wrong size"); }
	close(lfd); close(fd);
	out(path); out("\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "loop")) return cmd_loop(argv[2], argv[3], argv[4]);
	if (argc == 3 && !strcmp(argv[1], "status")) return cmd_status(argv[2]);
	if (argc == 4 && !strcmp(argv[1], "plan")) return cmd_plan(argv[2], argv[3]);
	if (argc == 4 && !strcmp(argv[1], "write")) return swap_tables(argv[2], argv[3], "old", "new");
	if (argc == 4 && !strcmp(argv[1], "restore")) return swap_tables(argv[2], argv[3], "new", "old");
	return fail("usage: superman-gpt status DISK | plan DISK DIR | write DISK DIR | restore DISK DIR | loop DISK FIRST LAST");
}
