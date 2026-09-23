#!/usr/bin/env python3
"""Bind Superman capture metadata to installed build artifacts before mkfs."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

COMPONENTS = {
    'diagnostic_script_sha256': 'vendor/bin/superman-diag-capture.sh',
    'reboot_capture_script_sha256': 'vendor/bin/superman-reboot-capture.sh',
    'memdiag_binary_sha256': 'bin/superman-memdiag',
    'healthdiag_binary_sha256': 'bin/superman-healthdiag',
    'kerneldiag_binary_sha256': 'bin/superman-kerneldiag',
    'framework_res_sha256': 'framework/framework-res.apk',
    'app_widgets_feature_sha256': 'etc/permissions/android.software.app_widgets.xml',
    'mediaextractor_policy_sha256': 'vendor/etc/seccomp_policy/mediaextractor.policy',
    'soundpicker_permissions_sha256': 'etc/permissions/privapp-permissions-superman.xml',
    'art_compiler_sha256': 'apex/com.android.art.debug/lib/libart-compiler.so',
    'runtime_libc_sha256': 'apex/com.android.runtime/lib/bionic/libc.so',
    'runtime_linker_sha256': 'apex/com.android.runtime/bin/linker',
    'touch_core_sha256': 'vendor/lib/modules/rmi_core.ko',
    'touch_i2c_sha256': 'vendor/lib/modules/rmi_i2c.ko',
    'touch_provenance_sha256': 'vendor/etc/superman-touch-modules.json',
    'reboot_mode_sha256': 'vendor/lib/modules/reboot-mode.ko',
    'qcom_pon_sha256': 'vendor/lib/modules/qcom-pon.ko',
    'pwrkey_sha256': 'vendor/lib/modules/pm8941-pwrkey.ko',
    'power_provenance_sha256': 'vendor/etc/superman-power-modules.json',
    'generic_kl_sha256': 'usr/keylayout/Generic.kl',
    'generic_kcm_sha256': 'usr/keychars/Generic.kcm',
    'virtual_kcm_sha256': 'usr/keychars/Virtual.kcm',
}


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def inspect_framework(path):
    """Check CRCs and Android's UTF-8/no-NUL central-directory name rule."""
    with zipfile.ZipFile(path) as archive, path.open('rb') as raw:
        for required in ('AndroidManifest.xml', 'resources.arsc',
                         'assets/images/android-logo-mask.png',
                         'assets/images/android-logo-shine.png'):
            archive.getinfo(required)
        raw.seek(archive.start_dir)
        for _ in archive.infolist():
            header = raw.read(46)
            if len(header) != 46 or header[:4] != b'PK\x01\x02':
                raise ValueError('Invalid framework central directory')
            length, extra, comment = struct.unpack_from('<HHH', header, 28)
            name = raw.read(length)
            if len(name) != length or b'\0' in name:
                raise ValueError('Invalid framework entry name')
            name.decode('utf-8', errors='strict')
            raw.seek(extra + comment, 1)
        bad = archive.testzip()
        if bad is not None:
            raise ValueError('Framework CRC mismatch: ' + bad)


def generate(template, system, identity, output):
    inspect_framework(system / COMPONENTS['framework_res_sha256'])
    fields = {key: sha256(system / path) for key, path in COMPONENTS.items()}
    provenance = json.loads((system / COMPONENTS['touch_provenance_sha256']).read_text())
    template_fields = dict(line.split('=', 1) for line in template.read_text().splitlines()
                           if '=' in line and not line.startswith('#'))
    if template_fields.get('intended_boot_sha256') != provenance['boot_sha256']:
        raise ValueError('Touch modules require their ABI-verified boot payload')
    for key in ('touch_core_sha256', 'touch_i2c_sha256'):
        if fields[key] != provenance[key]:
            raise ValueError('Touch module differs from ABI-verified artifact: ' + key)
    fields.update(artifact_id=identity, build_version=identity,
                  memdiag_source_status='compiled-component-hashes-generated-before-mkfs')
    power = json.loads((system / COMPONENTS['power_provenance_sha256']).read_text())
    if template_fields.get('intended_boot_sha256') != power['boot_sha256']:
        raise ValueError('Power modules require their ABI-verified boot payload')
    for key in ('reboot_mode_sha256', 'qcom_pon_sha256', 'pwrkey_sha256'):
        if fields[key] != power[key]:
            raise ValueError('Power module differs from ABI-verified artifact: ' + key)
    kept = [line for line in template.read_text().splitlines()
            if line.partition('=')[0] not in fields]
    content = '\n'.join(kept + [key + '=' + value for key, value in fields.items()]) + '\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + '.tmp')
    temporary.write_text(content)
    temporary.chmod(0o644)
    temporary.replace(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('template', 'system', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--identity', required=True)
    args = parser.parse_args()
    generate(args.template, args.system, args.identity, args.output)
