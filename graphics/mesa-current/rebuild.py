#!/usr/bin/env python3
"""Rebuild pinned Android 11 ARMv7 Mesa; never installs to a phone or image."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess, tarfile, zipfile
S=Path(__file__).resolve().parent
R=next(p for p in S.parents if (p/'prebuilts/clang').is_dir())
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir',type=Path,default=R/'out/superman-mesa-current')
p.add_argument('--verify-only',action='store_true')
a=p.parse_args(); A=a.build_dir.resolve()
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for line in (S/'INPUTS.sha256').read_text().splitlines():
 h,rel=line.split();assert digest(S/rel)==h,rel
if a.verify_only:
 print('PASS: pinned sources, tools, patches and candidate prebuilts');raise SystemExit
A.mkdir(parents=True,exist_ok=True)
for name in ['mesa-26.2.5','libdrm-2.4.134']:
 if not (A/name).exists():
  with tarfile.open(S/'upstream'/(name+'.tar.xz')) as t:t.extractall(A,filter='data')
changes=json.loads((S/'patches/CHANGES.json').read_text())
for row in changes:
 q=A/'mesa-26.2.5'/row['file']
 if digest(q)==row['before_sha256']:
  subprocess.run(['patch','-p1','--forward','--input',str(S/'patches'/(q.name+'.patch'))],cwd=A/'mesa-26.2.5',check=True)
 assert digest(q)==row['after_sha256'],q
cache=A/'mesa-26.2.5/subprojects/packagecache';cache.mkdir(parents=True,exist_ok=True)
for q in (S/'upstream/packagecache').iterdir():shutil.copy2(q,cache/q.name)
(A/'host-python').mkdir(exist_ok=True)
for q in (S/'build-tools').glob('*.whl'):
 with zipfile.ZipFile(q) as z:z.extractall(A/'host-python')
(A/'host-bison/bin').mkdir(parents=True,exist_ok=True)
shutil.copy2(S/'build-tools/bison',A/'host-bison/bin/bison');(A/'host-bison/bin/bison').chmod(0o755)
(A/'host-bison/share').mkdir(exist_ok=True)
with tarfile.open(S/'build-tools/bison-data.tar.gz') as t:t.extractall(A/'host-bison/share',filter='data')
(A/'host-libs').mkdir(exist_ok=True)
gcc=R/'prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8'
q=A/'host-libs/libgcc_s.so'
if not q.exists():q.symlink_to(gcc/'x86_64-linux/lib64/libgcc_s.so.1')
(A/'android-libs').mkdir(exist_ok=True)
for name in ['libc','libm','libdl','liblog','libsync','libhardware','libnativewindow','libc++','libz']:
 src=R/'out/target/product/superman/obj/SHARED_LIBRARIES'/(name+'_intermediates')/(name+'.so');assert src.is_file(),src
 q=A/'android-libs'/(name+'.so')
 if not q.exists():q.symlink_to(src)
shutil.copy2(S/'toolchain.py',A/'toolchain.py')
# The checked-in .ini templates retain exact qualified flags; replace only paths.
old='/run/media/doletrandat/AOSP-BUILD/aosp11';oldbuild=old+'/.codex-restart/artifacts/p150-mesa-current-upgrade-20261028'
for name in ['android-arm.ini','host.ini']:
 text=(S/name).read_text().replace(oldbuild,str(A)).replace(old,str(R));(A/name).write_text(text)
env=os.environ.copy();env['SUPERMAN_AOSP_ROOT']=str(R);env['PYTHONPATH']=str(A/'host-python')
env['PATH']=str(A/'host-bison/bin')+':'+str(R/'prebuilts/build-tools/linux-x86/bin')+':'+env['PATH']
env['M4']=str(R/'prebuilts/build-tools/linux-x86/bin/m4');env['BISON_PKGDATADIR']=str(A/'host-bison/share')
env['PKG_CONFIG_LIBDIR']=str(A/'prefix/lib/pkgconfig');env['PKG_CONFIG_PATH']=''
meson=['/usr/bin/python3','-m','mesonbuild.mesonmain']
def run(cmd):
 print(' '.join(map(str,cmd)),flush=True);subprocess.run(list(map(str,cmd)),env=env,cwd=R,check=True)
pc=A/'prefix/lib/pkgconfig';pc.mkdir(parents=True,exist_ok=True)
(pc/'zlib.pc').write_text(f'Name: zlib\nDescription: Android platform zlib\nVersion: 1.2.11\nLibs: -L{A}/android-libs -lz\nCflags: -I{R}/external/zlib\n')
for project in ['libdrm','mesa']:
 cmd=json.loads((S/(project+'-setup-command.json')).read_text())[1:]
 cmd=[s.replace(oldbuild,str(A)).replace(old,str(R)) for s in cmd]
 if (A/('build-'+project)/'build.ninja').exists():cmd.insert(1,'--reconfigure')
 if project=='mesa':cmd+=['-Degl-lib-suffix=_mesa','-Dgles-lib-suffix=_mesa','-Dgbm-backends-path=/vendor/lib/gbm']
 run(meson+cmd)
 run([R/'prebuilts/build-tools/linux-x86/bin/ninja','-C',A/('build-'+project),'-j20'])
 run(meson+['install','-C',A/('build-'+project),'--no-rebuild']+(['--tags','runtime'] if project=='mesa' else []))
payload=A/'payload';payload.mkdir(exist_ok=True);(payload/'gbm').mkdir(exist_ok=True)
for rel in ['libdrm.so','libgbm_mesa.so','libgallium_dri.so','libEGL_mesa.so','libGLESv1_CM_mesa.so','libGLESv2_mesa.so','gbm/dri_gbm.so']:
 shutil.copy2(A/'prefix/lib'/rel,payload/rel)
run(['/usr/bin/python3',A/'toolchain.py','arm-cpp','-shared','-Wl,-soname,libGLES_mesa.so','-Wl,--no-as-needed','-L'+str(A/'prefix/lib'),'-lEGL_mesa','-lGLESv1_CM_mesa','-lGLESv2_mesa','-o',payload/'libGLES_mesa.so'])
# Strip only non-runtime debugging data, matching the qualified payload.
for q in sorted(payload.rglob('*.so')):
 if q.name == 'libGLES_mesa.so': continue
 run([R/'prebuilts/clang/host/linux-x86/clang-r383902b1/bin/llvm-strip','--strip-debug',q])
# Candidate output stays outside product prebuilts until hardware requalification.
rows={str(q.relative_to(payload)):{'sha256':digest(q),'bytes':q.stat().st_size} for q in sorted(payload.rglob('*.so'))}
(A/'REBUILT-PAYLOAD.json').write_text(json.dumps(rows,indent=2)+'\n');print('Candidate built; requalify before replacing product prebuilts.')
