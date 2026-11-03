#!/usr/bin/env python3
"""Prepare an unsealed overlay and derived DT; no kernel/AOSP build or deployment."""
from pathlib import Path
import argparse, hashlib, json, shutil, subprocess, runpy

R = Path('/run/media/doletrandat/AOSP-BUILD/aosp11')
D = Path(__file__).resolve().parent
P30 = R / '.codex-restart/artifacts/r37-p30-composition-lock-20261007'
P34 = R / '.codex-restart/artifacts/r37-p34-native-display-candidate-20261007'
A = R / '.codex-restart/artifacts/r37-p35-native-test-profile-20261008'

def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''): h.update(b)
    return h.hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--phase', choices=['probe', 'flip', 'gpu', 'hwc'], required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--acquire-gate', type=Path, help='Private ARM32 software gate module; required for gpu phase')
    args = p.parse_args(); out = args.output.resolve()
    if not out.is_relative_to(A) or out == A: p.error('Output must be a new directory under the P35 artifact')
    if out.exists(): p.error('Preserve previous output; choose a new directory')
    if shutil.disk_usage(R).free < 200 * 1024**2: p.error('Insufficient capacity')
    if subprocess.check_output(['findmnt', '-T', str(R), '-n', '-o', 'TARGET,FSTYPE']).decode().strip() != '/run/media/doletrandat/AOSP-BUILD ext4': p.error('Unexpected mount')
    rows = []; tree = out / 'overlay'; tree.mkdir(parents=True)
    def add(src, target, expected=None, executable=False):
        src = Path(src); digest = sha(src)
        if expected is not None and digest != expected: raise ValueError(f'Hash mismatch: {src}')
        dst = tree / target.lstrip('/'); dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst); dst.chmod(0o755 if executable else 0o644)
        assert sha(dst) == digest
        rows.append({'source':str(src), 'runtime_path':target, 'sha256':digest, 'size':dst.stat().st_size})
    # Retained P30 files are preferred; active output is accepted only if exact.
    extras = json.loads((P30 / 'extra-component-manifest.json').read_text())['files']
    by_runtime = {('/vendor/' + x['image_path'][len('system/vendor/'):] if x['image_path'].startswith('system/vendor/') else '/' + x['image_path']):(i,x) for i,x in enumerate(extras)}
    lines = (R / 'device/nokia/superman/diagnostics/superman-gpu-startup.sha256').read_text().splitlines()
    for line in lines:
        digest, target = line.split()
        if target.endswith(('gralloc.superman_drm.so','superman-gpu-startup-check','superman-gpu-startup.sh','msm.ko')): continue
        if target in by_runtime:
            i, x = by_runtime[target]
            candidates = [P30/'package-work'/f'extra-{i}', P30/'package-work'/f'extra-{i+1}', Path(x['source'])]
        else:
            # Some unchanged base-image HALs were in startup hashes, not extras.
            relative = 'system'+target if target.startswith('/vendor/') else target.lstrip('/')
            candidates = [R/'out/target/product/superman'/relative]
        src = next((q for q in candidates if q.is_file() and sha(q) == digest), None)
        if src is None: raise ValueError(f'No exact retained source: {target}')
        if '/modules/gpu-diag/' in target: target = target.replace('/modules/gpu-diag/', '/modules/native-p34/')
        add(src, target, digest, '/bin/' in target)
    native = {'gralloc.superman_native.so':'/vendor/lib/hw/gralloc.superman_native.so', 'hwcomposer.superman_native.so':'/vendor/lib/hw/hwcomposer.superman_native.so', 'superman-native-kms-check':'/vendor/bin/superman-native-kms-check', 'msm-p34-unstripped.ko':'/vendor/lib/modules/native-p34/msm.ko', 'superman-fw-kms.ko':'/vendor/lib/modules/native-p34/superman-fw-kms.ko'}
    for x in json.loads((P34/'compiled-artifacts.json').read_text()):
        name = Path(x['path']).name
        add(x['path'], native[name], x['sha256'], name=='superman-native-kms-check')
    if args.phase == 'gpu':
        if not args.acquire_gate or not args.acquire_gate.is_file(): p.error('GPU phase requires the separately verified acquire gate module')
        add(args.acquire_gate, '/vendor/lib/modules/native-p34/superman-acquire-gate.ko')
    for name,target in [('init.headless.rc','/system/etc/init/hw/init.rc'),('init.superman.headless.rc','/vendor/etc/init/hw/init.superman.rc'),('init.superman.p35-hold.rc','/vendor/etc/init/zz-superman-p35-hold.rc'),('superman-native-startup.sh','/vendor/bin/superman-native-startup.sh'),('vdc.headless.rc','/system/etc/init/vdc.rc')]:
        add(D/name, target, executable=name.endswith('.sh'))
    # Deliberately no boot contract. Runtime stops at unsealed before ownership.
    (tree/'vendor/etc/superman-native-components.sha256').write_text(''.join(f"{x['sha256']}  {x['runtime_path']}\n" for x in sorted(rows,key=lambda x:x['runtime_path'])))
    properties = f'ro.superman.native_profile=p35\nro.superman.native_candidate=p34\nro.superman.native_stage={args.phase}\n'
    (out/'candidate-properties.txt').write_text(properties)
    base_cmd = (P34/'hardware-reconnected-RUN01/power-memory.txt').read_text().splitlines()[1]
    cmdline = base_cmd + ' androidboot.superman_native=p34 androidboot.superman_native_stage=' + args.phase
    (out/'candidate-cmdline.txt').write_text(cmdline+'\n')
    kv = json.loads((P30/'kernel-validation.json').read_text())
    boot = (P30/'boot.img').read_bytes()
    if hashlib.sha256(boot).hexdigest() != kv['boot_sha256']: raise ValueError('P30 boot mismatch')
    helper = runpy.run_path(str(R/'device/nokia/superman/scripts/prepare-quarantine-boot.py'))
    _, _, baseline, _ = helper['split_boot'](boot)
    nodes = runpy.run_path(str(D/'seal-contract.py'))['dt_nodes']
    builder = runpy.run_path(str(D/'native-boot-dt.py'))
    builder['build_dt'](baseline, cmdline, out, R/'.codex-restart/builds/r37-p14-integrated-foundation-20261001/scripts/dtc/dtc', nodes)
    rollback = json.loads((P30/'final-image-verification.json').read_text())
    if sha(Path(rollback['image'])) != rollback['image_sha256']: raise ValueError('P30 rollback image hash mismatch')
    record = {'profile':'p35-headless-'+args.phase,'phase':args.phase,'sealed':False,'candidate_boot_built':False,'image_built':False,'deployed':False,'components':rows,'properties':properties.splitlines(),'cmdline':cmdline,'config_sha256':json.loads((P30/'kernel-validation.json').read_text())['config_sha256'],'rollback':{'path':rollback['image'],'sha256':rollback['image_sha256'],'bytes':rollback['image_bytes']},'blocked_UI_gate':'GPU acquire-fence/HWC/SF retirement hardware tests not yet implemented/validated; consumers stay held even on PASS'}
    (out/'profile.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({'output':str(out),'phase':args.phase,'components':len(rows),'sealed':False,'rollback_hash_verified':True},indent=2))

if __name__ == '__main__': main()
