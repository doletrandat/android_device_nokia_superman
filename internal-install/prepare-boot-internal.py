"""CC59/CC60: internal-eMMC variant of the release boot image (CC60 340472e9 = CC54b + storage DMA).
Kernel, DTB and header cmdline unchanged. Ramdisk changes: /fstab.superman (by-name GPT partitions on the eMMC), and
/init = Android's init (/init.real): the SD pre-init logger (lumia-a11-log) looks for a LUMIA_BOOT partition 1 on a
card and is not needed internally (/lumia_sd comes from fstab.superman.late).
Predicted runtime FDT = the SD runtime FDT (4d170538) with /chosen linux,initrd-end moved by the ramdisk size change
(LK loads the ramdisk at 0x02000000 and adds no boot-source property; cmdline identical)."""
from pathlib import Path
import gzip, hashlib, json, runpy, struct

W = Path(__file__).resolve().parent
R = Path('/run/media/doletrandat/AOSP-BUILD/aosp11')
H = runpy.run_path(str(R / 'device/nokia/superman/scripts/prepare-quarantine-boot.py'))
sha = lambda b: hashlib.sha256(b).hexdigest()
base = Path('/home/doletrandat/.cache/superman-cc60-dma-20261110/boot-cc60-dma.img').read_bytes()
assert sha(base) == '340472e982d14211eab9e8b357066b7792aea46479ae9f4c5073a90ba51a2f8c'
nh, oz, od, nr = H['split_boot'](base)

# --- newc cpio: replace one file's data, keep every other byte of the archive ---
cp = gzip.decompress(nr)
def member(name):
    i = 0
    while True:
        h = cp[i:i + 110]; f = [int(h[6 + 8 * k:14 + 8 * k], 16) for k in range(13)]
        n = cp[i + 110:i + 110 + f[11] - 1].decode(); he = (i + 110 + f[11] + 3) & ~3
        if n == name: return cp[he:he + f[6]]
        if n == 'TRAILER!!!': raise KeyError(name)
        i = (he + f[6] + 3) & ~3
REPLACE = {'fstab.superman': (W / 'fstab.superman.internal').read_bytes(), 'init': member('init.real')}
out, i, found = bytearray(), 0, 0
al = lambda n: (n + 3) & ~3
while True:
    h = cp[i:i + 110]; assert h[:6] == b'070701', h[:6]
    f = [int(h[6 + 8 * k:14 + 8 * k], 16) for k in range(13)]
    namesize, filesize = f[11], f[6]
    name = cp[i + 110:i + 110 + namesize - 1].decode()
    hdr_end = al(i + 110 + namesize); data_end = al(hdr_end + filesize)
    if name in REPLACE:
        new = REPLACE[name]
        hh = bytearray(cp[i:hdr_end]); hh[6 + 8 * 6:14 + 8 * 6] = b'%08X' % len(new)
        out += hh + new + b'\0' * (al(len(new)) - len(new)); found += 1
    else:
        out += cp[i:data_end]
    i = data_end
    if name == 'TRAILER!!!':
        out += cp[i:]; break
assert found == 2
newrd = gzip.compress(bytes(out), compresslevel=9, mtime=0)

head = bytearray(nh)
struct.pack_into('<I', head, 16, len(newrd))
head[576:608] = H['boot_id'](oz + od, newrd)
pad = lambda b: b + b'\0' * (-len(b) % len(nh))
image = bytes(head) + pad(oz + od) + pad(newrd)
hh, zz, dd, rr = H['split_boot'](image)
assert zz == oz and dd == od and rr == newrd and hh[64:576] == nh[64:576]
layout = runpy.run_path(str(R / 'device/nokia/superman/scripts/verify-arm-boot-layout.py'))['verify_boot_layout'](image)
assert layout['safe'], layout
(W / 'boot-internal.img').write_bytes(image)

# --- predicted runtime FDT: patch linux,initrd-end in the SD runtime FDT ---
fdt = bytearray((W / 'runtime-sd.fdt').read_bytes())
assert sha(fdt) == '4d170538cf547add1443d0d3204c95444af121d78a439b66c571f0b35fdb94b1'
be = lambda o: struct.unpack_from('>I', fdt, o)[0]
off_struct, off_str = be(8), be(12)
o, path, hit = off_struct, [], []
while True:
    tag = be(o); o += 4
    if tag == 1:
        e = fdt.index(b'\0', o); path.append(fdt[o:e].decode()); o = (e + 4) & ~3
    elif tag == 2: path.pop()
    elif tag == 3:
        ln, no = be(o), be(o + 4); o += 8
        n = fdt[off_str + no:fdt.index(b'\0', off_str + no)].decode()
        if path[1:] == ['chosen'] and n in ('linux,initrd-start', 'linux,initrd-end'): hit.append((n, o, ln))
        o = (o + ln + 3) & ~3
    elif tag == 4: continue
    elif tag == 9: break
d = dict((n, (o, ln)) for n, o, ln in hit)
start = be(d['linux,initrd-start'][0]); end_o = d['linux,initrd-end'][0]
assert start == 0x02000000 and be(end_o) == start + len(nr), (hex(start), hex(be(end_o)), len(nr))
struct.pack_into('>I', fdt, end_o, start + len(newrd))
(W / 'runtime-expected-internal.fdt').write_bytes(fdt)
res = dict(boot_internal_sha256=sha(image), base_boot_sha256=sha(base), ramdisk_size=len(newrd), base_ramdisk_size=len(nr),
           expected_running_FDT_sha256=sha(bytes(fdt)), size=len(image), fstab_sha256=sha((W / 'fstab.superman.internal').read_bytes()))
(W / 'BOOT-VALIDATION.json').write_text(json.dumps(res, indent=2) + '\n')
print(json.dumps(res, indent=1))
