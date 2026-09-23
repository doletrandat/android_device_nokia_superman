#!/usr/bin/env python3
"""Preserve verified r10 boot components and replace only its appended DTB.

Uses the kernel's dtc/fdtoverlay, never the generic Android bootimage recipe.
All generated artifacts go to a new directory supplied by the caller.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

BASE_SHA = '60aa2c8d4c0e9356d391196bf804192782d8689ca5396abef746da68e4ce6198'


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def split_boot(data):
    require(data[:8] == b'ANDROID!', 'Not an Android boot image')
    ks, _, rs, _, ss, _, _, page, version, _ = struct.unpack_from('<10I', data, 8)
    require(version == 0 and ss == 0 and page == 2048, 'Unexpected baseline header')
    align = lambda size: (size + page - 1) // page * page
    ram_start = page + align(ks)
    end = ram_start + align(rs)
    require(len(data) == end, 'Unexpected boot trailer or truncation')
    kernel = data[page:page + ks]
    ramdisk = data[ram_start:ram_start + rs]
    require(not any(data[page + ks:ram_start] + data[ram_start + rs:end]),
            'Nonzero component padding')
    zsize = struct.unpack_from('<I', kernel, 44)[0]
    zstart = struct.unpack_from('<I', kernel, 40)[0]
    require(zstart == 0 and kernel[zsize:zsize + 4] == b'\xd0\x0d\xfe\xed',
            'Invalid zImage boundary / appended DTB')
    dtb = kernel[zsize:]
    require(struct.unpack_from('>I', dtb, 4)[0] == len(dtb), 'Unexpected DTB trailer')
    return data[:page], kernel[:zsize], dtb, ramdisk


def boot_id(kernel, ramdisk):
    digest = hashlib.sha1()
    for component in (kernel, ramdisk, b''):
        digest.update(component)
        digest.update(struct.pack('<I', len(component)))
    return digest.digest() + b'\0' * 12


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('dtc_dir', type=Path)
    args = parser.parse_args()
    old = args.baseline.read_bytes()
    require(sha(old) == BASE_SHA, 'Baseline hash mismatch')
    header, zimage, base_dtb, ramdisk = split_boot(old)
    require(header[576:608] == boot_id(zimage + base_dtb, ramdisk), 'Baseline ID mismatch')
    args.output.mkdir(exist_ok=False)
    out = args.output
    (out / 'baseline.dtb').write_bytes(base_dtb)
    overlay = Path(__file__).with_name('quarantine-overlay.dts')
    subprocess.run([str(args.dtc_dir / 'dtc'), '-@', '-I', 'dts', '-O', 'dtb',
                    '-o', str(out / 'quarantine.dtbo'), str(overlay)], check=True)
    subprocess.run([str(args.dtc_dir / 'fdtoverlay'), '-i', str(out / 'baseline.dtb'),
                    '-o', str(out / 'quarantine.dtb'), str(out / 'quarantine.dtbo')], check=True)

    def decode(path):
        return subprocess.check_output([str(args.dtc_dir / 'dtc'), '-q', '-I', 'dtb',
                                        '-O', 'dts', str(path)], text=True)

    before = decode(out / 'baseline.dtb')
    after = decode(out / 'quarantine.dtb')
    node = r'\n\t\tdiagnostic-quarantine@f500000 \{\n(?P<body>.*?)\n\t\t\};\n'
    matches = list(re.finditer(node, after, re.S))
    require(len(matches) == 1, 'Expected exactly one quarantine node')
    body = matches[0].group('body')
    require('reg = <0xf500000 0xb00000>;' in body and 'no-map;' in body,
            'Quarantine range/no-map incorrect')
    normalize = lambda text: '\n'.join(line for line in text.splitlines() if line.strip())
    require(normalize(re.sub(node, '', after, flags=re.S)) == normalize(before),
            'Unexpected changes outside quarantine node')
    (out / 'baseline.decoded.dts').write_text(before)
    (out / 'quarantine.decoded.dts').write_text(after)
    dtb = (out / 'quarantine.dtb').read_bytes()
    kernel = zimage + dtb
    new_header = bytearray(header)
    struct.pack_into('<I', new_header, 8, len(kernel))
    new_header[576:608] = boot_id(kernel, ramdisk)
    pad = lambda data: data + b'\0' * (-len(data) % len(header))
    new = bytes(new_header) + pad(kernel) + pad(ramdisk)
    (out / 'boot.img').write_bytes(new)
    nh, nz, nd, nr = split_boot((out / 'boot.img').read_bytes())
    require(nz == zimage and nr == ramdisk and nd == dtb, 'Component verification failed')
    masked = bytearray(nh)
    masked[8:12] = header[8:12]
    masked[576:608] = header[576:608]
    require(bytes(masked) == header, 'Boot parameters changed')
    require(nh[576:608] == boot_id(nz + nd, nr), 'New boot ID mismatch')
    (out / 'kernel').write_bytes(kernel)
    report = dict(baseline_boot_sha256=sha(old), boot_sha256=sha(new),
                  zimage_sha256=sha(zimage), ramdisk_sha256=sha(ramdisk),
                  baseline_dtb_sha256=sha(base_dtb), dtb_sha256=sha(dtb),
                  kernel_sha256=sha(kernel), boot_size=len(new),
                  kernel_ramdisk_parameters_preserved=True, only_dtb_node_added=True)
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
