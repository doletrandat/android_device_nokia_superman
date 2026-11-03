#!/system/bin/sh
# P70: bounded logs on journaled userdata, no automatic pixel readbacks/dumps.
PATH=/system/bin:/system/xbin:/vendor/bin
export PATH
umask 077
ROOT=/data/superman-v70
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id) || exit 1
case "$BOOT_ID" in *[!0-9a-f-]*|'') exit 1;; esac
[ "${#BOOT_ID}" = 36 ] || exit 1
# Never populate an unmounted /data directory, prune old evidence, or fill it.
awk '$2 == "/data" && $3 == "ext4" {found=1} END {exit !found}' /proc/mounts || exit 1
available=$(df -k /data | awk 'END {print $4}')
case "$available" in ''|*[!0-9]*) exit 1;; esac
[ "$available" -ge 131072 ] || exit 1
mkdir -p "$ROOT" || exit 1
OUT=$ROOT/boot-$BOOT_ID-$$
mkdir "$OUT" || exit 1
# All children inherit a per-file <=4 MiB cap even with 1024-byte shell units.
ulimit -f 4096 || exit 1
{
    echo "boot_id=$BOOT_ID uptime=$(cat /proc/uptime)"
    echo "scope=log-only duration=1800 file_limit=4194304 reserve_kib=131072"
    cat /proc/cmdline /proc/version /proc/mounts
    getprop
} > "$OUT/startup.txt" 2>&1
cp /system/etc/superman-v70-build-manifest.txt "$OUT/build-manifest.txt"
for buffer in main system crash; do
    (
        # No restart loop or rotation: preserve truncated output on cap/reset.
        timeout -k 2 1800 logcat -b "$buffer" -v threadtime -v monotonic -v usec \
            > "$OUT/logcat-$buffer.txt.incomplete" 2>&1
        result=$?
        echo "end status=$result uptime=$(cat /proc/uptime)" >> "$OUT/logcat-$buffer.txt.incomplete"
        [ "$result" -eq 0 ] && mv "$OUT/logcat-$buffer.txt.incomplete" "$OUT/logcat-$buffer.txt"
    ) &
done
# Previous-boot interfaces are copied only when present; no pstore setup/erase.
for node in /proc/last_kmsg /sys/fs/pstore/* /proc/config.gz; do
    [ -f "$node" ] && [ -r "$node" ] || continue
    timeout -k 1 5 cat "$node" > "$OUT/previous-${node##*/}.incomplete" 2>/dev/null && \
        mv "$OUT/previous-${node##*/}.incomplete" "$OUT/previous-${node##*/}"
done
wait
