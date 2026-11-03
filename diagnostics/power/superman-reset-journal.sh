#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Inactive one-shot helper. It does not load the module, open a watchdog
# device, change properties, or write any hardware/sysfs controls.
PATH=/system/bin:/vendor/bin
export PATH
umask 077
[ "$#" -eq 0 ] || exit 64
[ "$(tr '\000' '\n' < /proc/device-tree/compatible)" = "$(printf 'nokia,superman\nqcom,msm8226')" ] || exit 65
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id) || exit 66
case "$BOOT_ID" in *[!0-9a-f-]*|'') exit 66;; esac
[ "${#BOOT_ID}" -eq 36 ] || exit 66
awk '$2 == "/data" && $3 == "ext4" {found=1} END {exit !found}' /proc/mounts || exit 67
available=$(df -k /data | awk 'END {print $4}')
case "$available" in ''|*[!0-9]*) exit 67;; esac
[ "$available" -ge 131072 ] || exit 67
mkdir -p /data/superman-v70 || exit 68
LOG=/data/superman-v70/reset-$BOOT_ID-$$.txt
ulimit -f 64 || exit 68
set -C
exec 3> "$LOG" || exit 68

read_bounded() {
    echo "READ path=$1"
    if [ -r "$1" ]; then
        timeout 2 head -c 4096 "$1"
        read_status=$?
        printf '\nREAD_END status=%s\n' "$read_status"
    else
        echo "UNAVAILABLE"
    fi
}

{
    echo "SUPERMAN_RESET_JOURNAL boot_id=$BOOT_ID one_shot=1 no_hardware_writes=1"
    read_bounded /proc/uptime
    read_bounded /proc/cmdline
    read_bounded /sys/module/superman_reset_observer/parameters/snapshot
    for key in ro.build.description ro.boot.bootreason sys.boot.reason sys.boot_completed; do
        echo "PROPERTY key=$key"
        timeout 2 getprop "$key"
        echo "PROPERTY_END status=$?"
    done
    found=0
    for index in 0 1 2 3; do
        directory=/sys/class/watchdog/watchdog$index
        [ -d "$directory" ] || continue
        found=1
        for attribute in identity bootstatus status state timeout pretimeout nowayout; do
            read_bounded "$directory/$attribute"
        done
    done
    [ "$found" -eq 1 ] || echo "WATCHDOG_SYSFS unavailable=1 current_P70_config_disables_WATCHDOG_SYSFS=1 device_not_opened=1"
    echo "SUPERMAN_RESET_JOURNAL end boot_id=$BOOT_ID"
} >&3 2>&3
exec 3>&-
echo "$LOG"
