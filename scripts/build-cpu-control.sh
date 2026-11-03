#!/usr/bin/env bash
# Kernel-only control. Never installs into product prebuilts or packages an SD.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
source_tree="$root/.codex-restart/source-snapshots/kernel-b75aff58cd06-source"
mode=${1:-control}
[[ "$mode" == control || "$mode" == clock ]] || { echo 'Expected control or clock' >&2; exit 1; }
output=${2:-"$root/.codex-restart/builds/r37-cpu-control-20260923"}
if [[ "$mode" == clock && "$output" == "$root/.codex-restart/builds/r37-cpu-control-20260923" ]]; then
    echo 'Clock experiment requires a separate output directory' >&2
    exit 1
fi
reference="$root/handoff/evidence/r37-smp-investigation-20260920/captured-r37.config"
tools="$root/.codex-restart/toolchains/gcc13.3-ubuntu24.04"
host_tools="$root/.codex-restart/toolchains/host-gcc-resolute"
export PATH="$tools/bin:$root/prebuilts/build-tools/linux-x86/bin:$PATH"
export LD_LIBRARY_PATH="$tools/runtime-current"
export M4="$root/prebuilts/build-tools/linux-x86/bin/m4"
export KBUILD_BUILD_USER=dat KBUILD_BUILD_HOST=DESKTOP-558FK3C
export KBUILD_BUILD_TIMESTAMP='Sun Aug 30 15:20:11 +07 2026'
export KBUILD_BUILD_VERSION=1

# The historical timestamp matches the comparison binary; the output path and
# separate manifest identify this as a new, hardware-untested build.
if grep -q SUPERMAN_SMP "$source_tree/arch/arm/kernel/smp.c" \
    "$source_tree/arch/arm/mach-qcom/platsmp.c" \
    "$source_tree/drivers/firmware/qcom/qcom_scm.c"; then
    echo 'Control source contains diagnostic markers' >&2
    exit 1
fi
mkdir -p "$output"
if [[ ! -f "$output/.config" ]]; then cp "$reference" "$output/.config"; fi
if [[ "$mode" == clock ]]; then
    "$source_tree/scripts/config" --file "$output/.config" --enable QCOM_CLK_APCS_MSM8916
fi
make_args=(-C "$source_tree" O="$output" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi-
    HOSTCC="$host_tools/bin/gcc"
    HOSTCFLAGS="-Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11 -I$host_tools/sysroot/usr/include -I$host_tools/sysroot/usr/include/x86_64-linux-gnu"
    HOSTLDFLAGS="-L$host_tools/sysroot/usr/lib/x86_64-linux-gnu")
build_make="$root/prebuilts/build-tools/linux-x86/bin/make"
"$build_make" "${make_args[@]}" olddefconfig
python3 - "$reference" "$output/.config" "$mode" <<'PY'
from pathlib import Path
import sys
allowed = {'CONFIG_CC_CAN_LINK=y', '# CONFIG_DEBUG_INFO_SPLIT is not set'}
if sys.argv[3] == 'clock':
    allowed |= {'# CONFIG_QCOM_CLK_APCS_MSM8916 is not set', 'CONFIG_QCOM_CLK_APCS_MSM8916=y'}
    assert 'CONFIG_QCOM_CLK_APCS_MSM8916=y' in Path(sys.argv[2]).read_text().splitlines()
delta = set(Path(sys.argv[1]).read_text().splitlines()) ^ set(Path(sys.argv[2]).read_text().splitlines())
if not delta <= allowed:
    raise SystemExit('Control config drift: ' + repr(sorted(delta)))
PY
"$build_make" "${make_args[@]}" -j20 zImage
"$source_tree/scripts/extract-ikconfig" "$output/arch/arm/boot/Image" > "$output/embedded.config"
if grep -aq SUPERMAN_SMP "$output/arch/arm/boot/Image"; then
    echo 'Control binary contains diagnostic markers' >&2
    exit 1
fi
grep -q '^0x71e4016d[[:space:]]\+module_layout' "$output/vmlinux.symvers"
sha256sum "$output/arch/arm/boot/Image" "$output/arch/arm/boot/zImage"
