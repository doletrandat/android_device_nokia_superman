#!/bin/sh
# SPDX-License-Identifier: Apache-2.0

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/built/linux-tree" >&2
    exit 2
fi

kernel_tree=$1
zimage="$kernel_tree/arch/arm/boot/zImage"
dtb="$kernel_tree/arch/arm/boot/dts/qcom/qcom-msm8226-nokia-superman.dtb"
output_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/prebuilt

for input in "$zimage" "$dtb"; do
    if [ ! -f "$input" ]; then
        echo "missing kernel build output: $input" >&2
        exit 1
    fi
done

mkdir -p "$output_dir"
cat "$zimage" "$dtb" > "$output_dir/zImage-dtb"
sha256sum "$zimage" "$dtb" "$output_dir/zImage-dtb"
