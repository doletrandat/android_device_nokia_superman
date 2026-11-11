#!/usr/bin/env bash
# PC side of the Lumia 730 internal-storage install (alpha). Needs adb and about 5 GB free for the backups.
# The phone must be running the alpha from the SD card, connected over USB with adb.
#
#   ./install-internal.sh install [--no-data-backup]   back up, then install Android to the internal storage
#   ./install-internal.sh restore-windows BACKUP_DIR   put the Windows Phone partition table and Data partition back
#                                                      (run from the SD-card Android; then Windows Phone starts again
#                                                      and WPinternals can flash your stock FFU)
set -euo pipefail
trap 'echo "ERROR: command failed (line $LINENO); nothing after this point was done" >&2' ERR
cd "$(dirname "$0")"
PAYLOAD=payload
REMOTE=/data/local/tmp/superman-internal
die() { echo "ERROR: $*" >&2; exit 1; }
# every adb call gets </dev/null: adb would otherwise read (and swallow) the answers to the prompts below
sh_() { adb shell "$@" < /dev/null | tr -d '\r'; }

connect() {
    command -v adb >/dev/null || die "adb not found"
    adb root >/dev/null 2>&1 < /dev/null || true
    adb wait-for-device < /dev/null
    [ "$(sh_ getprop ro.product.device)" = superman ] || [ "$(sh_ getprop ro.hardware)" = superman ] || die "not a Lumia 730 running the alpha"
    [ "$(sh_ id -u)" = 0 ] || die "adb is not root (the alpha build allows adb root)"
    (cd "$PAYLOAD" && sha256sum -c SHA256SUMS >/dev/null) || die "payload damaged"
    sh_ "rm -rf $REMOTE; mkdir -p $REMOTE" >/dev/null
    adb push "$PAYLOAD/." "$REMOTE/" >/dev/null </dev/null
    sh_ "chmod 755 $REMOTE/superman-gpt"
}

install() {
    local data=1; [ "${1:-}" = --no-data-backup ] && data=0
    connect
    sh_ "sh $REMOTE/superman-install-internal.sh check" || die "preflight failed (see above)"
    sh_ "sh $REMOTE/superman-install-internal.sh prepare" || die "planning failed"
    B=backup-$(date +%Y%m%d-%H%M%S); mkdir -p "$B"
    for f in old-primary.bin old-backup.bin new-primary.bin new-backup.bin plan.env PLAN.sha256; do adb pull "$REMOTE/plan/$f" "$B/" >/dev/null </dev/null; done
    (cd "$B" && sha256sum -c PLAN.sha256 >/dev/null) || die "backup copies of the partition table are damaged"
    echo "Partition table backed up to $B/"
    local efi; efi=$(sh_ "for u in /sys/block/mmcblk0/mmcblk0p*/uevent; do grep -qx PARTNAME=EFIESP \$u && grep ^DEVNAME= \$u; done; true" | cut -d= -f2)
    [ -n "$efi" ] || die "EFIESP not found"
    adb exec-out "cat /dev/block/$efi" > "$B/EFIESP.img" < /dev/null
    [ "$(sha256sum "$B/EFIESP.img" | cut -d' ' -f1)" = "$(sh_ "sha256sum /dev/block/$efi" | cut -d' ' -f1)" ] || die "EFIESP backup mismatch"
    echo "EFIESP backed up"
    if [ $data = 1 ]; then
        echo "Backing up the Windows Phone Data partition (about 4.9 GB; a few minutes)..."
        adb exec-out "cat /dev/block/mmcblk0p27" > "$B/Data.img" < /dev/null
        [ "$(sha256sum "$B/Data.img" | cut -d' ' -f1)" = "$(sh_ "sha256sum /dev/block/mmcblk0p27" | cut -d' ' -f1)" ] || die "Data backup mismatch"
        echo "Data backed up"
    else
        echo "WARNING: no Data backup. Windows Phone can then only come back by flashing a stock FFU."
    fi
    (cd "$B" && sha256sum ./* > SHA256SUMS)
    echo
    echo "This ERASES Windows Phone's Data partition and installs Android in its place."
    echo "Every other partition (firmware, calibration, MainOS, EFIESP) stays as it is. Keep $B/ safe."
    read -r -p "Type INSTALL to continue: " a; [ "$a" = INSTALL ] || die "cancelled; nothing was written"
    sh_ "sh $REMOTE/superman-install-internal.sh install $(sha256sum "$B/old-primary.bin" | cut -d' ' -f1)" || die "install failed: see $REMOTE/install.log on the phone"
    adb pull "$REMOTE/install.log" "$B/" >/dev/null </dev/null || true
    echo "Done. Power the phone off, remove the SD card, power it on."
}

restore_windows() {
    local B=${1:?backup folder}
    [ -f "$B/Data.img" ] || die "$B has no Data.img: flash your stock FFU with WPinternals instead"
    (cd "$B" && sha256sum -c SHA256SUMS >/dev/null) || die "backup folder damaged"
    connect
    sh_ "mkdir -p $REMOTE/plan" >/dev/null
    for f in old-primary.bin old-backup.bin new-primary.bin new-backup.bin plan.env; do adb push "$B/$f" "$REMOTE/plan/" >/dev/null </dev/null; done
    sh_ "cd $REMOTE && ./superman-gpt status /dev/block/mmcblk0 | head -n 1" | grep -qx layout=android || die "the internal storage is not in the Android layout"
    read -r -p "Type RESTORE to put Windows Phone's partition table and Data back (Android on the eMMC is erased): " a
    [ "$a" = RESTORE ] || die "cancelled"
    sh_ "cd $REMOTE && ./superman-gpt restore /dev/block/mmcblk0 plan" || die "partition table restore failed"
    . "$B/plan.env"
    L=$(sh_ "cd $REMOTE && ./superman-gpt loop /dev/block/mmcblk0 $OLD_DATA_FIRST $OLD_DATA_LAST")
    echo "Writing Data back (about 4.9 GB)..."
    adb exec-in "cat > $L" < "$B/Data.img"
    sh_ "sync; blockdev --flushbufs $L"
    [ "$(sh_ "sha256sum $L" | cut -d' ' -f1)" = "$(sha256sum "$B/Data.img" | cut -d' ' -f1)" ] || die "Data read-back mismatch"
    sh_ "losetup -d $L"
    echo "Windows Phone restored. Run 'superman-bootmenu windows' and restart (or reboot and use WPinternals)."
}

case "${1:-}" in
install) shift; install "$@" ;;
restore-windows) shift; restore_windows "$@" ;;
*) echo "usage: $0 install [--no-data-backup] | restore-windows BACKUP_DIR" >&2; exit 64 ;;
esac
