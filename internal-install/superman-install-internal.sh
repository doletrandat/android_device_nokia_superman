#!/system/bin/sh
# superman-install-internal: copy the running SD-card Android onto the Lumia 730's internal eMMC (alpha).
#
# Run as root from Android booted from the SD card, from the payload folder (install-internal.sh on the PC does
# this over adb):
#   sh superman-install-internal.sh check              preflight only; nothing is written
#   sh superman-install-internal.sh prepare            preflight + plan the new GPT into ./plan (backup copies of
#                                                      the current GPT); nothing is written to the eMMC
#   sh superman-install-internal.sh install SHA256     write it all. SHA256 = sha256 of plan/old-primary.bin, i.e.
#                                                      proof that the PC has saved the backups
# Layout (Windows Phone "Data" space only; every other partition untouched, MainOS kept):
#   p27 "cache" 64 MiB  ext2 LUMIA_BOOT  copy of the SD boot partition, boot.img = internal variant
#   p30 "android_system" 2 GiB  ext4     copy of the SD system, grown; fstab/contracts switched to the eMMC
#   p31 "android_data"  rest    ext4     new, empty
# Every step is verified by read-back hashes. Re-running "install" after an interruption continues safely.
# Overrides for tests: DISK (default /dev/block/mmcblk0), SRC_BOOT, SRC_SYSTEM.
set -eu
PATH=/system/bin
export PATH
cd "$(dirname "$0")"
HERE=$(pwd)
DISK=${DISK:-/dev/block/mmcblk0}
SRC_BOOT=${SRC_BOOT:-/dev/block/mmcblk1p1}
SRC_SYSTEM=${SRC_SYSTEM:-/dev/block/mmcblk1p2}
PLAN=$HERE/plan
MNT=/mnt/superman-install
LOG=$HERE/install.log

RELEASE_BOOT=340472e982d14211eab9e8b357066b7792aea46479ae9f4c5073a90ba51a2f8c      # SD release boot.img (CC60)
INTERNAL_BOOT=7b134610a18a551e1ad603b0225331cab5ebb656c75e469977ade427117d1d16     # boot-internal.img (CC59)
INTERNAL_FDT=1731e791ef157830d140853f4b073f8de0333c51512fd6a958de89f46465b2ad      # its predicted runtime FDT

say() { echo "superman-install: $*"; echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$LOG"; }
die() { say "ERROR: $*"; cleanup; exit 1; }
LOOPS=$HERE/loops.txt   # a file: loop() runs in $(...) subshells, so a shell variable would not survive
cleanup() {   # umount -D: toybox umount would otherwise free the loop device itself
    umount -D "$MNT" 2>/dev/null || true
    [ -f "$LOOPS" ] && while read -r l; do losetup -d "$l" 2>/dev/null || true; done < "$LOOPS"
    rm -f "$LOOPS"
}
loop() {   # loop FIRST_SECTOR LAST_SECTOR -> prints the loop device covering that range of $DISK
    # superman-gpt loop: 64-bit offsets (toybox losetup -o fails above 2 GiB on 32-bit Android)
    l=$("$HERE/superman-gpt" loop "$DISK" "$1" "$2") || die "cannot attach a loop device"
    echo "$l" >> "$LOOPS"
    [ "$(blockdev --getsz "$l")" = $(($2 - $1 + 1)) ] || die "loop device has the wrong size"
    echo "$l"
}
hash_region() {  # hash_region DEV SKIP_MiB COUNT_MiB (offsets stay small: DEV is a loop device or an SD partition)
    dd if="$1" bs=1048576 skip="$2" count="$3" 2>/dev/null | sha256sum | cut -d' ' -f1
}
mounted_from() { # mounted_from MOUNTPOINT -> device (plain shell: toybox grep fails on /proc/mounts here)
    while read -r d m rest; do [ "$m" = "$1" ] && { echo "$d"; return 0; }; done < /proc/mounts
    return 0
}

preflight() {
    [ "$(id -u)" = 0 ] || die "run as root"
    (cd "$HERE" && sha256sum -c SHA256SUMS >/dev/null 2>&1) || die "payload damaged (SHA256SUMS)"
    [ "$(sha256sum boot-internal.img | cut -d' ' -f1)" = $INTERNAL_BOOT ] || die "boot-internal.img is not the expected build"
    case "$(mounted_from /)" in "$SRC_SYSTEM"|/dev/block/mmcblk1p2) ;; *) die "Android is not running from the SD card" ;; esac
    case "$(mounted_from /lumia_sd)" in "$SRC_BOOT"|/dev/mmcblk1p1|/dev/block/mmcblk1p1) ;; *) die "/lumia_sd is not the SD boot partition" ;; esac
    [ "$(sha256sum /lumia_sd/boot.img | cut -d' ' -f1)" = $RELEASE_BOOT ] || die "this is not the alpha release (boot.img hash); internal install needs it unmodified"
    say "checking the system files (native manifest)"
    sha256sum -c /vendor/etc/superman-native-components.sha256 > "$HERE/manifest-check.txt" 2>&1 || die "system files differ from the release (see manifest-check.txt)"
    while read -r d m rest; do
        case "$d" in "$DISK"p*|/dev/block/mmcblk0p*|/dev/mmcblk0p*) die "$d (internal) is mounted at $m; reboot and retry" ;; esac
    done < /proc/mounts
    # sizes in 512-byte sectors: Android's shell arithmetic is 32-bit
    sb=$(blockdev --getsz "$SRC_BOOT"); ss=$(blockdev --getsz "$SRC_SYSTEM")
    [ $((sb % 2048)) = 0 ] && [ "$sb" -le $((64 * 2048)) ] || die "unexpected SD boot partition size $sb sectors"
    [ $((ss % 2048)) = 0 ] && [ "$ss" -le $((2048 * 2048)) ] || die "unexpected SD system partition size $ss sectors"
    cap=$(cat /sys/class/power_supply/*bif*/capacity 2>/dev/null | head -n 1)
    [ -z "$cap" ] || [ "$cap" -ge 40 ] || die "battery at $cap %: charge to 40 % or more first"
    ./superman-gpt status "$DISK" > "$HERE/gpt-status.txt" || die "cannot read the internal partition table"
    layout=$(head -n 1 "$HERE/gpt-status.txt"); layout=${layout#layout=}
    say "preflight OK: internal layout=$layout, SD boot $((sb / 2048)) MiB, SD system $((ss / 2048)) MiB, battery ${cap:-?} %"
}

case "${1:-}" in
check)
    preflight
    cat "$HERE/gpt-status.txt"
    ;;
prepare)
    preflight
    [ "$layout" = windows ] || die "the internal storage is not in the Windows Phone layout ($layout)"
    rm -rf "$PLAN"; mkdir -p "$PLAN"
    ./superman-gpt plan "$DISK" "$PLAN" > /dev/null || die "planning failed"
    (cd "$PLAN" && sha256sum old-primary.bin old-backup.bin new-primary.bin new-backup.bin > PLAN.sha256)
    cat "$PLAN/plan.env"
    say "plan ready; old-primary sha256 $(sha256sum "$PLAN/old-primary.bin" | cut -d' ' -f1)"
    ;;
install)
    want=${2:-}
    preflight
    [ -f "$PLAN/plan.env" ] || die "run prepare first"
    (cd "$PLAN" && sha256sum -c PLAN.sha256 >/dev/null 2>&1) || die "plan files damaged"
    [ -n "$want" ] && [ "$(sha256sum "$PLAN/old-primary.bin" | cut -d' ' -f1)" = "$want" ] || die "backup confirmation does not match"
    . "$PLAN/plan.env"
    [ "$DISK_SECTORS" = "$(blockdev --getsz "$DISK")" ] || die "plan is for another disk size"
    trap cleanup EXIT
    mkdir -p "$MNT"

    say "1/4 partition table"
    ./superman-gpt write "$DISK" "$PLAN" >> "$LOG" 2>&1 || die "GPT write failed (nothing else was written)"
    ./superman-gpt status "$DISK" | head -n 1 | while read -r l; do [ "$l" = layout=android ] || exit 1; done || die "GPT not in the Android layout after writing"

    say "2/4 boot partition (copy of the SD boot partition, internal boot.img)"
    n=$(($(blockdev --getsz "$SRC_BOOT") / 2048))
    L=$(loop $BOOT_FIRST $BOOT_LAST)
    dd if="$SRC_BOOT" of="$L" bs=1048576 count=$n conv=fsync 2>> "$LOG"
    blockdev --flushbufs "$L"
    [ "$(hash_region "$SRC_BOOT" 0 $n)" = "$(hash_region "$L" 0 $n)" ] || die "boot copy read-back mismatch"
    # ext2 filesystem, ext4 driver (the ext2 driver refuses to mount while SELinux is active)
    mount -t ext4 -o rw,sync "$L" "$MNT" || die "cannot mount the new boot partition"
    for f in "$MNT"/boot*.img; do [ "$f" = "$MNT/boot.img" ] || die "unexpected extra boot image $f on the SD boot partition"; done
    rm -f "$MNT"/lumia-android-kmsg*.txt
    cat boot-internal.img > "$MNT/boot.img"; sync
    umount -D "$MNT"
    e2fsck -fn "$L" >> "$LOG" 2>&1 || die "boot filesystem check failed"
    mount -t ext4 -o ro "$L" "$MNT"
    [ "$(sha256sum "$MNT/boot.img" | cut -d' ' -f1)" = $INTERNAL_BOOT ] || die "internal boot.img read-back mismatch"
    umount -D "$MNT"

    n=$(($(blockdev --getsz "$SRC_SYSTEM") / 2048))
    say "3/4 system (copy of the SD system: $n MiB, this takes a few minutes)"
    L=$(loop $SYSTEM_FIRST $SYSTEM_LAST)
    dd if="$SRC_SYSTEM" of="$L" bs=1048576 count=$n conv=fsync 2>> "$LOG"
    blockdev --flushbufs "$L"
    [ "$(hash_region "$SRC_SYSTEM" 0 $n)" = "$(hash_region "$L" 0 $n)" ] || die "system copy read-back mismatch"
    e2fsck -fy "$L" >> "$LOG" 2>&1 || [ $? = 1 ] || die "system filesystem check failed"
    resize2fs "$L" >> "$LOG" 2>&1 || die "resize2fs failed"
    mount -t ext4 -o rw "$L" "$MNT" || die "cannot mount the new system"
    V=$MNT/system/vendor
    [ -f "$V/etc/superman-native-components.sha256" ] || die "new system has no vendor manifest"
    cat fstab.superman.internal > "$V/etc/fstab.superman"
    cat fstab.superman.late.internal > "$V/etc/fstab.superman.late"
    echo "$INTERNAL_BOOT kms" > "$V/etc/superman-native-boot.contract"
    read -r b0 f0 c0 n0 < "$V/etc/superman/audio-boot.contract"
    [ "$b0" = $RELEASE_BOOT ] || die "unexpected audio contract"
    echo "$INTERNAL_BOOT $INTERNAL_FDT $c0 $n0" > "$V/etc/superman/audio-boot.contract"
    cat superman-bootmenu > "$V/bin/superman-bootmenu"
    m=$V/etc/superman-native-components.sha256
    : > "$HERE/manifest.new"
    while read -r h p; do
        case "$p" in
            /vendor/etc/superman-native-boot.contract|/vendor/etc/superman/audio-boot.contract)
                h=$(sha256sum "$MNT/system$p" | cut -d' ' -f1) ;;
        esac
        echo "$h  $p" >> "$HERE/manifest.new"
    done < "$m"
    cat "$HERE/manifest.new" > "$m"
    # verify every manifest line inside the new system
    while read -r h p; do
        case "$p" in /vendor/*) f=$MNT/system$p ;; *) f=$MNT$p ;; esac
        echo "$h  $f"
    done < "$m" > "$HERE/manifest-mnt.txt"
    sha256sum -c "$HERE/manifest-mnt.txt" > "$HERE/manifest-mnt-check.txt" 2>&1 || die "new system manifest check failed"
    sync; umount -D "$MNT"
    e2fsck -fn "$L" >> "$LOG" 2>&1 || die "new system filesystem check failed"

    say "4/4 data (new, empty ext4)"
    L=$(loop $DATA_FIRST $DATA_LAST)
    mke2fs -F -q -t ext4 -b 4096 -I 256 -m 1 -L data -E nodiscard \
        -O none,has_journal,ext_attr,dir_index,filetype,extent,sparse_super,large_file,huge_file,uninit_bg,dir_nlink,extra_isize \
        "$L" >> "$LOG" 2>&1 || die "mke2fs failed"
    e2fsck -fn "$L" >> "$LOG" 2>&1 || die "data filesystem check failed"

    cleanup
    ./superman-gpt status "$DISK" >> "$LOG"
    sync
    say "DONE. Power the phone off, take the SD card out and power it on: Android starts from the internal storage."
    ;;
*)
    echo "usage: sh superman-install-internal.sh check | prepare | install <sha256 of plan/old-primary.bin>" >&2
    exit 64
    ;;
esac
