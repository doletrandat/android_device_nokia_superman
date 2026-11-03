#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Exact P67 laboratory identity only. No power/USB/current/thermal writes.
PATH=/system/bin:/vendor/bin
export PATH
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id) || exit 72
LOG=/data/superman-v70/charge-$BOOT_ID.txt
umask 077
MANIFEST=/vendor/etc/superman-native-components.sha256
CONTRACT=/vendor/etc/superman-native-boot.contract
[ "$#" -eq 0 ] && [ "$(getprop ro.superman.native_sf_layer_candidate)" = p69 ] || exit 64
case "$(getprop ro.build.description)" in *v70-graphicsdiag-r37-smp-p75-gpu-registers-20261016*) ;; *) exit 65;; esac
[ "$(tr '\000' '\n' < /proc/device-tree/compatible)" = "$(printf 'nokia,superman\nqcom,msm8226')" ] || exit 66
read -r boot_sha phase extra < "$CONTRACT"
[ -z "$extra" ] && [ "$phase" = hwc ] && [ "${#boot_sha}" -eq 64 ] || exit 67
case "$boot_sha" in *[!0-9a-f]*) exit 67;; esac
[ "$(sha256sum /lumia_sd/boot.img | cut -d ' ' -f 1)" = "$boot_sha" ] || exit 68
available=$(df -k /data | awk 'END {print $4}')
case "$available" in ''|*[!0-9]*) exit 72;; esac
[ "$available" -ge 131072 ] || exit 72
mkdir -p /data/superman-v70 || exit 72
ulimit -f 1024 || exit 72
[ ! -e "$LOG" ] || exit 72
sha256sum -c "$MANIFEST" > "$LOG" 2>&1 || exit 69
[ "$(zcat /proc/config.gz | sha256sum | cut -d ' ' -f 1)" = c58a41e6079dcc8bfa18657f2765a49b1a6819727c40a78debe9ce49e5250970 ] || exit 70
[ ! -d /sys/module/superman_charge_observer ] || exit 71
insmod /vendor/lib/modules/native-p34/superman-charge-observer.ko >> "$LOG" 2>&1
module_status=$?
echo "SUPERMAN_CHARGE_JOURNAL begin module_status=$module_status samples=13 interval_seconds=15 writes_to_power_hardware=false" >> "$LOG"
echo "SUPERMAN_CHARGE_JOURNAL begin module_status=$module_status log=$LOG" > /dev/kmsg
sample=0
while [ "$sample" -lt 13 ]; do
    {
        echo "CHARGE_SAMPLE index=$sample boot_id=$(cat /proc/sys/kernel/random/boot_id) uptime=$(cat /proc/uptime)"
        for supply in /sys/class/power_supply/*; do
            [ -r "$supply/uevent" ] || continue
            echo "POWER_SUPPLY path=$supply";cat "$supply/uevent"
        done
        for file in /sys/class/udc/*/state /sys/class/udc/*/current_speed /config/usb_gadget/*/configs/*/MaxPower; do
            [ -r "$file" ] || continue
            echo "USB_STATE path=$file value=$(cat "$file")"
        done
        if [ "$module_status" -eq 0 ]; then
            cat /sys/module/superman_charge_observer/parameters/snapshot
            echo "SMBB_SNAPSHOT_read_status=$?"
        fi
        grep -E 'chg-done|chg-fast|chg-trkl|chg-gone|bat-temp-ok|bat-present|usb-valid|dc-valid' /proc/interrupts
        for zone in /sys/class/thermal/thermal_zone*; do
            [ -r "$zone/type" ] && [ -r "$zone/temp" ] || continue
            echo "THERMAL type=$(cat "$zone/type") value=$(cat "$zone/temp")"
        done
        echo "CHARGE_SAMPLE_END index=$sample"
    } >> "$LOG" 2>&1
    sample=$((sample+1))
    [ "$sample" -eq 13 ] || sleep 15
 done
 echo "SUPERMAN_CHARGE_JOURNAL end samples=$sample charge_policy_unchanged=true" >> "$LOG"
 echo "SUPERMAN_CHARGE_JOURNAL end samples=$sample log=$LOG" > /dev/kmsg
