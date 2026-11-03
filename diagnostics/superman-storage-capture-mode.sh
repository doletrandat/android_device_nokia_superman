#!/system/bin/sh
# Protect the unjournaled removable boot partition after startup validation.
# Log collectors use journaled /data. No filesystem repair or block writes here.
PATH=/system/bin
export PATH
while read -r source target type options rest; do
    [ "$target" = /lumia_sd ] || continue
    case "$source:$type" in
        /dev/mmcblk1p1:ext2|/dev/block/mmcblk1p1:ext2) ;;
        *) echo "SUPERMAN_BOOT_RO rejected source=$source type=$type" > /dev/kmsg; exit 1 ;;
    esac
    # The inherited ramdisk logger holds a writable FD. The kernel collector
    # confirms that exact child exited after its replacement records are durable.
    # A missed/unsupported handoff fails closed: no signal-by-name or forced mount.
    attempt=0
    while ! mount -t ext2 -o remount,ro "$source" /lumia_sd; do
        attempt=$((attempt+1))
        [ "$attempt" -lt 10 ] || {
            echo "SUPERMAN_BOOT_RO remount_failed" > /dev/kmsg
            exit 1
        }
        sleep 1
    done
    while read -r actual destination filesystem flags remainder; do
        [ "$destination" = /lumia_sd ] || continue
        [ "$actual" = "$source" ] && [ "$filesystem" = ext2 ] || exit 1
        case ",$flags," in
            *,ro,*) echo "SUPERMAN_BOOT_RO verified flags=$flags" > /dev/kmsg; exit 0 ;;
        esac
        exit 1
    done < /proc/mounts
    exit 1
done < /proc/mounts
echo "SUPERMAN_BOOT_RO mount_missing" > /dev/kmsg
exit 1
