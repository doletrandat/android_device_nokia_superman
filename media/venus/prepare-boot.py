"""CC20: permanent boot (CC19 + SMMU resets = <&mmcc VENUS0_RESET>, CC20 kernel) = CC10 boot with the proven Venus DT (bisect A9) and CC18d kernel.

DT vs CC10 stored DTB: reserved venus@f500000 at 0x0f000000/0x500000 (PIL window tail
after the ADSP image), Venus SMMU qcom,msm8226-venus-iommu (TZ-owned global/SMRs,
secure-id 0, ns ctx 0 + secure ctx 1/2, clocks AHB/AXI/VCODEC0 as iface/bus/tbu, no
power-domains), video-codec qcom,msm8226-venus (no power-domains). Kernel = CC4b + the
qcom_iommu Venus cfg and probe/IRQ power guards. Ramdisk and cmdline unchanged.
"""
from pathlib import Path
import hashlib, json, runpy, struct

W = Path(__file__).resolve().parent
C4 = Path('/home/doletrandat/.cache/superman-cc4-cpuidle-20261103')
C10 = Path('/home/doletrandat/.cache/superman-cc10-kmsdefault-20261104')
V = Path('/home/doletrandat/.cache/superman-cc18-venus-20261104')
R = Path('/run/media/doletrandat/AOSP-BUILD/aosp11')
H = runpy.run_path(str(R / 'device/nokia/superman/scripts/prepare-quarantine-boot.py'))
P4 = runpy.run_path(str(C4 / 'prepare-boot.py'), run_name='cc19')
sha = lambda b: hashlib.sha256(b).hexdigest()
props = P4['properties']

old = (C10 / 'boot-cc10-kms.img').read_bytes()
assert sha(old) == '287042b75b05495e0b2bbf5ff308229e618dbe63c6a7fa98e1d3e773a2d72ff7'
nh, oz, od, nr = H['split_boot'](old)
import subprocess
T = R / '.codex-restart/artifacts/pf29-complete-phone-20261030/tools'
d = W / 'venus-reset.dtb'
d.write_bytes((V / 'bisect-A9.dtb').read_bytes())
subprocess.run([str(T / 'fdtput'), '-t', 'x', str(d), '/soc/iommu@fdc84000', 'resets', '2', '2'], check=True)
dtb = d.read_bytes()
kb = json.loads((V / 'KERNEL-BUILD.json').read_text())
z = (V / 'kernel-output/zImage').read_bytes()
assert sha(z) == kb['zImage'] == '31b7283cc6c33f081cf5a09b3cec2bbcf3dfcd8ee8baec0dd62c652f3d52c06d'

bp, ap = props(od), props(dtb)
changed = {k for k, v in bp.items() if ap.get(k) != v}
assert changed == set(), changed            # no existing property touched
added = sorted({k[0] for k in set(ap) - set(bp)})
assert added == ['/reserved-memory/venus@f500000', '/soc/iommu@fdc84000',
                 '/soc/iommu@fdc84000/iommu-ctx@0', '/soc/iommu@fdc84000/iommu-ctx@1000',
                 '/soc/iommu@fdc84000/iommu-ctx@2000', '/soc/video-codec@fdc00000',
                 '/soc/video-codec@fdc00000/video-decoder',
                 '/soc/video-codec@fdc00000/video-encoder'], added
assert ap['/reserved-memory/venus@f500000', 'reg'] == struct.pack('>II', 0x0f000000, 0x500000)
assert ap['/soc/iommu@fdc84000', 'compatible'] == b'qcom,msm8226-venus-iommu\0'
assert ('/soc/iommu@fdc84000', 'power-domains') not in ap
assert ('/soc/video-codec@fdc00000', 'power-domains') not in ap
assert ap['/soc/iommu@fdc84000', 'resets'] == struct.pack('>II', 2, 2)

head = bytearray(nh)
struct.pack_into('<I', head, 8, len(z) + len(dtb))
head[576:608] = H['boot_id'](z + dtb, nr)
pad = lambda b: b + b'\0' * (-len(b) % len(nh))
image = bytes(head) + pad(z + dtb) + pad(nr)
hh, zz, dd, rr = H['split_boot'](image)
assert zz == z and dd == dtb and rr == nr and hh[64:576] == nh[64:576]
(W / 'boot-cc20-venus.img').write_bytes(image)

expected = W / 'runtime-expected.dtb'
expected.write_bytes(dtb)
P4['replay'](expected, props((C4 / 'dt/runtime-before.dtb').read_bytes()))
layout = runpy.run_path(str(R / 'device/nokia/superman/scripts/verify-arm-boot-layout.py'))[
    'verify_boot_layout'](image)
assert layout['safe'], layout
out = {'boot_sha256': sha(image), 'base_boot_sha256': sha(old), 'stored_dtb_sha256': sha(dtb),
       'expected_running_FDT_sha256': sha(expected.read_bytes()),
       'zImage_sha256': kb['zImage'], 'kernel_notes_sha256': kb['notes'],
       'added_nodes': added, 'existing_properties_unchanged': True, 'boot_layout_safe': True}
(W / 'BOOT-VALIDATION.json').write_text(json.dumps(out, indent=2) + '\n')
print(json.dumps({k: out[k] for k in ('boot_sha256', 'expected_running_FDT_sha256',
                                      'kernel_notes_sha256')}, indent=2))
