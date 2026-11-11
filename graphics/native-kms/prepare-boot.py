"""CC10: production native-display boot = CC4b kernel/ramdisk/cmdline + CC5 display DTB.

Only the stored DTB changes, so the LK replay of the CC4b runtime FDT predicts
the running FDT exactly (model verified byte-identical on the CC5 test boot).
"""
from pathlib import Path
import hashlib, json, runpy, struct

W = Path(__file__).resolve().parent
C4 = Path('/home/doletrandat/.cache/superman-cc4-cpuidle-20261103')
C5 = Path('/home/doletrandat/.cache/superman-cc5-display-20261103')
R = Path('/run/media/doletrandat/AOSP-BUILD/aosp11')
H = runpy.run_path(str(R / 'device/nokia/superman/scripts/prepare-quarantine-boot.py'))
P4 = runpy.run_path(str(C4 / 'prepare-boot.py'), run_name='cc10')
sha = lambda b: hashlib.sha256(b).hexdigest()

old = (C4 / 'boot-cpuidle.img').read_bytes()
assert sha(old) == 'cc9632a335a9074cae46ecb10fe08ffb693fc2aa170fd189b0b38064928d597f'
nh, oz, od, nr = H['split_boot'](old)
dtb = (C5 / 'dt/display.dtb').read_bytes()
assert sha(dtb) == '3e623c178523432b36be4392e59a5708b59932bff18c954b166d5167ea4232da'
props = P4['properties']
changed = {k for k, v in props(od).items() if props(dtb).get(k) != v}
assert changed == {('/soc/display-subsystem@fd900000', 'status')}, changed

head = bytearray(nh)
struct.pack_into('<I', head, 8, len(oz) + len(dtb))
head[576:608] = H['boot_id'](oz + dtb, nr)
pad = lambda b: b + b'\0' * (-len(b) % len(nh))
image = bytes(head) + pad(oz + dtb) + pad(nr)
hh, zz, dd, rr = H['split_boot'](image)
assert zz == oz and dd == dtb and rr == nr and hh[64:576] == nh[64:576]
(W / 'boot-cc10-kms.img').write_bytes(image)

expected = W / 'runtime-expected.dtb'
expected.write_bytes(dtb)
P4['replay'](expected, props((C4 / 'dt/runtime-before.dtb').read_bytes()))

layout = runpy.run_path(str(R / 'device/nokia/superman/scripts/verify-arm-boot-layout.py'))[
    'verify_boot_layout'](image)
assert layout['safe'], layout
out = {'boot_sha256': sha(image), 'base_boot_sha256': sha(old), 'stored_dtb_sha256': sha(dtb),
       'expected_running_FDT_sha256': sha(expected.read_bytes()),
       'kernel_ramdisk_cmdline_unchanged': True,
       'dtb_changes': 'CC5 display.dtb: MDSS okay, DSI/PHY supplies, 4-lane endpoint, Burton panel@0',
       'boot_layout_safe': True}
(W / 'BOOT-VALIDATION.json').write_text(json.dumps(out, indent=2) + '\n')
print(json.dumps(out, indent=2))
