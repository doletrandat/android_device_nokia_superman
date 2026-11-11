#!/usr/bin/env python3
"""Update the phone's hash contracts after a kernel, boot image or system file change.

  update-contracts.py --root ROOT [--boot BOOT.img] [--runtime-fdt FDT] [--config-sha H] [--notes-sha H]
                      [--release-manifest release-manifest.txt]

ROOT is a copy of the phone's root file tree that contains the files to ship (ROOT/system/vendor/etc/... as in
the release file store). The tool rewrites, in ROOT:

  system/vendor/etc/superman-native-boot.contract      "<boot sha256> <phase>"
  system/vendor/etc/superman/audio-boot.contract       "<boot> <runtime FDT> <kernel config> <kernel notes>"
  system/vendor/etc/superman-native-components.sha256  one "sha256  path" line per protected file

Fields that are not given keep their value. Every manifest line whose file exists under ROOT is re-hashed, so an
edited script or library is picked up too; files that are not in ROOT keep their recorded hash.
--release-manifest also updates the sha256 column of the image assembly's file list for files present in ROOT.

Where the values come from: boot = sha256 of boot.img; runtime FDT = sha256 of /sys/firmware/fdt on the phone
running that boot image (predict it with boot-tool.py predict-fdt, or RAM-boot and pull it); config = sha256 of
`zcat /proc/config.gz`; notes = sha256 of /sys/kernel/notes (both printed by build-kernel.sh)."""
import argparse, hashlib, re, sys
from pathlib import Path

sha = lambda b: hashlib.sha256(b).hexdigest()
HEX = re.compile(r'^[0-9a-f]{64}$')
def die(m): sys.exit('update-contracts: ' + m)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--boot', type=Path); ap.add_argument('--runtime-fdt', type=Path)
    ap.add_argument('--config-sha'); ap.add_argument('--notes-sha')
    ap.add_argument('--release-manifest', type=Path)
    a = ap.parse_args()
    etc = a.root / 'system/vendor/etc'
    nb, ab, nm = etc / 'superman-native-boot.contract', etc / 'superman/audio-boot.contract', etc / 'superman-native-components.sha256'
    for f in (nb, ab, nm):
        if not f.is_file(): die(f'missing {f}')
    for h in (a.config_sha, a.notes_sha):
        if h and not HEX.match(h): die('--config-sha / --notes-sha want 64 hex digits')

    boot_old, phase = nb.read_text().split()
    b0, f0, c0, n0 = ab.read_text().split()
    if b0 != boot_old: die('the two contracts name different boot images; fix that first')
    boot = sha(a.boot.read_bytes()) if a.boot else boot_old
    fdt = sha(a.runtime_fdt.read_bytes()) if a.runtime_fdt else f0
    if a.boot and boot != boot_old and not a.runtime_fdt:
        print('warning: new boot image but no --runtime-fdt: audio, battery and the modem will refuse to start if the FDT changed', file=sys.stderr)
    nb.write_text(f'{boot} {phase}\n')
    ab.write_text(f'{boot} {fdt} {a.config_sha or c0} {a.notes_sha or n0}\n')

    def on_disk(p):                      # manifest path -> file in ROOT ("/vendor/x" lives in /system/vendor/x)
        return a.root / ('system' + p if p.startswith('/vendor/') else p.lstrip('/'))
    out, changed = [], []
    for line in nm.read_text().splitlines():
        h, p = line.split('  ', 1)
        f = on_disk(p)
        if f != nm and f.is_file():
            n = sha(f.read_bytes())
            if n != h: changed.append(p); h = n
        out.append(f'{h}  {p}')
    nm.write_text('\n'.join(out) + '\n')

    if a.release_manifest:
        rows, n = [], 0
        for line in a.release_manifest.read_text().splitlines():
            q = line.split(' ')
            if q[0] == 'F':
                f = a.root / q[6].lstrip('/')
                if f.is_file():
                    h = sha(f.read_bytes())
                    if h != q[5]: q[5] = h; n += 1
            rows.append(' '.join(q))
        a.release_manifest.write_text('\n'.join(rows) + '\n')
        print(f'release manifest: {n} line(s) updated')
    print(f'boot    {boot}\nfdt     {fdt}\nconfig  {a.config_sha or c0}\nnotes   {a.notes_sha or n0}')
    print('manifest lines re-hashed:', ', '.join(changed) if changed else 'none')
    for f in (nb, ab, nm): print(f'{sha(f.read_bytes())}  {f.relative_to(a.root)}')

if __name__ == '__main__':
    main()
