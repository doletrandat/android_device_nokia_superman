#!/usr/bin/env python3
"""Boot image tool for the Lumia 730 (superman): Android boot image v0, 2048-byte pages, zImage with appended DTB.

  boot-tool.py info BOOT
  boot-tool.py split BOOT DIR                      -> DIR/{header.bin,zImage,dtb,ramdisk.cpio.gz,cmdline.txt}
  boot-tool.py build --base BOOT -o OUT [--zimage F] [--dtb F] [--ramdisk F] [--cmdline-replace OLD NEW ...]
        Replace components of BOOT. --cmdline-replace edits the header command line and, when OLD and NEW have
        the same length, also the copy in the DTB's /chosen/bootargs (LK joins both; the last value wins).
  boot-tool.py internal --base BOOT --fstab FSTAB -o OUT
        Internal-storage variant: ramdisk /fstab.superman = FSTAB and /init = /init.real (no SD pre-init logger).
  boot-tool.py predict-fdt --old-boot A --old-runtime-fdt F --new-boot B -o OUT
        The runtime FDT (/sys/firmware/fdt) the phone will have with B, from the one it had with A. Works when
        the stored DTBs differ only by same-length substitutions and/or the ramdisk size changed. Otherwise
        RAM-boot the image (fastboot boot) and pull /sys/firmware/fdt.

Every command prints the sha256 values the phone's contracts pin (see DEVELOPING.md, "Hash checks")."""
import argparse, gzip, hashlib, json, runpy, struct, sys
from pathlib import Path

PAGE = 2048
sha = lambda b: hashlib.sha256(b).hexdigest()
def die(msg): sys.exit('boot-tool: ' + msg)
def pad(b): return b + b'\0' * (-len(b) % PAGE)

def split(data):
    if data[:8] != b'ANDROID!': die('not an Android boot image')
    ks, _, rs, _, ss, _, _, page, version, _ = struct.unpack_from('<10I', data, 8)
    if version != 0 or ss != 0 or page != PAGE: die('unexpected header (want v0, no second stage, 2048-byte pages)')
    kernel = data[PAGE:PAGE + ks]; rstart = PAGE + len(pad(kernel)); ramdisk = data[rstart:rstart + rs]
    if len(data) != rstart + len(pad(ramdisk)): die('unexpected trailer or truncation')
    zsize = struct.unpack_from('<I', kernel, 44)[0]
    if struct.unpack_from('<I', kernel, 40)[0] != 0 or kernel[zsize:zsize + 4] != b'\xd0\x0d\xfe\xed': die('no DTB appended to the zImage')
    dtb = kernel[zsize:]
    if struct.unpack_from('>I', dtb, 4)[0] != len(dtb): die('unexpected data after the DTB')
    return data[:PAGE], kernel[:zsize], dtb, ramdisk

def boot_id(kernel, ramdisk):
    d = hashlib.sha1()
    for c in (kernel, ramdisk, b''):
        d.update(c); d.update(struct.pack('<I', len(c)))
    return d.digest() + b'\0' * 12

def join(header, zimage, dtb, ramdisk):
    h = bytearray(header)
    struct.pack_into('<I', h, 8, len(zimage) + len(dtb)); struct.pack_into('<I', h, 16, len(ramdisk))
    h[576:608] = boot_id(zimage + dtb, ramdisk)
    image = bytes(h) + pad(zimage + dtb) + pad(ramdisk)
    assert split(image) == (bytes(h), zimage, dtb, ramdisk)
    here = Path(__file__).resolve().parent
    layout = runpy.run_path(str(here.parent / 'scripts/verify-arm-boot-layout.py'))['verify_boot_layout'](image)
    if not layout['safe']: die('boot layout check failed: ' + json.dumps(layout))
    return image

def cmdline(header): return header[64:576].rstrip(b'\0').decode()
def report(image, label='boot'):
    h, z, d, r = split(image)
    print(json.dumps({label + '_sha256': sha(image), 'size': len(image), 'zImage_sha256': sha(z), 'dtb_sha256': sha(d),
                      'ramdisk_sha256': sha(r), 'ramdisk_size': len(r), 'header_cmdline': cmdline(h)}, indent=1))

def replace_cmdline(header, dtb, pairs):
    h = bytearray(header); line = cmdline(header).encode()
    for old, new in pairs:
        if line.count(old) != 1: die(f'"{old.decode()}" is not exactly once in the header command line')
        line = line.replace(old, new)
        if len(old) == len(new):
            if dtb.count(old) != 1: die(f'"{old.decode()}" is not exactly once in the DTB')
            dtb = dtb.replace(old, new)
        else:
            print(f'note: "{old.decode()}" changes length; the DTB copy of the command line is left as it is', file=sys.stderr)
    if len(line) > 511: die('command line too long')
    h[64:576] = line + b'\0' * (512 - len(line))
    return bytes(h), dtb

def cpio_replace(archive, repl):
    """newc cpio: replace the data of some members, keep every other byte."""
    al = lambda n: (n + 3) & ~3
    def member(name):
        i = 0
        while True:
            f = [int(archive[i + 6 + 8 * k:i + 14 + 8 * k], 16) for k in range(13)]
            n = archive[i + 110:i + 110 + f[11] - 1].decode(); he = al(i + 110 + f[11])
            if n == name: return archive[he:he + f[6]]
            if n == 'TRAILER!!!': die('ramdisk has no ' + name)
            i = al(he + f[6])
    repl = {k: (member(v[1:]) if isinstance(v, str) else v) for k, v in repl.items()}   # '@name' copies a member
    out, i, found = bytearray(), 0, 0
    while True:
        if archive[i:i + 6] != b'070701': die('ramdisk is not a newc cpio archive')
        f = [int(archive[i + 6 + 8 * k:i + 14 + 8 * k], 16) for k in range(13)]
        name = archive[i + 110:i + 110 + f[11] - 1].decode(); he = al(i + 110 + f[11]); de = al(he + f[6])
        if name in repl:
            new = repl[name]; hh = bytearray(archive[i:he]); hh[6 + 8 * 6:14 + 8 * 6] = b'%08X' % len(new)
            out += hh + new + b'\0' * (al(len(new)) - len(new)); found += 1
        else: out += archive[i:de]
        i = de
        if name == 'TRAILER!!!': out += archive[i:]; break
    if found != len(repl): die('not every ramdisk member to replace was found')
    return bytes(out)

def fdt_props(fdt, want):
    be = lambda o: struct.unpack_from('>I', fdt, o)[0]
    o, strs, path, hit = be(8), be(12), [], {}
    while True:
        tag = be(o); o += 4
        if tag == 1: e = fdt.index(b'\0', o); path.append(fdt[o:e].decode()); o = (e + 4) & ~3
        elif tag == 2: path.pop()
        elif tag == 3:
            ln, no = be(o), be(o + 4); o += 8
            n = fdt[strs + no:fdt.index(b'\0', strs + no)].decode()
            if (tuple(path[1:]), n) in want: hit[n] = (o, ln)
            o = (o + ln + 3) & ~3
        elif tag == 9: return hit

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('info'); p.add_argument('boot', type=Path)
    p = sub.add_parser('split'); p.add_argument('boot', type=Path); p.add_argument('dir', type=Path)
    p = sub.add_parser('build'); p.add_argument('--base', type=Path, required=True); p.add_argument('-o', '--out', type=Path, required=True)
    for a in ('--zimage', '--dtb', '--ramdisk'): p.add_argument(a, type=Path)
    p.add_argument('--cmdline-replace', action='append', default=[], nargs=2, metavar=('OLD', 'NEW'))
    p = sub.add_parser('internal'); p.add_argument('--base', type=Path, required=True); p.add_argument('--fstab', type=Path, required=True)
    p.add_argument('-o', '--out', type=Path, required=True)
    p = sub.add_parser('predict-fdt')
    for a in ('--old-boot', '--old-runtime-fdt', '--new-boot'): p.add_argument(a, type=Path, required=True)
    p.add_argument('-o', '--out', type=Path, required=True)
    a = ap.parse_args()

    if a.cmd == 'info':
        report(a.boot.read_bytes())
    elif a.cmd == 'split':
        h, z, d, r = split(a.boot.read_bytes()); a.dir.mkdir(parents=True, exist_ok=True)
        for n, b in (('header.bin', h), ('zImage', z), ('dtb', d), ('ramdisk.cpio.gz', r), ('cmdline.txt', (cmdline(h) + '\n').encode())):
            (a.dir / n).write_bytes(b)
        report(a.boot.read_bytes())
    elif a.cmd == 'build':
        h, z, d, r = split(a.base.read_bytes())
        if a.zimage: z = a.zimage.read_bytes()
        if a.dtb: d = a.dtb.read_bytes()
        if a.ramdisk: r = a.ramdisk.read_bytes()
        pairs = [(old.encode(), new.encode()) for old, new in a.cmdline_replace]
        if pairs: h, d = replace_cmdline(h, d, pairs)
        image = join(h, z, d, r); a.out.write_bytes(image); report(image)
    elif a.cmd == 'internal':
        h, z, d, r = split(a.base.read_bytes())
        cp = cpio_replace(gzip.decompress(r), {'fstab.superman': a.fstab.read_bytes(), 'init': '@init.real'})
        image = join(h, z, d, gzip.compress(cp, compresslevel=9, mtime=0)); a.out.write_bytes(image); report(image)
    elif a.cmd == 'predict-fdt':
        oh, oz, od, orr = split(a.old_boot.read_bytes()); nh, nz, nd, nr = split(a.new_boot.read_bytes())
        fdt = bytearray(a.old_runtime_fdt.read_bytes())
        if len(od) != len(nd): die('the stored DTBs differ in size: RAM-boot the new image and pull /sys/firmware/fdt instead')
        # same-length substitutions in the stored DTB / header command line appear verbatim in the runtime FDT
        subs, i = [], 0
        for src_old, src_new in ((od, nd), (oh[64:576], nh[64:576])):
            i = 0
            while i < len(src_old):
                if src_old[i] != src_new[i]:
                    s = i
                    while s > 0 and src_old[s - 1] not in (0, 0x20): s -= 1
                    e = i
                    while e < len(src_old) and src_old[e] not in (0, 0x20): e += 1
                    if src_new[s:e].count(0) or src_new[e:e + 1] != src_old[e:e + 1]: die('a non-text or length-changing DTB/command-line change: RAM-boot and pull /sys/firmware/fdt')
                    subs.append((bytes(src_old[s:e]), bytes(src_new[s:e]))); i = e
                else: i += 1
        for old, new in dict.fromkeys(subs):
            n = sum(1 for o, _ in subs if o == old)
            if fdt.count(old) != n: die(f'"{old.decode(errors="replace")}" is in the runtime FDT {fdt.count(old)} times, expected {n}')
            fdt = bytearray(bytes(fdt).replace(old, new))
        hit = fdt_props(fdt, {(('chosen',), 'linux,initrd-start'), (('chosen',), 'linux,initrd-end')})
        start = struct.unpack_from('>I', fdt, hit['linux,initrd-start'][0])[0]; eo = hit['linux,initrd-end'][0]
        if struct.unpack_from('>I', fdt, eo)[0] != start + len(orr): die('the old runtime FDT does not belong to the old boot image (initrd size)')
        struct.pack_into('>I', fdt, eo, start + len(nr))
        a.out.write_bytes(fdt)
        print(json.dumps({'old_runtime_fdt_sha256': sha(a.old_runtime_fdt.read_bytes()), 'predicted_runtime_fdt_sha256': sha(bytes(fdt)),
                          'substitutions': [f'{o.decode()} -> {n.decode()}' for o, n in dict.fromkeys(subs)],
                          'initrd_size': [len(orr), len(nr)]}, indent=1))

if __name__ == '__main__':
    main()
