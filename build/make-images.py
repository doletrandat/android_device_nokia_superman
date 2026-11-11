#!/usr/bin/env python3
"""Make new SD and internal images from a published SD image, a boot image and changed files.

  make-images.py --base superman-alpha-sdcard.img --out OUT
                 [--boot boot.img --runtime-fdt sd-runtime.fdt] [--config-sha H] [--notes-sha H]
                 [--overlay ROOT] [--keep-raw]

--base     a released SD card image (the starting point; nothing from the original build host is needed)
--boot     the SD boot image to ship. --runtime-fdt is the phone's /sys/firmware/fdt with that boot image
           (boot-tool.py predict-fdt, or RAM-boot it and pull the file). Needed when the boot image changes.
--overlay  a file tree with replacements for files that already exist in the image, at the phone's paths
           (ROOT/system/vendor/bin/foo replaces /system/vendor/bin/foo). Owner, mode and SELinux label of the
           old file are kept. New files are not supported here; add them with debugfs.
The contracts and the file manifest are updated to match (see update-contracts.py), then written into the image.

Outputs in OUT:
  superman-alpha-sdcard.img.gz          SD card image (p1 boot, p2 system, p3 empty data)
  internal/{boot,system,data}.img.gz    partition images for the install on the phone itself
  boot-internal.img                     the internal boot image (ramdisk with the internal fstab)
  IMAGES.json                           every hash, including the values the Windows setup tool pins

Needs e2fsprogs (debugfs, e2fsck, resize2fs, mke2fs) and about 5 GB free in OUT. Builds are reproducible: fixed
timestamps and UUIDs. With an unchanged base, boot image and no overlay, the outputs are byte-identical to the
released ones when OUT is on ext4. On other filesystems (NTFS, some tmpfs setups) only the superblock's
"Lifetime writes" counter differs, which changes the hashes but not the contents."""
import argparse, gzip, hashlib, json, os, runpy, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEV = HERE.parent
os.environ['E2FSPROGS_FAKE_TIME'] = '1793836800'          # 2026-11-05 00:00 UTC
# e2fsck -fy stamps "last checked" into the internal system image; pin it (the released image's value) so the
# build does not depend on the clock.
os.environ['E2FSCK_TIME'] = '1794372055'
SECTOR = 512
def die(m): sys.exit('make-images: ' + m)
def sha_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''): h.update(c)
    return h.hexdigest()
sha = lambda b: hashlib.sha256(b).hexdigest()
def run(cmd, ok=(0,), **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode not in ok: die(f'{cmd[0]} failed: {" ".join(str(c) for c in cmd)}\n{r.stdout[-1500:]}{r.stderr[-1500:]}')
    return r.stdout + r.stderr
def dbg(img, *cmds, write=True):
    return ''.join(run(['debugfs'] + (['-w'] if write else []) + ['-R', c, img]) for c in cmds)
def cat(img, path):
    r = subprocess.run(['debugfs', '-R', f'cat "{path}"', str(img)], capture_output=True)
    if r.returncode or b'File not found' in r.stderr: die(f'{path} is not in {img}')
    return r.stdout
def replace(img, path, data, tmp):
    """Replace an existing file, keeping mode, owner and SELinux label."""
    st = dbg(img, f'stat "{path}"', write=False)
    if 'Mode:' not in st: die(f'{path} does not exist in the image (only existing files can be replaced)')
    mode = st.split('Mode:')[1].split()[0]; uid = st.split('User:')[1].split()[0]; gid = st.split('Group:')[1].split()[0]
    lab = dbg(img, f'ea_get "{path}" security.selinux', write=False).strip()
    label = lab[lab.index('"') + 1:lab.rindex('"')].replace('\\000', '') if lab.count('"') == 2 else None
    src = tmp / 'replace.bin'; src.write_bytes(data)
    cmds = [f'rm "{path}"', f'write {src} "{path}"', f'set_inode_field "{path}" mode 0{0o100000 | int(mode, 8):o}',
            f'set_inode_field "{path}" uid {uid}', f'set_inode_field "{path}" gid {gid}']
    if label:
        lf = tmp / 'replace.lbl'; lf.write_bytes(label.encode() + b'\0'); cmds.append(f'ea_set -f {lf} "{path}" security.selinux')
    dbg(img, *cmds)
    if cat(img, path) != data: die(f'{path} did not read back after replacing it')
def gz(src, dst):
    with open(src, 'rb') as f, open(dst, 'wb') as g, gzip.GzipFile(fileobj=g, mode='wb', compresslevel=9, mtime=0, filename='') as z:
        shutil.copyfileobj(f, z, 1 << 20)

V = '/system/vendor/etc'
NB, AB, NM = f'{V}/superman-native-boot.contract', f'{V}/superman/audio-boot.contract', f'{V}/superman-native-components.sha256'
def set_contracts(img, tmp, boot, fdt, config=None, notes=None, changed=()):
    """Write both contracts and refresh the manifest lines of the contracts and of `changed` files."""
    b0, f0, c0, n0 = cat(img, AB).decode().split(); phase = cat(img, NB).decode().split()[1]
    new = {NB: f'{boot} {phase}\n'.encode(), AB: f'{boot} {fdt or f0} {config or c0} {notes or n0}\n'.encode()}
    for p, d in new.items():
        if cat(img, p) != d: replace(img, p, d, tmp)
    lines = []
    for l in cat(img, NM).decode().splitlines():
        h, p = l.split('  ', 1); f = '/system' + p if p.startswith('/vendor/') else p
        if f in new or f in changed: h = sha(cat(img, f))
        lines.append(f'{h}  {p}')
    data = ('\n'.join(lines) + '\n').encode()
    if cat(img, NM) != data: replace(img, NM, data, tmp)
    bad = [l for l in lines if sha(cat(img, ('/system' + l.split('  ', 1)[1]) if l.split('  ', 1)[1].startswith('/vendor/') else l.split('  ', 1)[1])) != l.split('  ', 1)[0]]
    return len(lines), bad

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base', type=Path, required=True); ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--boot', type=Path); ap.add_argument('--runtime-fdt', type=Path)
    ap.add_argument('--config-sha'); ap.add_argument('--notes-sha'); ap.add_argument('--overlay', type=Path)
    ap.add_argument('--keep-raw', action='store_true', help='keep the uncompressed images too')
    a = ap.parse_args()
    for t in ('debugfs', 'e2fsck', 'resize2fs', 'mke2fs'):
        if not shutil.which(t): die(f'{t} not found (install e2fsprogs; it may be in /usr/sbin)')
    BT = runpy.run_path(str(HERE / 'boot-tool.py'))
    O = a.out; O.mkdir(parents=True, exist_ok=True); (O / 'internal').mkdir(exist_ok=True); tmp = Path(tempfile.mkdtemp(prefix='superman-images-'))   # debugfs commands cannot take paths with spaces

    # ---- take the base image apart (MBR, three primary partitions) ----
    with open(a.base, 'rb') as f:
        head = f.read(SECTOR)
        if head[510:512] != b'\x55\xaa': die('the base image has no MBR')
        parts = [struct.unpack_from('<II', head, 446 + 16 * i + 8) for i in range(3)]
        first = parts[0][0] * SECTOR
        f.seek(0); pre = f.read(first)
        files = {}
        for name, (start, count) in zip(('boot', 'system', 'data'), parts):
            if not count: die('the base image does not have three partitions')
            if name == 'data': continue
            f.seek(start * SECTOR)
            with open(O / f'{name}.img', 'wb') as w:
                left = count * SECTOR
                while left: c = f.read(min(1 << 20, left)); w.write(c); left -= len(c)
            files[name] = O / f'{name}.img'
    BOOT, SYS = files['boot'], files['system']
    res = {'base': {'image': str(a.base), 'boot_partition_sha256': sha_file(BOOT), 'system_sha256': sha_file(SYS)}}

    # ---- SD boot partition ----
    old_boot = cat(BOOT, '/boot.img')
    boot = a.boot.read_bytes() if a.boot else old_boot
    if boot != old_boot:
        if not a.runtime_fdt: die('--boot changes the boot image: --runtime-fdt is needed too')
        src = tmp / 'boot.img'; src.write_bytes(boot)
        dbg(BOOT, 'rm /boot.img', f'write {src} /boot.img', 'set_inode_field /boot.img uid 0', 'set_inode_field /boot.img gid 0',
            'set_inode_field /boot.img mode 0100644')
    run(['e2fsck', '-fn', BOOT])
    sd_fdt = a.runtime_fdt.read_bytes() if a.runtime_fdt else None

    # ---- SD system: overlay, contracts, manifest ----
    changed = []
    if a.overlay:
        for f in sorted(p for p in a.overlay.rglob('*') if p.is_file()):
            path = '/' + f.relative_to(a.overlay).as_posix()
            if path in (NB, AB, NM): continue                       # always generated
            if cat(SYS, path) != f.read_bytes(): replace(SYS, path, f.read_bytes(), tmp); changed.append(path)
    n, bad = set_contracts(SYS, tmp, sha(boot), sha(sd_fdt) if sd_fdt else None, a.config_sha, a.notes_sha, changed)
    if bad: die('files in the image do not match the manifest: ' + ', '.join(bad))
    run(['e2fsck', '-fn', SYS])
    sd_contract_fdt = cat(SYS, AB).decode().split()[1]

    # ---- internal images (what superman-install-internal.sh does on the phone) ----
    IB = O / 'boot-internal.img'
    h, z, d, r = BT['split'](boot)
    cp = BT['cpio_replace'](gzip.decompress(r), {'fstab.superman': (DEV / 'internal-install/fstab.superman.internal').read_bytes(), 'init': '@init.real'})
    iboot = BT['join'](h, z, d, gzip.compress(cp, compresslevel=9, mtime=0)); IB.write_bytes(iboot)
    ifdt = None
    if sd_fdt:                                                      # internal runtime FDT: only the ramdisk size differs
        f = bytearray(sd_fdt); hit = BT['fdt_props'](f, {(('chosen',), 'linux,initrd-start'), (('chosen',), 'linux,initrd-end')})
        start = struct.unpack_from('>I', f, hit['linux,initrd-start'][0])[0]
        if struct.unpack_from('>I', f, hit['linux,initrd-end'][0])[0] != start + len(r): die('--runtime-fdt does not belong to --boot (initrd size)')
        struct.pack_into('>I', f, hit['linux,initrd-end'][0], start + len(BT['split'](iboot)[3])); ifdt = sha(bytes(f))
        (O / 'runtime-expected-internal.fdt').write_bytes(f)
    I = O / 'internal'
    shutil.copyfile(BOOT, I / 'boot.img')
    src = tmp / 'boot-internal.img'; src.write_bytes(iboot)
    dbg(I / 'boot.img', 'rm /boot.img', f'write {src} /boot.img', 'set_inode_field /boot.img uid 0', 'set_inode_field /boot.img gid 0',
        'set_inode_field /boot.img mode 0100644')
    run(['e2fsck', '-fn', I / 'boot.img'])
    shutil.copyfile(SYS, I / 'system.img')
    run(['e2fsck', '-fy', I / 'system.img'], ok=(0, 1)); run(['resize2fs', I / 'system.img', str(2 * 1024 ** 3 // 4096)])
    for p, src in ((f'{V}/fstab.superman', 'fstab.superman.internal'), (f'{V}/fstab.superman.late', 'fstab.superman.late.internal')):
        replace(I / 'system.img', p, (DEV / 'internal-install' / src).read_bytes(), tmp)
    if not ifdt:
        known = {'4d170538cf547add1443d0d3204c95444af121d78a439b66c571f0b35fdb94b1': '1731e791ef157830d140853f4b073f8de0333c51512fd6a958de89f46465b2ad'}
        ifdt = known.get(sd_contract_fdt) or die('the internal runtime FDT is unknown for this base: give --boot and --runtime-fdt')
    n2, bad = set_contracts(I / 'system.img', tmp, sha(iboot), ifdt, a.config_sha, a.notes_sha)
    if bad: die('files in the internal system image do not match the manifest: ' + ', '.join(bad))
    run(['e2fsck', '-fn', I / 'system.img'])
    env = dict(os.environ, MKE2FS_CONFIG='/dev/null')
    FEAT = 'none,has_journal,ext_attr,dir_index,filetype,extent,sparse_super,large_file,huge_file,uninit_bg,dir_nlink,extra_isize'
    if (I / 'data.img').exists(): (I / 'data.img').unlink()
    run(['mke2fs', '-q', '-F', '-t', 'ext4', '-b', '4096', '-I', '256', '-m', '1', '-L', 'data', '-U', 'a5d1a7a0-7e1e-4c3a-9b1f-5e0000000013',
         '-E', 'hash_seed=a5d1a7a0-7e1e-4c3a-9b1f-5e0000000014,lazy_itable_init=1,lazy_journal_init=1,nodiscard', '-O', FEAT,
         I / 'data.img', str(2560 * 1024 * 1024 // 4096)], env=env)
    run(['e2fsck', '-fn', I / 'data.img'])
    res['internal'] = {'boot_internal_img_sha256': sha(iboot), 'runtime_fdt_sha256': ifdt}
    for name in ('boot', 'system', 'data'):
        img = I / f'{name}.img'; g = I / f'{name}.img.gz'; gz(img, g)
        res['internal'][name] = {'bytes': img.stat().st_size, 'sha256': sha_file(img), 'gz_sha256': sha_file(g)}
        if not a.keep_raw: img.unlink()

    # ---- SD card image (parts are deleted as they are copied in, to keep the peak disk use low) ----
    IMG = O / 'superman-alpha-sdcard.img'
    res['sd'] = {'boot_img_sha256': sha(boot), 'runtime_fdt_sha256': sd_contract_fdt, 'boot_partition_sha256': sha_file(BOOT),
                 'system_sha256': sha_file(SYS), 'manifest_lines': n, 'files_replaced': changed}
    def put(f, part, start, count):
        if part.stat().st_size != count * SECTOR: die(f'{part} does not have the size of its partition')
        f.seek(start * SECTOR)
        with open(part, 'rb') as p: shutil.copyfileobj(p, f, 1 << 20)
        part.unlink()
    with open(IMG, 'wb') as f:
        f.write(pre); f.truncate((parts[2][0] + parts[2][1]) * SECTOR)
        put(f, BOOT, *parts[0]); put(f, SYS, *parts[1])
        DATA = O / 'data.img'
        run(['mke2fs', '-q', '-F', '-t', 'ext4', '-b', '4096', '-I', '256', '-N', '65536', '-m', '1', '-L', 'data', '-U', 'a5d1a7a0-7e1e-4c3a-9b1f-5e0000000003',
             '-E', 'hash_seed=a5d1a7a0-7e1e-4c3a-9b1f-5e0000000004,lazy_itable_init=0,lazy_journal_init=0', '-O', FEAT,
             DATA, str(parts[2][1] * SECTOR // 4096)], env=env)
        run(['e2fsck', '-fn', DATA])
        res['sd']['data_sha256'] = sha_file(DATA)
        put(f, DATA, *parts[2])
    res['sd']['image_sha256'] = sha_file(IMG); res['sd']['bytes'] = IMG.stat().st_size
    gz(IMG, O / 'superman-alpha-sdcard.img.gz'); res['sd']['gz_sha256'] = sha_file(O / 'superman-alpha-sdcard.img.gz')
    if not a.keep_raw: IMG.unlink()
    shutil.rmtree(tmp)
    res['windows_setup_tool'] = {
        '$ImageGzSha': res['sd']['gz_sha256'], '$ImageSha': res['sd']['image_sha256'], '$ImageBytes': res['sd']['bytes'],
        'files\\internal-direct\\*.img.gz': {k: res['internal'][k]['gz_sha256'] for k in ('boot', 'system', 'data')},
        '$InternalImages Sha/Bytes': {k: [res['internal'][k]['sha256'], res['internal'][k]['bytes']] for k in ('boot', 'system', 'data')},
        'files\\internal\\payload\\boot-internal.img': res['internal']['boot_internal_img_sha256'],
        'superman-install-internal.sh': {'RELEASE_BOOT': res['sd']['boot_img_sha256'], 'INTERNAL_BOOT': res['internal']['boot_internal_img_sha256'],
                                         'INTERNAL_FDT': res['internal']['runtime_fdt_sha256']}}
    (O / 'IMAGES.json').write_text(json.dumps(res, indent=2) + '\n')
    print(json.dumps(res, indent=1))

if __name__ == '__main__':
    main()
