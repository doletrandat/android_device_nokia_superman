#!/usr/bin/env bash
# Build the Lumia 730 (superman) kernel from a kernel tree, the way the shipped one was built.
#
#   build/build-kernel.sh KERNEL_SRC OUT_DIR [extra make arguments...]
#
# KERNEL_SRC  the shipped tree (release source/linux-6.16-rc4-superman.tar.xz) or the kernel repository
# OUT_DIR     a NEW, empty directory. Never reuse an old output tree: stale objects once produced a kernel that
#             "matched" sources it was not built from.
# Config      KERNEL_SRC/superman.config, else arch/arm/configs/lumia_superman_qualified.config, or $CONFIG.
# Toolchain   arm-linux-gnueabi- GCC 13.3 (Ubuntu 24.04: apt install gcc-arm-linux-gnueabi). Override with
#             CROSS_COMPILE=...; MAKE=... picks another make; extra arguments go to make (HOSTCC=..., -j8).
#
# The four KBUILD_BUILD_* values below are the ones of the shipped kernel. With them, the same compiler and the
# same sources, the decompressed Image equals the shipped one except for two 20-byte build IDs (vDSO and kernel
# notes), which depend on the output directory. A new build ID changes the "notes" hash, which the phone checks:
# see DEVELOPING.md, "Hash checks".
set -euo pipefail
[ $# -ge 2 ] || { sed -n '2,16p' "$0" >&2; exit 64; }
SRC=$(readlink -f "$1"); OUT=$2; shift 2
[ -f "$SRC/Makefile" ] || { echo "not a kernel tree: $SRC" >&2; exit 1; }
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "$OUT is not empty: use a fresh output directory" >&2; exit 1; fi
mkdir -p "$OUT"; OUT=$(readlink -f "$OUT")
CFG=${CONFIG:-}
if [ -z "$CFG" ]; then
    for c in "$SRC/superman.config" "$SRC/arch/arm/configs/lumia_superman_qualified.config"; do [ -f "$c" ] && { CFG=$c; break; }; done
fi
[ -f "$CFG" ] || { echo "no config found (set CONFIG=...)" >&2; exit 1; }
: "${CROSS_COMPILE:=arm-linux-gnueabi-}" "${MAKE:=make}"
command -v "${CROSS_COMPILE}gcc" >/dev/null || { echo "${CROSS_COMPILE}gcc not found" >&2; exit 1; }
export LC_ALL=C
export KBUILD_BUILD_USER=${KBUILD_BUILD_USER:-dat} KBUILD_BUILD_HOST=${KBUILD_BUILD_HOST:-DESKTOP-558FK3C}
export KBUILD_BUILD_TIMESTAMP=${KBUILD_BUILD_TIMESTAMP:-Sun Aug 30 15:20:11 +07 2026} KBUILD_BUILD_VERSION=${KBUILD_BUILD_VERSION:-1}
cp "$CFG" "$OUT/.config"
before=$(sha256sum "$OUT/.config" | cut -d' ' -f1)
mk() { "$MAKE" -C "$SRC" O="$OUT" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" -j"$(nproc)" "$@"; }
mk "$@" olddefconfig > "$OUT/build.log" 2>&1
after=$(sha256sum "$OUT/.config" | cut -d' ' -f1)
[ "$before" = "$after" ] || echo "note: olddefconfig changed the config ($before -> $after); the phone pins the config hash" >&2
mk "$@" zImage modules dtbs >> "$OUT/build.log" 2>&1 || { tail -n 30 "$OUT/build.log" >&2; exit 1; }
"${CROSS_COMPILE}objcopy" --dump-section .notes="$OUT/kernel.notes" "$OUT/vmlinux" "$OUT/vmlinux.discard" && rm -f "$OUT/vmlinux.discard"
echo "zImage  $(sha256sum "$OUT/arch/arm/boot/zImage" | cut -d' ' -f1)  $OUT/arch/arm/boot/zImage"
echo "Image   $(sha256sum "$OUT/arch/arm/boot/Image" | cut -d' ' -f1)"
echo "config  $after   (audio-boot.contract field 3; shipped: 76cf12182ed0bea29aa7eb0916766b0b4f5715e9fb41dfed53c962d86142af73)"
echo "notes   $(sha256sum "$OUT/kernel.notes" | cut -d' ' -f1)   (audio-boot.contract field 4, venus/camera kernel allowlists)"
echo "release $(cat "$OUT/include/config/kernel.release")"
