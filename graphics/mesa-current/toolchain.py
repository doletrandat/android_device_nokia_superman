#!/usr/bin/python3
from pathlib import Path
import os, sys

A = Path(__file__).resolve().parent
R = Path(os.environ['SUPERMAN_AOSP_ROOT']).resolve()
C = R/'prebuilts/clang/host/linux-x86/clang-r383902b1'
mode = sys.argv[1]
args = sys.argv[2:]
cpp = mode.endswith('cpp')
compiler = str(C/'bin'/('clang++' if cpp else 'clang'))
if mode.startswith('host'):
    base = ['--target=x86_64-linux-gnu', '--gcc-toolchain='+str(R/'prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8'), '--sysroot='+str(R/'prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/sysroot')]
    gcc = R/'prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/lib/gcc/x86_64-linux/4.8.3'
    base += ['-B'+str(gcc), '-L'+str(gcc), '-L'+str(A/'host-libs')]
    if cpp:
        base += ['-stdlib=libc++', '-L'+str(C/'lib64'), '-Wl,-rpath,'+str(C/'lib64')]
    os.execv(compiler, [compiler]+base+args)

base = ['--target=armv7a-linux-androideabi30', '-mcpu=cortex-a7', '-mfloat-abi=softfp',
        '-mfpu=neon-vfpv4', '-fPIC', '-nostdlibinc', '-DANDROID',
        '-B'+str(R/'prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/arm-linux-androideabi/bin')]
for path in ['bionic/libc/include', 'bionic/libc/kernel/uapi', 'bionic/libc/kernel/uapi/asm-arm',
             'bionic/libc/kernel/android/uapi', 'bionic/libc/kernel/android/scsi']:
    base += ['-isystem', str(R/path)]
if cpp:
    base += ['-nostdinc++', '-I'+str(R/'external/libcxx/include'), '-I'+str(R/'external/libcxxabi/include')]
if any(a in args for a in ['-c', '-E', '-S', '--version', '-v', '-dumpmachine']):
    os.execv(compiler, [compiler]+base+args)

shared = '-shared' in args
begin = R/('out/soong/.intermediates/bionic/libc/crtbegin_so/android_arm_armv7-a-neon_cortex-a7/crtbegin_so.o' if shared else
           'out/soong/.intermediates/bionic/libc/crtbegin_dynamic/android_arm_armv7-a-neon_cortex-a7/crtbegin_dynamic.o')
end = R/('out/soong/.intermediates/bionic/libc/crtend_so/android_arm_armv7-a-neon_cortex-a7/obj/bionic/libc/arch-common/bionic/crtend_so.o' if shared else
         'out/soong/.intermediates/bionic/libc/crtend_android/android_arm_armv7-a-neon_cortex-a7/obj/bionic/libc/arch-common/bionic/crtend.o')
libs = ['-Wl,--start-group', '-lc', '-lm', '-ldl', '-llog']
if cpp:
    libs += ['-lc++']
libs += [str(R/'out/target/product/superman/obj/STATIC_LIBRARIES/libatomic_intermediates/libatomic.a'),
         str(C/'lib64/clang/11.0.2/lib/linux/libclang_rt.builtins-arm-android.a'), '-Wl,--end-group']
os.execv(compiler, [compiler]+base+['-nostdlib', '-fuse-ld=lld', '-L'+str(A/'android-libs'), str(begin)]+args+libs+[str(end)])
