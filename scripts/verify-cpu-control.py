#!/usr/bin/env python3
"""Validate the isolated control and produce a boot-only review artifact."""
from pathlib import Path
import gzip
import hashlib
import json
import runpy
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[4]
OUT = ROOT / '.codex-restart/builds/r37-cpu-control-20260923'
DEST = ROOT / 'handoff/evidence/r37-cpu-control-20260923'
BASE = ROOT / '.codex-restart/builds/package-r37-compat-cpudiag-20260923/final-boot.img'
HELP = runpy.run_path(str(Path(__file__).with_name('prepare-quarantine-boot.py')))
sha = lambda data: hashlib.sha256(data).hexdigest()


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def module_table(option, path):
    result = subprocess.run(['modprobe', option, str(path)],
                            capture_output=True, text=True)
    if option == '--show-exports' and result.returncode == 1 and 'No data available' in result.stderr:
        return {}  # Modules need not export symbols.
    result.check_returncode()
    return {fields[1]: int(fields[0], 16) for line in result.stdout.splitlines()
            if (fields := line.split())}


def main():
    baseline = BASE.read_bytes()
    require(sha(baseline) == '3926ea3dc1bc6bc129df6aa3f8ebbb906d2e8ab735067a3d847cbfc111bb4d58',
            'Proven boot hash drift')
    header, old_z, dtb, ramdisk = HELP['split_boot'](baseline)
    image = (OUT / 'arch/arm/boot/Image').read_bytes()
    zimage = (OUT / 'arch/arm/boot/zImage').read_bytes()
    reference = (ROOT / 'handoff/evidence/r37-smp-investigation-20260920/captured-r37.config').read_text()
    embedded = (OUT / 'embedded.config').read_text()
    allowed = {'CONFIG_CC_CAN_LINK=y', '# CONFIG_DEBUG_INFO_SPLIT is not set'}
    delta = set(reference.splitlines()) ^ set(embedded.splitlines())
    require(delta <= allowed, 'Embedded control config drift: ' + repr(sorted(delta)))
    require(b'SUPERMAN_SMP' not in image, 'Unexpected markers in control')
    require(zlib.decompress(zimage[zimage.index(b'\x1f\x8b\x08'):], 31) == image,
            'zImage does not decompress to control Image')

    # Preserve and audit the actual ramdisk modules, including storage modules.
    cpio = gzip.decompress(ramdisk)
    offset = 0
    ramdisk_modules = []
    while offset + 110 <= len(cpio):
        require(cpio[offset:offset+6] in (b'070701', b'070702'), 'Unknown ramdisk archive format')
        fields = [int(cpio[offset+6+i*8:offset+14+i*8], 16) for i in range(13)]
        size, namesize = fields[6], fields[11]
        name = cpio[offset+110:offset+110+namesize-1].decode()
        start = (offset + 110 + namesize + 3) & ~3
        data = cpio[start:start+size]
        offset = (start + size + 3) & ~3
        if name == 'TRAILER!!!':
            break
        if name.endswith('.ko'):
            target = DEST / 'ramdisk-modules' / name.replace('/', '_')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            ramdisk_modules.append(target)

    modules = [ROOT / ('device/nokia/superman/prebuilt/' + rel) for rel in (
        'touch-r33/rmi_core.ko', 'touch-r36/rmi_i2c.ko',
        'power-r35/reboot-mode.ko', 'power-r35/qcom-pon.ko',
        'power-r35/pm8941-pwrkey.ko')] + ramdisk_modules
    providers = {f[1]: int(f[0], 16) for line in (OUT / 'vmlinux.symvers').read_text().splitlines()
                 if (f := line.split())}
    for module in modules:
        for name, crc in module_table('--show-exports', module).items():
            require(name not in providers or providers[name] == crc, 'Conflicting export: ' + name)
            providers[name] = crc
    module_reports = []
    failures = []
    for module in modules:
        imports = module_table('--show-modversions', module)
        require(imports, 'Module has no versioned imports: ' + str(module))
        bad = {name: {'required': hex(crc), 'provided': hex(providers[name]) if name in providers else None}
               for name, crc in imports.items() if providers.get(name) != crc}
        info = subprocess.run(['modinfo', '-F', 'vermagic', str(module)], check=True,
                              capture_output=True, text=True).stdout.strip()
        # kernel/module/version.c same_magic ignores the release token when
        # version CRCs are present. The preserved ramdisk dm-mod has a git suffix.
        require(info.partition(' ')[2] == 'SMP preempt mod_unload modversions ARMv7 p2v8',
                'Unexpected module vermagic: ' + info)
        module_reports.append({'path': str(module.relative_to(ROOT)), 'sha256': sha(module.read_bytes()),
                               'imports_checked': len(imports), 'mismatches': bad, 'vermagic': info})
        failures.extend(bad)
    report = {'kind': 'uninstrumented-kernel-control', 'device_test': 'not performed',
              'source_snapshot': '.codex-restart/source-snapshots/kernel-b75aff58cd06-source',
              'baseline_boot_sha256': sha(baseline), 'config_metadata_delta': sorted(delta),
              'image_sha256': sha(image), 'zimage_sha256': sha(zimage),
              'image_byte_equal_to_proven': image == zlib.decompress(old_z[old_z.index(b'\x1f\x8b\x08'):], 31),
              'dtb_sha256': sha(dtb), 'ramdisk_sha256': sha(ramdisk), 'modules': module_reports}
    (DEST / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    require(not failures, 'Module import CRC validation failed; see validation.json')

    new_header = bytearray(header)
    payload = zimage + dtb
    struct.pack_into('<I', new_header, 8, len(payload))
    new_header[576:608] = HELP['boot_id'](payload, ramdisk)
    padded = lambda data: data + bytes((-len(data)) % 2048)
    boot = bytes(new_header) + padded(payload) + padded(ramdisk)
    nh, nz, nd, nr = HELP['split_boot'](boot)
    require(nz == zimage and nd == dtb and nr == ramdisk, 'Boot readback mismatch')
    masked = bytearray(nh)
    masked[8:12], masked[576:608] = header[8:12], header[576:608]
    require(bytes(masked) == header, 'Boot parameters changed')
    dest = DEST / 'boot-control.img'
    require(not dest.exists(), 'Refusing to overwrite control boot artifact')
    dest.write_bytes(boot)
    report.update(boot_control_sha256=sha(boot), boot_size=len(boot), boot_parameters_preserved=True)
    (DEST / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    dest.with_suffix('.img.sha256').write_text(sha(boot) + '  ' + dest.name + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
