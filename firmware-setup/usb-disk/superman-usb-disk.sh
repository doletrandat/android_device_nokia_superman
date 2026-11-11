#!/system/bin/sh
# CC56: show the phone's internal eMMC (mmcblk0) to the PC as a READ-ONLY USB disk, next to adb.
# Replaces WPinternals' Mass Storage Mode for reading EFIESP/BCD from a PC. The kernel enforces read-only
# (lun ro=1): the PC cannot write the phone.
#   superman-usb-disk.sh start [module.ko]   |   superman-usb-disk.sh stop   |   superman-usb-disk.sh status
set -eu
G=$(ls -d /config/usb_gadget/* | head -n 1)
C=$(ls -d "$G"/configs/* | head -n 1)
F=$G/functions/mass_storage.superman
UDC=$(ls /sys/class/udc | head -n 1)
say() { echo "superman-usb-disk: $*"; echo "superman-usb-disk: $*" > /dev/kmsg; }
# Unbind, change, re-bind with no pause: Android's USB handling re-binds the gadget within a second, and
# configfs refuses to link a function into a bound gadget (EINVAL).
rebind() {
    i=0
    while :; do
        echo "" > "$G/UDC" 2>/dev/null || true
        [ -z "$(cat "$G/UDC")" ] && "$@" && break
        i=$((i + 1)); [ "$i" -lt 20 ] || { say "could not change the gadget while unbound"; return 1; }
    done
    echo "$UDC" > "$G/UDC"
}
case "${1:-}" in
start)
    [ -e /sys/module/usb_f_mass_storage ] || insmod "${2:-/data/local/tmp/usb_f_mass_storage.ko}"
    [ ! -e "$C/mass_storage.superman" ] || { say "already present"; exit 0; }
    add() {
        [ -e "$F" ] || mkdir "$F"
        echo 1 > "$F/lun.0/ro"
        [ "$(cat "$F/lun.0/ro")" = 1 ]
        [ "$(cat "$F/lun.0/file")" = /dev/block/mmcblk0 ] || echo /dev/block/mmcblk0 > "$F/lun.0/file"
        ln -s "$F" "$C/mass_storage.superman"
    }
    rebind add
    say "internal eMMC exported read-only ($(cat "$F/lun.0/file"), ro=$(cat "$F/lun.0/ro"))"
    ;;
stop)
    [ -e "$F" ] || { say "not present"; exit 0; }
    del() { rm -f "$C/mass_storage.superman"; echo "" > "$F/lun.0/file" 2>/dev/null || true; rmdir "$F"; }
    rebind del
    say "removed"
    ;;
status)
    [ -e "$F" ] && echo "exported: $(cat "$F/lun.0/file") ro=$(cat "$F/lun.0/ro")" || echo "not exported"
    ;;
*) exit 64 ;;
esac
