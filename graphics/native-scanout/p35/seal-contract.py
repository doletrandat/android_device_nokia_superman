#!/usr/bin/env python3
"""Seal a private profile against a separately built, verified boot; never deploy."""
from pathlib import Path
import argparse, hashlib, json, struct, runpy

def digest(b): return hashlib.sha256(b).hexdigest()

def dt_nodes(b):
    h=struct.unpack_from('>10I',b)
    if h[0]!=0xd00dfeed or h[1]!=len(b): raise ValueError('Invalid DT extent')
    pos=h[2]; strings=b[h[3]:h[3]+h[8]]; stack=[]; nodes={}
    while True:
        t=struct.unpack_from('>I',b,pos)[0];pos+=4
        if t==1:
            end=b.index(b'\0',pos);stack.append(b[pos:end].decode());pos=(end+4)&~3;nodes['/'+'/'.join(stack[1:])]={}
        elif t==2:stack.pop()
        elif t==3:
            n,o=struct.unpack_from('>2I',b,pos);pos+=8;name=strings[o:].split(b'\0',1)[0].decode()
            nodes['/'+'/'.join(stack[1:])][name]=b[pos:pos+n];pos=(pos+n+3)&~3
        elif t==4:pass
        elif t==9:return nodes
        else:raise ValueError('Invalid DT token')

def check_dt(candidate, baseline, cmdline):
    helper = runpy.run_path(str(Path(__file__).with_name('native-boot-dt.py')))
    helper['validate_dt'](candidate, baseline, cmdline, dt_nodes)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--profile',type=Path,required=True)
    ap.add_argument('--boot',type=Path,required=True)
    args=ap.parse_args(); p=args.profile.resolve()
    root=Path('/run/media/doletrandat/AOSP-BUILD/aosp11')
    art=root/'.codex-restart/artifacts/r37-p35-native-test-profile-20261008'
    if not p.is_relative_to(art): ap.error('Private P35 profile required')
    r=json.loads((p/'profile.json').read_text())
    if r['phase'] not in ['probe','flip','gpu','hwc']: ap.error('Headless probe/flip/gpu phase required')
    contract=p/'overlay/vendor/etc/superman-native-boot.contract'
    if contract.exists() or r['sealed']: ap.error('Existing seal is immutable; prepare a new profile')
    kv=json.loads((root/'.codex-restart/artifacts/r37-p30-composition-lock-20261007/kernel-validation.json').read_text())
    b=args.boot.read_bytes()
    if b[:8]!=b'ANDROID!' or len(b)<1632: ap.error('Android boot header required')
    ks,ka,rs,ra,ss,sa,ta,page,version,osver=struct.unpack_from('<10I',b,8)
    if page not in [2048,4096] or version!=0 or ss!=0: ap.error('Expected preserved P30 v0 boot layout')
    old=(root/'.codex-restart/artifacts/r37-p30-composition-lock-20261007/boot.img').read_bytes()
    if digest(old)!=kv['boot_sha256']: ap.error('P30 reference boot hash mismatch')
    if (ka,rs,ra,ss,sa,ta,page,version,osver)!=struct.unpack_from('<10I',old,8)[1:]: ap.error('P30 boot layout changed')
    cmd=b[64:576].split(b'\0',1)[0]+b[608:1632].split(b'\0',1)[0]
    if cmd.decode()!=r['cmdline']: ap.error('Boot command line mismatch')
    aligned=lambda n:(n+page-1)//page*page
    if len(b)<page+aligned(ks)+rs: ap.error('Truncated boot image')
    kernel=b[page:page+ks]; ramdisk=b[page+aligned(ks):page+aligned(ks)+rs]
    dt=(p/'candidate.dtb').read_bytes()
    split = runpy.run_path(str(root/'device/nokia/superman/scripts/prepare-quarantine-boot.py'))['split_boot']
    check_dt(dt,split(old)[2],r['cmdline'])
    if not kernel.endswith(dt) or digest(kernel[:-len(dt)])!=kv['zimage_sha256']: ap.error('Kernel/DT identity mismatch')
    if digest(ramdisk)!=kv['ramdisk_sha256']: ap.error('P30 ramdisk preservation mismatch')
    for x in r['components']:
        if digest((p/'overlay'/x['runtime_path'].lstrip('/')).read_bytes())!=x['sha256']: ap.error('Overlay component mismatch: '+x['runtime_path'])
    # Read back the runtime manifest too, including path uniqueness.
    manifest=p/'overlay/vendor/etc/superman-native-components.sha256'
    expected=''.join(f"{x['sha256']}  {x['runtime_path']}\n" for x in sorted(r['components'],key=lambda x:x['runtime_path']))
    if manifest.read_text()!=expected: ap.error('Runtime manifest mismatch')
    sha=digest(b); value=f"{sha} {r['phase']}\n"
    contract.write_text(value); contract.chmod(0o644)
    with manifest.open('a') as f: f.write(f'{digest(value.encode())}  /vendor/etc/superman-native-boot.contract\n')
    r.update(sealed=True,candidate_boot_built=True,boot_path=str(args.boot.resolve()),boot_sha256=sha,dtb_sha256=digest(dt),ramdisk_preserved=True,kernel_preserved=True,system_image_verified=False,deployed=False)
    (p/'sealed-profile.json').write_text(json.dumps(r,indent=2)+'\n')
    print('Sealed private contract; final system-image insertion/readback and deployment remain separate.')

if __name__=='__main__': main()
