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
reboot_mode="$kernel_tree/drivers/power/reset/reboot-mode.ko"
qcom_pon="$kernel_tree/drivers/power/reset/qcom-pon.ko"
pm8941_pwrkey="$kernel_tree/drivers/input/misc/pm8941-pwrkey.ko"
output_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/prebuilt

for input in "$zimage" "$dtb" "$reboot_mode" "$qcom_pon" "$pm8941_pwrkey"; do
    if [ ! -f "$input" ]; then
        echo "missing kernel build output: $input" >&2
        exit 1
    fi
done

mkdir -p "$output_dir"
cat "$zimage" "$dtb" > "$output_dir/zImage-dtb"
cp "$reboot_mode" "$qcom_pon" "$pm8941_pwrkey" "$output_dir/"
sha256sum \
    "$zimage" \
    "$dtb" \
    "$output_dir/zImage-dtb" \
    "$output_dir/reboot-mode.ko" \
    "$output_dir/qcom-pon.ko" \
    "$output_dir/pm8941-pwrkey.ko"
