// SPDX-License-Identifier: Apache-2.0
/*
 * bcd-default: show or change which entry the Windows Boot Manager starts by default, in a BCD hive file.
 * Lumia 730 Android port: lets Android switch the phone's boot menu between "Boot Shim" (LK -> Android) and
 * Windows Phone, without a PC.
 *
 *   bcd-default BCD status
 *   bcd-default BCD windows     default = Windows Loader {7619dcc9-fafe-11d9-b411-000476eba25f}
 *   bcd-default BCD android     default = the entry whose path is \Windows\System32\BOOT\bootshim.efi
 *
 * Only {bootmgr}\Elements\23000003 (REG_SZ, a 38-character GUID) is changed, in place, same length: the GUID's
 * 76 UTF-16 bytes are rewritten in place (lseek + write) and read back. Nothing is allocated or moved in the hive. Refuses to
 * write a hive whose sequence numbers differ (dirty: pending transaction log).
 * Builds with the kernel's header-only nolibc (static, no Android libraries) or with a normal libc for host tests.
 */
#ifndef NOLIBC
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#endif

#define MAXHIVE (2 * 1024 * 1024)
static unsigned char d[MAXHIVE];
static long hlen;

static const char *BOOTMGR = "{9dea862c-5cdd-4e70-acc1-f32b344d4795}";
static const char *WINLOADER = "{7619dcc9-fafe-11d9-b411-000476eba25f}";
static const char *SHIMPATH = "\\Windows\\System32\\BOOT\\bootshim.efi";

static void out(const char *s) { write(1, s, strlen(s)); }
static int fail(const char *s) { write(2, "bcd-default: ", 13); write(2, s, strlen(s)); write(2, "\n", 1); return 1; }

static unsigned u16(long o) { return o >= 0 && o + 2 <= hlen ? d[o] | d[o + 1] << 8 : 0; }
static unsigned u32(long o) { return o >= 0 && o + 4 <= hlen ? d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (unsigned)d[o + 3] << 24 : 0; }
/* cell offset (relative to the first hbin) -> absolute offset of its data, or -1 */
static long cell(unsigned off) {
	long o = 0x1000 + (long)off;
	if (off == 0xffffffffu || o + 4 > hlen) return -1;
	int size = (int)u32(o);
	if (size >= 0 || o + 4 + (-size - 4) > hlen) return -1;   /* allocated cells have negative size */
	return o + 4;
}
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* nk key name equals s (case-insensitive); names are ASCII (flag 0x20) or UTF-16 */
static int nk_name_is(long nk, const char *s) {
	unsigned flags = u16(nk + 2), n = u16(nk + 0x48), i, len = strlen(s);
	long p = nk + 0x4c;
	if (flags & 0x20) {
		if (n != len) return 0;
		for (i = 0; i < n; i++) if (lower(d[p + i]) != lower(s[i])) return 0;
	} else {
		if (n != 2 * len) return 0;
		for (i = 0; i < len; i++) if (lower(u16(p + 2 * i)) != lower(s[i])) return 0;
	}
	return 1;
}
static long nk_at(unsigned off) {
	long o = cell(off);
	return o >= 0 && d[o] == 'n' && d[o + 1] == 'k' ? o : -1;
}
/* find subkey `name` of key `nk` (lf/lh/li/ri lists) */
static long sub_list(long lst, const char *name, int depth) {
	if (lst < 0 || depth > 4) return -1;
	unsigned n = u16(lst + 2), i;
	int lf = (d[lst] == 'l' && (d[lst + 1] == 'f' || d[lst + 1] == 'h'));
	int li = (d[lst] == 'l' && d[lst + 1] == 'i'), ri = (d[lst] == 'r' && d[lst + 1] == 'i');
	for (i = 0; i < n; i++) {
		unsigned off = u32(lst + 4 + i * (lf ? 8 : 4));
		if (ri) { long r = sub_list(cell(off), name, depth + 1); if (r >= 0) return r; continue; }
		if (!lf && !li) return -1;
		long k = nk_at(off);
		if (k >= 0 && nk_name_is(k, name)) return k;
	}
	return -1;
}
static long subkey(long nk, const char *name) {
	if (nk < 0 || !u32(nk + 0x14)) return -1;
	return sub_list(cell(u32(nk + 0x1c)), name, 0);
}
/* value "Element" of key nk: returns absolute data offset, sets *len and *type; -1 if absent or inline */
static long element(long nk, unsigned *len, unsigned *type) {
	if (nk < 0) return -1;
	unsigned nval = u32(nk + 0x24), i;
	long vl = cell(u32(nk + 0x28));
	for (i = 0; vl >= 0 && i < nval; i++) {
		long vk = cell(u32(vl + i * 4));
		if (vk < 0 || d[vk] != 'v' || d[vk + 1] != 'k') continue;
		unsigned nlen = u16(vk + 2), dlen = u32(vk + 4), flags = u16(vk + 0x10);
		if (!(flags & 1) || nlen != 7 || memcmp(d + vk + 0x14, "Element", 7)) continue;
		if (dlen & 0x80000000u) return -1;
		*len = dlen; *type = u32(vk + 0xc);
		long p = cell(u32(vk + 8));
		return p >= 0 && p + dlen <= hlen ? p : -1;
	}
	return -1;
}
static long object_el(const char *obj, const char *el, unsigned *len, unsigned *type) {
	long root = nk_at(u32(0x24));
	long k = subkey(subkey(subkey(subkey(root, "Objects"), obj), "Elements"), el);
	return element(k, len, type);
}
/* UTF-16LE string at p (n bytes, NUL-terminated) equals ASCII s (case-insensitive) */
static int u16_is(long p, unsigned n, const char *s) {
	unsigned len = strlen(s), i;
	if (n < 2 * len + 2) return 0;
	for (i = 0; i < len; i++) if (lower(u16(p + 2 * i)) != lower(s[i])) return 0;
	return u16(p + 2 * len) == 0;
}
static void u16_to_ascii(long p, unsigned chars, char *buf) {
	unsigned i;
	for (i = 0; i < chars; i++) buf[i] = (char)u16(p + 2 * i);
	buf[chars] = 0;
}
/* the shim object: an Objects\{guid} whose 12000002 path is bootshim.efi */
static int find_shim(char *guid) {
	long root = nk_at(u32(0x24)), objs = subkey(root, "Objects");
	if (objs < 0) return 0;
	long lst = cell(u32(objs + 0x1c));
	unsigned n = u16(lst + 2), i, found = 0;
	int lf = d[lst] == 'l' && (d[lst + 1] == 'f' || d[lst + 1] == 'h');
	if (!lf) return 0;                       /* BCD object lists are lf/lh */
	for (i = 0; i < n; i++) {
		long k = nk_at(u32(lst + 4 + i * 8));
		if (k < 0 || !(u16(k + 2) & 0x20) || u16(k + 0x48) != 38) continue;
		char name[39]; memcpy(name, d + k + 0x4c, 38); name[38] = 0;
		unsigned len, type; long p = object_el(name, "12000002", &len, &type);
		if (p >= 0 && type == 1 && u16_is(p, len, SHIMPATH)) { memcpy(guid, name, 39); found++; }
	}
	return found == 1;                       /* exactly one, or refuse */
}

int main(int argc, char **argv) {
	if (argc != 3) return fail("usage: bcd-default BCD status|windows|android");
	const char *mode = argv[2];
	int write_mode = !strcmp(mode, "windows") || !strcmp(mode, "android");
	if (!write_mode && strcmp(mode, "status")) return fail("mode must be status, windows or android");
	int fd = open(argv[1], write_mode ? O_RDWR : O_RDONLY);
	if (fd < 0) return fail("cannot open the BCD file");
	hlen = read(fd, d, MAXHIVE);
	if (hlen < 0x2000 || hlen >= MAXHIVE || memcmp(d, "regf", 4)) return fail("not a registry hive");
	unsigned len, type;
	long p = object_el(BOOTMGR, "23000003", &len, &type);
	if (p < 0 || type != 1 || len != 78 || u16(p) != '{' || u16(p + 74) != '}' || u16(p + 76) != 0)
		return fail("{bootmgr} default (23000003) not found or not a GUID string");
	char cur[39], shim[39];
	u16_to_ascii(p, 38, cur);
	int have_shim = find_shim(shim);
	unsigned wl, wt;
	int have_win = object_el(WINLOADER, "12000004", &wl, &wt) >= 0;
	out("default: "); out(cur);
	out(!strcmp(cur, WINLOADER) ? " (Windows Phone)\n" : (have_shim && !strcmp(cur, shim) ? " (Boot Shim -> Android)\n" : " (other)\n"));
	out("boot shim entry: "); out(have_shim ? shim : "none (or more than one)"); out("\n");
	out("windows loader: "); out(have_win ? WINLOADER : "missing"); out("\n");
	if (!write_mode) return 0;

	if (u32(4) != u32(8)) return fail("hive is dirty (sequence numbers differ): refusing to write");
	const char *want = !strcmp(mode, "windows") ? (have_win ? WINLOADER : 0) : (have_shim ? shim : 0);
	if (!want) return fail("target entry not present in this BCD");
	if (!strcmp(cur, want)) { out("already the default, nothing written\n"); return 0; }
	unsigned char buf[76]; unsigned i;
	for (i = 0; i < 38; i++) { buf[2 * i] = (unsigned char)want[i]; buf[2 * i + 1] = 0; }
	if (lseek(fd, p, SEEK_SET) != p || write(fd, buf, 76) != 76) return fail("write failed");
	fsync(fd);
	unsigned char back[76];
	if (lseek(fd, p, SEEK_SET) != p || read(fd, back, 76) != 76 || memcmp(back, buf, 76)) return fail("read-back mismatch");
	close(fd);
	out("default set to "); out(want); out("\n");
	return 0;
}
