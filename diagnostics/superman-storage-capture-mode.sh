#!/system/bin/sh
# Controlled diagnostic comparison: only the removable capture filesystem.
# Keep explicit collector flushes and all /data durability settings unchanged.
PATH=/system/bin
export PATH
while read -r source target type options rest; do
    [ "$target" = /lumia_sd ] || continue
    case "$source:$type" in
        /dev/mmcblk1p1:ext2|/dev/block/mmcblk1p1:ext2) ;;
        *) echo "SUPERMAN_CAPTURE_ASYNC rejected source=$source type=$type" > /dev/kmsg; exit 1 ;;
    esac
    echo "SUPERMAN_CAPTURE_ASYNC begin options=$options" > /dev/kmsg
    mount -t ext2 -o remount,async "$source" /lumia_sd || {
        echo "SUPERMAN_CAPTURE_ASYNC remount_failed" > /dev/kmsg
        exit 1
    }
    while read -r actual destination filesystem flags remainder; do
        [ "$destination" = /lumia_sd ] || continue
        case ",$flags," in
            *,sync,*) echo "SUPERMAN_CAPTURE_ASYNC verification_failed flags=$flags" > /dev/kmsg; exit 1 ;;
        esac
        echo "SUPERMAN_CAPTURE_ASYNC verified source=$actual flags=$flags" > /dev/kmsg
        exit 0
    done < /proc/mounts
    exit 1
done < /proc/mounts
echo "SUPERMAN_CAPTURE_ASYNC mount_missing" > /dev/kmsg
exit 1
