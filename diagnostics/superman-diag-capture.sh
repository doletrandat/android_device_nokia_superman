#!/system/bin/sh
# Low-overhead, per-boot diagnostic capture for Superman Android bring-up.
PATH=/system/bin:/system/xbin:/vendor/bin
export PATH
umask 077

PUBLISH_ROOT=/lumia_sd/superman-v70
WORK_ROOT=/data/superman-v70-work
BUILD_MANIFEST=/system/etc/superman-v70-build-manifest.txt
PUBLISH_INTERVAL=30
SNAPSHOT_INTERVAL=60

BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
[ -n "$BOOT_ID" ] || BOOT_ID=unknown-$(cat /proc/uptime 2>/dev/null | tr -cd '0-9')
PREFIX=v70-$BOOT_ID
mkdir -p "$PUBLISH_ROOT" "$WORK_ROOT" 2>/dev/null || exit 1

BASE_DIR=$PUBLISH_ROOT/boot-$BOOT_ID
N=0
while :; do
    [ "$N" -eq 0 ] && CAPTURE_DIR=$BASE_DIR || CAPTURE_DIR=$BASE_DIR-$N
    mkdir "$CAPTURE_DIR" 2>/dev/null && break
    [ "$N" -lt 100 ] || exit 1
    N=$((N + 1))
done

WORK_DIR=$WORK_ROOT/boot-$BOOT_ID-$$
mkdir "$WORK_DIR" 2>/dev/null || exit 1
STATE_FILE=$WORK_DIR/state-$PREFIX.txt
KERNEL_FILE=$WORK_DIR/kernel-$PREFIX.txt
ERROR_FILE=$WORK_DIR/errors-$PREFIX.txt
HEARTBEAT_FILE=$WORK_DIR/heartbeat-$PREFIX.txt
SEEN_TOMBSTONES=$WORK_DIR/tombstones-seen-$PREFIX.txt
LOGCAT_MAIN=$WORK_DIR/logcat-main-$PREFIX.txt
LOGCAT_SYSTEM=$WORK_DIR/logcat-system-$PREFIX.txt
LOGCAT_CRASH=$WORK_DIR/logcat-crash-$PREFIX.txt

uptime_now() { cat /proc/uptime 2>/dev/null; }
record_error() { echo "[$(uptime_now)] $*" >> "$ERROR_FILE"; }

# Atomic publication leaves interrupted copies visible as .incomplete-*.
publish_file() {
    source_file=$1
    name=${source_file##*/}
    [ -f "$source_file" ] || return
    tmp=$CAPTURE_DIR/.incomplete-$name-$$
    echo "SUPERMAN_PUBLISH_BEGIN file=$name uptime=$(uptime_now)" > /dev/kmsg
    if cp -p "$source_file" "$tmp" 2>> "$ERROR_FILE"; then
        mv "$tmp" "$CAPTURE_DIR/$name" 2>> "$ERROR_FILE" || record_error "publish rename failed: $name"
    else
        record_error "publish copy failed: $name"
        [ -e "$tmp" ] && mv "$tmp" "$CAPTURE_DIR/.failed-$name-$$" 2>/dev/null
    fi
    echo "SUPERMAN_PUBLISH_END file=$name uptime=$(uptime_now)" > /dev/kmsg
}

publish_all() {
    for file in "$KERNEL_FILE" "$LOGCAT_MAIN" "$LOGCAT_SYSTEM" "$LOGCAT_CRASH" \
        "$STATE_FILE" "$ERROR_FILE" "$HEARTBEAT_FILE" "$SEEN_TOMBSTONES"; do
        publish_file "$file"
    done
    # One periodic global flush; this tree's toybox sync has no -f option.
    # Kernel timing survives a blocked sync without requiring another SD copy.
    echo "SUPERMAN_CAPTURE_SYNC_BEGIN uptime=$(uptime_now)" > /dev/kmsg
    sync
    echo "SUPERMAN_CAPTURE_SYNC_END uptime=$(uptime_now)" > /dev/kmsg
}

snapshot() {
    {
        echo
        echo "===== SNAPSHOT $(uptime_now) ====="
        echo "--- selected properties ---"
        for p in ro.build.version.incremental ro.bionic.cpu_variant init.svc.zygote \
            init.svc.surfaceflinger init.svc.bootanim init.svc.audioserver \
            init.svc.keystore init.svc.vendor.audio-hal init.svc.vendor.keymaster-4-1 \
            sys.boot_completed service.bootanim.exit; do
            echo "$p=$(getprop "$p" 2>/dev/null)"
        done
        echo "--- proc/uptime ---"
        cat /proc/uptime 2>&1
        echo "--- RMI modules ---"
        grep '^rmi_' /proc/modules 2>&1
        echo "--- input devices (16 KiB cap; does not consume events) ---"
        head -c 16384 /proc/bus/input/devices 2>&1
        echo "--- proc/meminfo ---"
        cat /proc/meminfo 2>&1
        echo "--- proc/pressure ---"
        for p in /proc/pressure/cpu /proc/pressure/io /proc/pressure/memory; do
            [ -r "$p" ] && { echo "[$p]"; cat "$p"; }
        done
        echo "--- bounded process state ---"
        count=0
        for proc in /proc/[0-9]*; do
            [ -r "$proc/status" ] || continue
            pid=${proc##*/}
            name=$(sed -n 's/^Name:[[:space:]]*//p' "$proc/status" 2>/dev/null)
            state=$(sed -n 's/^State:[[:space:]]*//p' "$proc/status" 2>/dev/null)
            echo "$pid $name $state"
            count=$((count + 1))
            [ "$count" -ge 160 ] && break
        done
        echo "--- framebuffer ---"
        for p in /sys/class/graphics/fb0/name /sys/class/graphics/fb0/mode \
            /sys/class/graphics/fb0/modes /sys/class/graphics/fb0/stride \
            /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/virtual_size; do
            [ -r "$p" ] && echo "$p=$(cat "$p" 2>/dev/null)"
        done
        echo "--- tombstones ---"
        ls -la /data/tombstones 2>&1
    } >> "$STATE_FILE" 2>> "$ERROR_FILE"
}

mirror_reports() {
    report_dir=$1
    report_prefix=$2
    SEEN_TOMBSTONES=$3
    [ -d "$report_dir" ] || return
    touch "$SEEN_TOMBSTONES" || return
    for tombstone in "$report_dir"/"$report_prefix"*; do
        [ -f "$tombstone" ] || continue
        name=${tombstone##*/}
        before=$(stat -c '%s:%Y' "$tombstone" 2>/dev/null) || continue
        sleep 1
        after=$(stat -c '%s:%Y' "$tombstone" 2>/dev/null) || continue
        [ "$before" = "$after" ] || continue
        hash=$(sha256sum "$tombstone" 2>/dev/null | awk '{print $1}')
        [ -n "$hash" ] || continue
        grep -Fqx "$name|$hash" "$SEEN_TOMBSTONES" 2>/dev/null && continue
        sequence=$(wc -l < "$SEEN_TOMBSTONES" 2>/dev/null)
        sequence=$((sequence + 1))
        dest=${PREFIX}-${sequence}-${name}
        tmp=$CAPTURE_DIR/.incomplete-$dest-$$
        if cp -p "$tombstone" "$tmp" 2>> "$ERROR_FILE" && \
            [ "$(sha256sum "$tmp" 2>/dev/null | awk '{print $1}')" = "$hash" ]; then
            mv "$tmp" "$CAPTURE_DIR/$dest" 2>> "$ERROR_FILE" && echo "$name|$hash" >> "$SEEN_TOMBSTONES"
        else
            record_error "report publish failed: $name"
            [ -e "$tmp" ] && mv "$tmp" "$CAPTURE_DIR/.failed-$dest-$$" 2>/dev/null
        fi
        # The periodic publisher flushes these reports with other captures.
    done
}

logcat_loop() {
    buffer=$1
    file=$2
    while :; do
        while [ "$(getprop init.svc.logd 2>/dev/null)" != running ]; do sleep 1; done
        echo "[$(uptime_now)] logcat -b $buffer started" >> "$STATE_FILE"
        logcat -b "$buffer" -v threadtime -v monotonic -v usec -v printable -f "$file" 2>> "$ERROR_FILE"
        record_error "logcat -b $buffer exited"
        sleep 1
    done
}

{
    echo "SUPERMAN V70 DIAGNOSTIC CAPTURE"
    echo "boot_id=$BOOT_ID"
    echo "capture_dir=$CAPTURE_DIR"
    echo "work_dir=$WORK_DIR"
    echo "uptime=$(uptime_now)"
    echo "cmdline=$(cat /proc/cmdline 2>/dev/null)"
    echo "layout=flat-all-artifacts-in-capture-dir"
} >> "$STATE_FILE" 2>> "$ERROR_FILE"
[ -f "$BUILD_MANIFEST" ] && cp -p "$BUILD_MANIFEST" "$CAPTURE_DIR/build-manifest.txt"

# Independent bounded workers: never wait for the large periodic publisher.
sh /vendor/bin/superman-reboot-capture.sh "$CAPTURE_DIR" "$BOOT_ID" &

{
    echo "SUPERMAN V70 KERNEL CAPTURE"
    echo "boot_id=$BOOT_ID"
    dmesg 2>&1
    dmesg -w 2>&1
} >> "$KERNEL_FILE" 2>> "$ERROR_FILE" &

logcat_loop main "$LOGCAT_MAIN" &
# Seven possible pixel events: four animation exits, boot completion, two timers.
# Each event has two files capped at 8192 shell units (<=112 MiB total at
# 1024 bytes/unit), plus metadata. Retain partial staging files.
# Each event publishes independently so blocked SD I/O cannot serialize sampling.
PIXEL_ROOT=/dev/superman-pixels-$BOOT_ID
mkdir -p "$PIXEL_ROOT"
late_pixels() {
    pixel_event=$1
    mkdir "$PIXEL_ROOT/$pixel_event" 2>/dev/null || return
    pixel_dir=$PIXEL_ROOT/$pixel_event
    (
        pixel_meta=$pixel_dir/state.txt
        {
            echo "event=$pixel_event boot_id=$BOOT_ID begin=$(uptime_now)"
            for node in virtual_size stride bits_per_pixel modes name; do
                echo "fb0/$node=$(cat /sys/class/graphics/fb0/$node 2>&1)"
            done
            # Raw first: a stalled screencap must not prevent the framebuffer read.
            for kind in framebuffer screenshot; do
                echo "$kind begin=$(uptime_now)"
                (ulimit -f 8192
                 if [ "$kind" = framebuffer ]; then
                     [ "$(cat /sys/class/graphics/fb0/virtual_size)" = "720,1280" ] &&
                     [ "$(cat /sys/class/graphics/fb0/stride)" = 2880 ] &&
                     [ "$(cat /sys/class/graphics/fb0/bits_per_pixel)" = 32 ] &&
                     timeout 8 dd if=/dev/graphics/fb0 bs=4096 count=900
                 else
                     timeout 8 screencap -p
                 fi) > "$pixel_dir/$kind.incomplete" 2>> "$pixel_meta"
                result=$?
                bytes=$(stat -c %s "$pixel_dir/$kind.incomplete")
                # Layout is specific to the observed 720x1280x32 simplefb.
                if [ "$kind" = framebuffer ] && [ "$bytes" != 3686400 ]; then result=1; fi
                if [ "$kind" = screenshot ] && [ "$bytes" = 0 ]; then result=1; fi
                echo "$kind end=$(uptime_now) status=$result bytes=$bytes"
                [ "$result" = 0 ] && mv "$pixel_dir/$kind.incomplete" "$pixel_dir/$kind"
                sha256sum "$pixel_dir/$kind"* 2>&1
            done
        } > "$pixel_meta" 2>&1
        echo "SUPERMAN_PIXELS acquired event=$pixel_event uptime=$(uptime_now)" > /dev/kmsg
        # Advisory 32 MiB floor; concurrent log writers can consume this space.
        # Never delete existing evidence.
        available=$(df -k "$CAPTURE_DIR" 2>/dev/null | awk 'END {print $4}')
        case "$available" in ''|*[!0-9]*) return;; esac
        [ "$available" -ge 32768 ] || {
            echo "SUPERMAN_PIXELS publish_skip event=$pixel_event available=$available" > /dev/kmsg
            return
        }
        for pixel_file in "$pixel_dir"/*; do
            pixel_dest=$CAPTURE_DIR/pixels-$PREFIX-$pixel_event-${pixel_file##*/}
            if cp "$pixel_file" "$pixel_dest.copy-incomplete"; then
                mv "$pixel_dest.copy-incomplete" "$pixel_dest"
            fi
        done
        echo "SUPERMAN_PIXELS published event=$pixel_event uptime=$(uptime_now)" > /dev/kmsg
    ) &
}
# Independent, small text captures survive a stalled binary graphics worker.
# Every command has its own capped file, so partial output is already on SD.
# No extra global sync: durability remains with the periodic publisher.
transition_snapshot() {
    event=$1
    echo "SUPERMAN_TRANSITION event=$event uptime=$(uptime_now)" > /dev/kmsg
    mkdir "$WORK_ROOT/transition-$BOOT_ID-$event" 2>/dev/null || return
    available=$(df -k "$CAPTURE_DIR" 2>/dev/null | awk 'END {print $4}')
    case "$available" in ''|*[!0-9]*) return;; esac
    [ "$available" -ge 8192 ] || return
    for section in properties mounts window activity display power layers logcat; do
        (
            dest=$CAPTURE_DIR/transition-$PREFIX-$event-$section.txt
            # Bounded output: 512 shell file-size units per section.
            ulimit -f 512
            {
                echo "event=$event section=$section uptime=$(uptime_now)"
                case "$section" in
                    properties) timeout 5 getprop;;
                    mounts) timeout 5 cat /proc/mounts;;
                    window) timeout 8 dumpsys -t 5 window;;
                    activity) timeout 8 dumpsys -t 5 activity activities;;
                    display) timeout 8 dumpsys -t 5 display;;
                    power) timeout 8 dumpsys -t 5 power;;
                    layers) timeout 8 dumpsys -t 5 SurfaceFlinger;;
                    logcat) timeout 8 logcat -b main -b system -b crash -d -t 300 -v threadtime;;
                esac
                result=$?
                echo "end status=$result uptime=$(uptime_now)"
                [ "$result" -eq 0 ]
            } > "$dest.incomplete" 2>&1 && mv "$dest.incomplete" "$dest"
        ) &
    done
    # Do not wait here: one blocked filesystem write must not hold the watcher.
}
transition_watch() {
    seen_running=0
    exit_count=0
    while :; do
        read seconds rest < /proc/uptime
        [ "${seconds%%.*}" -lt 1800 ] || break
        anim=$(getprop init.svc.bootanim)
        [ "$anim" = running ] && seen_running=1
        if [ "$seen_running" = 1 ] && [ "$anim" = stopped ]; then
            seen_running=0
            exit_count=$((exit_count + 1))
            # Four independent exits fit the diagnostic budget; never overwrite
            # an earlier exit or stop watching after the first boot completion.
            if [ "$exit_count" -le 4 ]; then
                late_pixels "animation-stopped-$exit_count"
                (transition_snapshot "animation-stopped-$exit_count") &
            else
                echo "SUPERMAN_TRANSITION exit_limit count=$exit_count uptime=$(uptime_now)" > /dev/kmsg
                break
            fi
        fi
        sleep 2
    done
}
transition_watch &
# One independent boot-completion observer. Animation exit polling/publication
# must not prevent this event. The pixel worker and text workers are asynchronous;
# their individual timestamps describe sequential observations, not one frame.
boot_complete_watch() {
    while :; do
        read seconds rest < /proc/uptime
        [ "${seconds%%.*}" -lt 1800 ] || return
        if [ "$(timeout -k 1 2 getprop sys.boot_completed)" = 1 ]; then
            late_pixels boot-completed
            (transition_snapshot boot-completed) &
            (sleep 30; transition_snapshot boot-completed-plus30) &
            return
        fi
        sleep 2
    done
}
boot_complete_watch &
# A small independent stream avoids waiting for the large logcat publisher.
# Existing framework messages carry sleep reason and requested display state.
# No wake lock, power-setting change, or synchronous dump on a framework thread.
power_watch() {
    ulimit -f 2048
    power_count=0
    last_power=0
    timeout 1800 logcat -b system -b main -v monotonic -T 1 \
        PowerManagerService:I DisplayManagerService:I DisplayPowerController:V '*:S' |
    while IFS= read -r line; do
        read seconds rest < /proc/uptime
        seconds=${seconds%%.*}
        [ "$seconds" -lt 1800 ] || break
        echo "observed_boottime=$seconds $line"
        case "$line" in
            *'Going to sleep'*|*'Waking up'*|*'Display device changed state:'*)
                echo "SUPERMAN_POWER observed_boottime=$seconds $line" > /dev/kmsg
                # Bound expensive asynchronous dumps separately from the stream.
                if [ "$power_count" -lt 4 ] && [ $((seconds - last_power)) -ge 10 ]; then
                    power_count=$((power_count + 1))
                    last_power=$seconds
                    (transition_snapshot "power-$power_count") &
                fi
                ;;
        esac
    done
}
(power_watch > "$CAPTURE_DIR/power-events-$PREFIX.txt.incomplete" 2>&1) &
for target in 900 1020; do
    (
        while :; do
            read seconds rest < /proc/uptime
            [ "${seconds%%.*}" -ge "$target" ] && break
            sleep 2
        done
        late_pixels "uptime-$target"
        transition_snapshot "uptime-$target"
    ) &
done
logcat_loop system "$LOGCAT_SYSTEM" &
logcat_loop crash "$LOGCAT_CRASH" &
while :; do mirror_reports /data/tombstones tombstone_ "$WORK_DIR/tombstones-seen-$PREFIX.txt"; sleep 2; done &
# Separate worker so a large or still-growing ANR cannot delay tombstones.
# Retain each stable content version and failed/incomplete copies as above.
while :; do mirror_reports /data/anr '' "$WORK_DIR/anr-seen-$PREFIX.txt"; sleep 5; done &
while :; do
    echo "uptime=$(uptime_now) publish_interval=$PUBLISH_INTERVAL" > "$HEARTBEAT_FILE"
    publish_all
    sleep "$PUBLISH_INTERVAL"
done &
while :; do snapshot; sleep "$SNAPSHOT_INTERVAL"; done
