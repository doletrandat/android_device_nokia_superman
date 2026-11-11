#!/system/bin/sh
# superman-bootmenu: choose what the phone boots by default, from Android (root), without a PC.
#   superman-bootmenu status     show the boot menu's default entry
#   superman-bootmenu windows    boot Windows Phone by default (then use WPinternals there: FFU, Mass Storage)
#   superman-bootmenu android    boot the Boot Shim (LK -> Android) by default
# Back to Android from Windows Phone: hold Volume Up while the phone starts (the boot menu's custom action for
# Volume Up starts the Boot Shim), then run "superman-bootmenu android".
# Changes only {bootmgr} default (BCD element 23000003), in place; the BCD is backed up to /data first.
set -eu
PATH=/system/bin:/vendor/bin
export PATH
mode=${1:-status}
case "$mode" in status|windows|android) ;; *) echo "usage: superman-bootmenu status|windows|android" >&2; exit 64 ;; esac
[ "$(id -u)" = 0 ] || { echo "run as root (su)" >&2; exit 1; }
TOOL=${BCD_DEFAULT:-/vendor/bin/bcd-default}
M=/mnt/vendor/superman-efiesp
BACKUPS=/data/vendor/superman/bcd-backups

dev=
for u in /sys/block/mmcblk0/mmcblk0p*/uevent; do
    grep -qx 'PARTNAME=EFIESP' "$u" && dev=/dev/block/$(grep '^DEVNAME=' "$u" | cut -d= -f2)
done
[ -n "$dev" ] || { echo "EFIESP partition not found" >&2; exit 1; }
# After an internal Android install the Windows Phone "Data" partition is gone: Windows Phone cannot start, so do
# not make it the default (restore it first: see the internal-install README).
if [ "$mode" = windows ]; then
    wp=0
    for u in /sys/block/mmcblk0/mmcblk0p*/uevent; do grep -qx 'PARTNAME=Data' "$u" && wp=1; done
    [ $wp = 1 ] || { echo "Windows Phone's Data partition is gone (Android is installed internally); refusing" >&2; exit 1; }
fi
# toybox grep fails on /proc/mounts (and on pipes) with "Invalid argument" on this phone: check in plain shell
while read -r mdev mpoint rest; do
    [ "$mdev" != "$dev" ] || { echo "$dev is already mounted at $mpoint; refusing" >&2; exit 1; }
done < /proc/mounts

mkdir -p "$M"
if [ "$mode" = status ]; then
    mount -t vfat -o ro "$dev" "$M"
    trap 'umount "$M" 2>/dev/null || true' EXIT
    "$TOOL" "$M/EFI/Microsoft/Boot/BCD" status
    exit 0
fi

blockdev --setrw "$dev" 2>/dev/null || true      # the firmware setup may have set it read-only this boot
mount -t vfat -o rw,sync "$dev" "$M"
trap 'sync; umount "$M" 2>/dev/null || true' EXIT
mkdir -p "$BACKUPS"; chmod 0700 "$BACKUPS"
b="$BACKUPS/BCD-$(date +%Y%m%d-%H%M%S)"
cp "$M/EFI/Microsoft/Boot/BCD" "$b"
cmp "$M/EFI/Microsoft/Boot/BCD" "$b"
echo "backup: $b"
"$TOOL" "$M/EFI/Microsoft/Boot/BCD" "$mode"
sync
"$TOOL" "$M/EFI/Microsoft/Boot/BCD" status | head -n 1
echo "superman-bootmenu: default -> $mode" > /dev/kmsg
