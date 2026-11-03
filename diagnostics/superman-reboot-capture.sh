#!/system/bin/sh
# r27: supplemental capture only; no service restart, watchdog or power changes.
PATH=/system/bin:/system/xbin:/vendor/bin
export PATH
umask 077
CAPTURE_DIR=$1
BOOT_ID=$2
[ -d "$CAPTURE_DIR" ] && [ -n "$BOOT_ID" ] || exit 1
# Guard against duplicate invocation for the same capture directory.
mkdir "$CAPTURE_DIR/reboot-diagnostics" 2>/dev/null || exit 1
OUT=$CAPTURE_DIR/reboot-diagnostics

uptime_now() { cat /proc/uptime; }

# Direct SD files retain partial output if reset interrupts a worker. There is
# no extra global sync, so durability still depends on the existing publisher.
# File-size units vary by shell; at <=1024 bytes/unit these logs total <=8 MiB.
terminal_log() {
    buffer=$1
    ulimit -f 4096 || return
    echo "boot_id=$BOOT_ID buffer=$buffer begin=$(uptime_now)"
    # No restart loop: bounded to 30 min from launch even if logd never connects.
    # All system tags include Watchdog, ActivityManager and shutdown messages.
    timeout -k 2 1800 logcat -b "$buffer" -v threadtime -v monotonic -v usec
    result=$?
    echo "end status=$result uptime=$(uptime_now)"
    [ "$result" -eq 0 ]
}
for buffer in system crash; do
    (terminal_log "$buffer" > "$OUT/terminal-$buffer.txt.incomplete" 2>&1 &&
        mv "$OUT/terminal-$buffer.txt.incomplete" "$OUT/terminal-$buffer.txt") &
done

# r33: retain zygote/linker abort text from main even if the routine main-log
# publisher or crash_dump fails. Fatal-only plus linker errors limits traffic on
# the synchronous boot filesystem. Independent, one invocation, <=1 MiB/30 min;
# no full main buffer, restart loop, rotation, or additional sync calls.
(
    ulimit -f 1024 || exit 1
    echo "boot_id=$BOOT_ID buffer=main filters=*:F,linker:E begin=$(uptime_now)"
    timeout -k 2 1800 logcat -b main -v threadtime -v monotonic -v usec '*:F' 'linker:E'
    result=$?
    echo "end status=$result uptime=$(uptime_now)"
    [ "$result" -eq 0 ]
) > "$OUT/terminal-main-fatal.txt.incomplete" 2>&1 && \
    mv "$OUT/terminal-main-fatal.txt.incomplete" "$OUT/terminal-main-fatal.txt" &

# Boot reason properties can be normalized later by bootstat. Sample twice;
# each record includes current boot ID, so a framework restart is distinguishable
# from a kernel reboot once records from both runs are retrieved.
reset_state() {
    ulimit -f 256 || return
    echo "boot_id=$BOOT_ID begin=$(uptime_now)"
    timeout -k 1 3 cat /proc/cmdline /proc/version
    echo "cmdline_version_status=$?"
    for prop in ro.boot.bootreason sys.boot.reason sys.boot.reason.last \
        persist.sys.boot.reason ro.build.version.incremental init.svc.zygote \
        init.svc.bootanim sys.boot_completed sys.powerctl; do
        echo "property=$prop"
        timeout -k 1 2 getprop "$prop"
        echo "status=$?"
    done
    echo "--- USB framework and FunctionFS ---"
    for prop in ro.superman.mainline_usb sys.usb.controller sys.usb.configfs \
        sys.usb.config sys.usb.state sys.usb.ffs.ready init.svc.adbd; do
        echo "property=$prop"
        timeout -k 1 2 getprop "$prop"
    done
    timeout -k 1 3 ls -l /sys/class/udc /dev/usb-ffs/adb /dev/usb-ffs/mtp \
        /dev/usb-ffs/ptp /config/usb_gadget/g1/configs/b.1
    for node in /config/usb_gadget/g1/UDC /config/usb_gadget/g1/idProduct; do
        echo "path=$node"
        timeout -k 1 2 cat "$node"
        echo "status=$?"
    done
    udc_count=0
    for node in /sys/class/udc/*/state; do
        [ -f "$node" ] || continue
        [ "$udc_count" -lt 4 ] || break
        udc_count=$((udc_count + 1))
        echo "path=$node"
        timeout -k 1 2 cat "$node"
        echo "status=$?"
    done
    echo "--- pstore mounts ---"
    timeout -k 1 3 cat /proc/mounts
    echo "mounts_status=$?"
    echo "--- available previous-kernel interfaces ---"
    for node in /sys/fs/pstore /proc/last_kmsg /proc/config.gz; do
        ls -ld "$node"
        echo "probe_status=$? path=$node"
    done
    echo "end uptime=$(uptime_now)"
}
(reset_state > "$OUT/reset-at-start.txt.incomplete" 2>&1 &&
    mv "$OUT/reset-at-start.txt.incomplete" "$OUT/reset-at-start.txt") &
(sleep 60; reset_state > "$OUT/reset-plus60.txt.incomplete" 2>&1 &&
    mv "$OUT/reset-plus60.txt.incomplete" "$OUT/reset-plus60.txt") &

# Read existing records only: do not mount, erase, repair or configure pstore.
# Per-file readers run independently; max 8 records, <=1 MiB each, 5 s timeout.
(
    n=0
    for node in /proc/last_kmsg /sys/fs/pstore/* /proc/config.gz; do
        [ -f "$node" ] && [ -r "$node" ] || continue
        [ "$n" -lt 8 ] || break
        n=$((n + 1))
        (
            ulimit -f 1024 || exit 1
            dest=$OUT/previous-$n-${node##*/}
            echo "source=$node boot_id=$BOOT_ID begin=$(uptime_now)" > "$dest.status"
            timeout -k 1 5 cat "$node" > "$dest.incomplete" 2>> "$dest.status"
            result=$?
            echo "status=$result end=$(uptime_now)" >> "$dest.status"
            if [ "$result" -eq 0 ]; then
                mv "$dest.incomplete" "$dest"
                sha256sum "$dest" >> "$dest.status"
            fi
        ) &
    done
) &

# Serial bounded Java samples. Skip missed windows instead of replaying them.
# debuggerd -j is supported by this tree; it requests a diagnostic stack, not a
# fatal tombstone. Sampling can perturb timing; its outcome is not a deadlock proof.
(
    for target in 360 420 480 540; do
        while :; do
            read seconds rest < /proc/uptime
            seconds=${seconds%%.*}
            [ "$seconds" -ge "$target" ] && break
            sleep 2
        done
        [ "$seconds" -lt $((target + 15)) ] || continue
        (
            ulimit -f 1024 || exit 1
            dest=$OUT/system-server-$target.txt
            {
                echo "boot_id=$BOOT_ID target=$target begin=$(uptime_now)"
                pids=$(timeout -k 1 2 pidof system_server)
                set -- $pids
                # Ambiguous/missing PID is evidence; do not guess a process.
                echo "pids=$pids"
                [ "$#" -eq 1 ] || exit 1
                timeout -k 1 2 cat "/proc/$1/stat"
                timeout -k 2 12 debuggerd -j "$1"
                result=$?
                echo "end status=$result uptime=$(uptime_now)"
                [ "$result" -eq 0 ]
            } > "$dest.incomplete" 2>&1 && mv "$dest.incomplete" "$dest"
        )
    done
) &
wait
