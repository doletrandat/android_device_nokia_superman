#!/usr/bin/env python3
"""Assemble and verify an r10-based full SD image in regular files only."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import stat
import subprocess

BASE_SHA = '379bd676b56c3881b7666efa4fd977194c44dce33ebfa4980572a9d77595ccb7'
P1_START, P1_SIZE = 2048 * 512, 129024 * 512
P2_START, P2_SIZE = 131072 * 512, 3145728 * 512
P3_START, IMAGE_SIZE = 3276800 * 512, 5373952 * 512


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def copy_region(source, dest, size):
    while size:
        data = source.read(min(size, 1024 * 1024))
        require(data, 'Unexpected EOF')
        dest.write(data)
        size -= len(data)


def digest(path, offset=0, size=None):
    if size is None:
        size = path.stat().st_size - offset
    result = hashlib.sha256()
    with path.open('rb') as source:
        source.seek(offset)
        while size:
            data = source.read(min(size, 1024 * 1024))
            require(data, 'Unexpected EOF while hashing')
            result.update(data)
            size -= len(data)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('base', 'boot', 'system', 'output', 'work'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--identity', required=True)
    parser.add_argument('--allow-kernel-replacement', action='store_true',
                        help='Allow a diagnostic zImage replacement while preserving ramdisk and boot parameters')
    parser.add_argument('--base-sha256', default=BASE_SHA,
                        help='Explicit expected hash for a separately verified base image')
    args = parser.parse_args()
    for path in (args.base, args.boot, args.system):
        require(stat.S_ISREG(path.stat().st_mode), 'Input must be a regular file')
    require(not args.output.exists(), 'Refusing to overwrite an image')
    require(args.base.stat().st_size == IMAGE_SIZE and digest(args.base) == args.base_sha256,
            'Base image provenance mismatch')
    require(args.system.stat().st_size == P2_SIZE, 'System size mismatch')
    args.work.mkdir(exist_ok=False)
    work = args.work
    old_p1, p1 = work / 'r10-p1.img', work / 'new-p1.img'
    with args.base.open('rb') as source, old_p1.open('xb') as dest:
        source.seek(P1_START)
        copy_region(source, dest, P1_SIZE)

    def debugfs(image, command, write=False):
        result = subprocess.run(['debugfs'] + (['-w'] if write else []) +
                                ['-R', command, str(image)], check=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        with (work / 'debugfs.log').open('a') as log:
            log.write(command + '\n' + result.stdout + result.stderr)
        return result.stdout

    baseline_boot = work / 'r10-boot.img'
    debugfs(old_p1, 'dump boot.img ' + str(baseline_boot))
    helpers = runpy.run_path(str(Path(__file__).with_name('prepare-quarantine-boot.py')))
    require(digest(baseline_boot) == helpers['BASE_SHA'], 'Embedded baseline boot mismatch')
    bh, bz, bd, br = helpers['split_boot'](baseline_boot.read_bytes())
    nh, nz, nd, nr = helpers['split_boot'](args.boot.read_bytes())
    require(br == nr, 'Ramdisk changed')
    if args.allow_kernel_replacement:
        require(bz != nz, 'Diagnostic kernel replacement was not applied')
    else:
        require(bz == nz, 'Kernel executable changed')
    masked = bytearray(nh)
    masked[8:12], masked[576:608] = bh[8:12], bh[576:608]
    require(bytes(masked) == bh, 'Boot parameters changed')
    require(nd != bd and b'diagnostic-quarantine@f500000\0' in nd, 'Quarantine DTB missing')
    require(nh[576:608] == helpers['boot_id'](nz + nd, nr), 'Boot ID mismatch')
    manifest = debugfs(args.system, 'cat system/etc/superman-v70-build-manifest.txt')
    fields = dict(line.split('=', 1) for line in manifest.splitlines()
                  if '=' in line and not line.startswith('#'))
    require(fields.get('artifact_id') == args.identity and
            fields.get('build_version') == args.identity, 'System identity mismatch')
    require(fields.get('intended_boot_sha256') == digest(args.boot), 'Manifest boot mismatch')
    require(fields.get('intended_dtb_sha256') == helpers['sha'](nd), 'Manifest DTB mismatch')
    for key, filename in [('diagnostic_script_sha256', 'system/vendor/bin/superman-diag-capture.sh'),
                          ('reboot_capture_script_sha256', 'system/vendor/bin/superman-reboot-capture.sh'),
                          ('framework_res_sha256', 'system/framework/framework-res.apk'),
                          ('app_widgets_feature_sha256', 'system/etc/permissions/android.software.app_widgets.xml'),
                          ('mediaextractor_policy_sha256', 'system/vendor/etc/seccomp_policy/mediaextractor.policy'),
                          ('soundpicker_permissions_sha256', 'system/etc/permissions/privapp-permissions-superman.xml'),
                          ('art_compiler_sha256', 'system/apex/com.android.art.debug/lib/libart-compiler.so'),
                          ('runtime_libc_sha256', 'system/apex/com.android.runtime/lib/bionic/libc.so'),
                          ('runtime_linker_sha256', 'system/apex/com.android.runtime/bin/linker'),
                          ('touch_core_sha256', 'system/vendor/lib/modules/rmi_core.ko'),
                          ('touch_i2c_sha256', 'system/vendor/lib/modules/rmi_i2c.ko'),
                          ('touch_provenance_sha256', 'system/vendor/etc/superman-touch-modules.json'),
                          ('reboot_mode_sha256', 'system/vendor/lib/modules/reboot-mode.ko'),
                          ('qcom_pon_sha256', 'system/vendor/lib/modules/qcom-pon.ko'),
                          ('pwrkey_sha256', 'system/vendor/lib/modules/pm8941-pwrkey.ko'),
                          ('power_provenance_sha256', 'system/vendor/etc/superman-power-modules.json'),
                          ('generic_kl_sha256', 'system/usr/keylayout/Generic.kl'),
                          ('generic_kcm_sha256', 'system/usr/keychars/Generic.kcm'),
                          ('virtual_kcm_sha256', 'system/usr/keychars/Virtual.kcm'),
                          ('memdiag_binary_sha256', 'system/bin/superman-memdiag'),
                          ('healthdiag_binary_sha256', 'system/bin/superman-healthdiag'),
                          ('kerneldiag_binary_sha256', 'system/bin/superman-kerneldiag')]:
        target = work / Path(filename).name
        debugfs(args.system, 'dump ' + filename + ' ' + str(target))
        require(fields.get(key) == digest(target), 'Manifest component mismatch: ' + key)
    touch = json.loads((work / 'superman-touch-modules.json').read_text())
    require(touch['boot_sha256'] == digest(args.boot), 'Touch module boot ABI mismatch')
    for key in ('touch_core_sha256', 'touch_i2c_sha256'):
        require(touch[key] == fields[key], 'Touch module provenance mismatch: ' + key)
    framework_check = runpy.run_path(str(Path(__file__).with_name('generate-build-manifest.py')))
    power = json.loads((work / 'superman-power-modules.json').read_text())
    require(power['boot_sha256'] == digest(args.boot), 'Power module boot ABI mismatch')
    for key in ('reboot_mode_sha256', 'qcom_pon_sha256', 'pwrkey_sha256'):
        require(power[key] == fields[key], 'Power module provenance mismatch: ' + key)
    framework_check['inspect_framework'](work / 'framework-res.apk')
    (work / 'manifest.txt').write_text(manifest)
    # Reject the known missing-JNI startup regression before assembling an image.
    required_hashes = {}
    for filename in ('system/lib/librs_jni.so', 'system/bin/superman-healthdiag',
                     'system/bin/superman-kerneldiag'):
        target = work / Path(filename).name
        debugfs(args.system, 'dump ' + filename + ' ' + str(target))
        require(target.is_file(), 'Required system component missing: ' + filename)
        with target.open('rb') as source:
            header = source.read(52)
        require(len(header) == 52 and header[:6] == b'\x7fELF\x01\x01' and
                header[18:20] == b'\x28\x00', 'Expected little-endian ARM32 ELF: ' + filename)
        required_hashes[filename] = digest(target)
    shutil.copyfile(old_p1, p1)
    debugfs(p1, 'rm boot.img', True)
    debugfs(p1, 'write ' + str(args.boot) + ' boot.img', True)
    debugfs(p1, 'set_inode_field boot.img mode 0100664', True)
    with (work / 'p1-fsck.log').open('w') as log:
        subprocess.run(['e2fsck', '-fn', str(p1)], stdout=log, stderr=log, check=True)
    with args.base.open('rb') as source, args.output.open('xb') as dest:
        copy_region(source, dest, IMAGE_SIZE)
        dest.seek(P1_START)
        with p1.open('rb') as component:
            copy_region(component, dest, P1_SIZE)
        dest.seek(P2_START)
        with args.system.open('rb') as component:
            copy_region(component, dest, P2_SIZE)
    require(args.output.stat().st_size == IMAGE_SIZE, 'Final size mismatch')
    require(digest(args.output, 0, P1_START) == digest(args.base, 0, P1_START), 'MBR/prefix changed')
    require(digest(args.output, P3_START) == digest(args.base, P3_START), 'Data partition changed')
    require(digest(args.output, P1_START, P1_SIZE) == digest(p1), 'Boot partition copy mismatch')
    require(digest(args.output, P2_START, P2_SIZE) == digest(args.system), 'System copy mismatch')
    extracted_p1 = work / 'final-p1.img'
    with args.output.open('rb') as source, extracted_p1.open('xb') as dest:
        source.seek(P1_START)
        copy_region(source, dest, P1_SIZE)
    extracted_boot = work / 'final-boot.img'
    debugfs(extracted_p1, 'dump boot.img ' + str(extracted_boot))
    require(extracted_boot.read_bytes() == args.boot.read_bytes(), 'Embedded boot mismatch')
    report = dict(image=str(args.output), image_sha256=digest(args.output),
                  image_size=IMAGE_SIZE, system_sha256=digest(args.system),
                  boot_sha256=digest(extracted_boot), p1_sha256=digest(extracted_p1),
                  dtb_sha256=helpers['sha'](nd), identity=args.identity,
                  required_component_hashes=required_hashes,
                  prefix_and_data_preserved=True, partition_readbacks_match=True)
    (work / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    args.output.with_suffix('.img.sha256').write_text(report['image_sha256'] + '  ' + args.output.name + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
