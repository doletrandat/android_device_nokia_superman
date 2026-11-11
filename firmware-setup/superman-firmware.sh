#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# CC53 first-boot firmware setup. Runs blocking (exec_start) at post-fs-data, before anything loads
# phone firmware. The shared image ships WITHOUT proprietary firmware; each phone builds its own from:
#   - Windows Phone MainOS (NTFS, read-only): Windows/system32/qc*.mbn -> ADSP, Venus, WCNSS, MBA, MPSS
#   - DPP (FAT, read-only): /QCOM/WLAN_CLPC.PROVISION (this phone's Wi-Fi calibration)
# MBA and MPSS are decrypted by this phone's TrustZone (pf29 helper modules). Every input and output must
# match /vendor/etc/superman/firmware-allowlist.txt; anything else leaves the phone in a clear
# "unsupported firmware" state and logs the hashes so that version can be added.
# The internal eMMC is only ever READ (block devices set read-only before mounting).
#
#   superman-firmware.sh              normal boot: verify the store and bind it, or set it up once
#   superman-firmware.sh --test DIR   build into DIR/store and stop (no bind mount, no state property)
set -eu
PATH=/system/bin:/vendor/bin
export PATH
umask 077

mode=boot; ROOT=/data/vendor/firmware
case "$#:${1:-}" in
    0:) ;;
    2:--test) mode=test; ROOT=$2 ;;
    *) exit 64 ;;
esac
STORE=$ROOT/store/qcom/superman          # final firmware, bound over /vendor/firmware/qcom/superman
STAGE=$ROOT/stage/qcom/superman          # encrypted inputs, bound there only while decrypting
RECORD=$ROOT/store/FIRMWARE.sha256       # written last: the store is complete
TARGET=$(readlink -f /vendor)/firmware/qcom/superman   # real path: /vendor is a symlink, /proc/mounts uses the target
ALLOW=/vendor/etc/superman/firmware-allowlist.txt
MODS=/vendor/lib/modules/firmware-setup
MNT=/mnt/vendor/superman-fw
if [ "$mode" = test ]; then   # a test run may bring its own allowlist/modules before they are installed
    [ -f "$ROOT/firmware-allowlist.txt" ] && ALLOW=$ROOT/firmware-allowlist.txt
    [ -d "$ROOT/modules" ] && MODS=$ROOT/modules
fi

say() { echo "SUPERMAN_FIRMWARE $*" > /dev/kmsg; }   # visible on the boot console (loglevel 7)
state() { [ "$mode" = test ] || setprop vendor.superman.firmware "$1"; say "state=$1 $2"; }
allowed() { grep -q "^$1 $2 " "$ALLOW"; }             # sha256, name
cleanup() {
    rc=$?
    set +e
    umount "$TARGET/qcdsp28626.mbn" 2>/dev/null; umount "$TARGET" 2>/dev/null   # only our temporary stage binds
    for m in pf29_mpss pf29_mba; do [ -e "/sys/module/$m" ] && rmmod "$m"; done
    umount "$MNT/mainos" 2>/dev/null; umount "$MNT/dpp" 2>/dev/null; umount "$MNT/dpp-addr" 2>/dev/null
    rm -rf "$ROOT/stage" "$ROOT/.sources"
    [ "$rc" = 0 ] || state failed "stage=${stage:-?}"
    exit "$rc"
}

store_valid() {
    [ -f "$RECORD" ] || return 1
    want=$(grep -c ' output$' "$ALLOW")
    [ "$(wc -l < "$RECORD")" = "$want" ] || return 1
    while read -r h name; do
        allowed "$h" "$name" || return 1
        [ -f "$STORE/$name" ] && [ "$(sha256sum "$STORE/$name" | cut -d ' ' -f 1)" = "$h" ] || return 1
    done < "$RECORD"
    read -r h name < "$ROOT/store/CALIBRATION.sha256" || return 1
    [ "$name" = WLAN_CLPC.PROVISION ] && [ "$(sha256sum "$STORE/$name" | cut -d ' ' -f 1)" = "$h" ]
}
bind_store() {
    [ "$mode" = test ] && return 0
    grep -q " $TARGET " /proc/mounts && return 0
    # toybox cannot make a bind mount read-only here (ro is ignored, remount gives EBUSY). The store files
    # are root-owned 0444 and every boot re-verifies them against the allowlist before binding.
    mount -o bind "$STORE" "$TARGET"
}

part() {   # GPT name -> /dev/block node on the internal eMMC
    for u in /sys/block/mmcblk0/mmcblk0p*/uevent; do
        if grep -qx "PARTNAME=$1" "$u"; then echo "/dev/block/$(grep '^DEVNAME=' "$u" | cut -d= -f2)"; return 0; fi
    done
    return 1
}

# CC54: this phone's Wi-Fi/BT addresses (DPP /QCOM/WLAN.PROVISION + BT.PROVISION) go into the live device
# tree before superman-radio-start loads wcn36xx/btqcomsmd, so the shared DTB carries none. Saved once next
# to the firmware store, re-checked every boot. Never blocks the firmware; reported separately.
ADDR=$ROOT/store/ADDRESSES
addresses() {   # explicit "|| return 1": set -e does not apply inside the ( ) || below
    if [ ! -f "$ADDR" ]; then
        d=$(part DPP) || return 1
        blockdev --setro "$d" || return 1
        mkdir -p "$MNT/dpp-addr" && mount -t vfat -o ro "$d" "$MNT/dpp-addr" || return 1
        w=$(od -An -tx1 -v "$MNT/dpp-addr/QCOM/WLAN.PROVISION" | tr -d ' \n')
        b=$(od -An -tx1 -v "$MNT/dpp-addr/QCOM/BT.PROVISION" | tr -d ' \n')
        umount "$MNT/dpp-addr" || return 1
        # WLAN.PROVISION: 01 19 04 + four MACs (wlan0 uses the first); BT.PROVISION: 01 06 + BD address
        [ "${#w}" = 54 ] && [ "${w%"${w#??????}"}" = 011904 ] || return 1
        [ "${#b}" = 16 ] && [ "${b%"${b#????}"}" = 0106 ] || return 1
        w=${w#??????}; w=${w%"${w#????????????}"}
        b=${b#????}
        case "$w$b" in *[!0-9a-f]*) return 1;; esac
        echo "wlan=$w bt=$b" > "$ADDR.tmp" && chmod 0444 "$ADDR.tmp" || return 1
        sha256sum "$ADDR.tmp" | cut -d ' ' -f 1 > "$ADDR.sha256" || return 1
        mv "$ADDR.tmp" "$ADDR" || return 1
    fi
    [ "$(sha256sum "$ADDR" | cut -d ' ' -f 1)" = "$(cat "$ADDR.sha256")" ] || return 1
    read -r wl bl extra < "$ADDR" || return 1
    [ -z "$extra" ] || return 1
    [ "$mode" = test ] && { say "addresses $wl $bl"; return 0; }
    [ -e /sys/module/superman_dpp_addr ] || insmod "$MODS/superman-dpp-addr.ko" "$wl" "$bl" || return 1
    r="wlan=$(cat /sys/module/superman_dpp_addr/parameters/wlan_result) bt=$(cat /sys/module/superman_dpp_addr/parameters/bt_result)"
    setprop vendor.superman.addresses "$r"
    say "addresses $r"
    case "$r" in wlan=replaced\ bt=replaced|wlan=added\ bt=added|wlan=present\ bt=present) return 0;; esac
    return 1
}
run_addresses() { ( addresses ) || { [ "$mode" = test ] || setprop vendor.superman.addresses failed; say "addresses failed"; }; }

# ---- normal boots: verify and bind --------------------------------------------------------------------
if store_valid; then
    bind_store
    state ready "store=present"
    run_addresses
    exit 0
fi
[ ! -f "$RECORD" ] || { state failed "stage=store_mismatch (record present, files differ; not rebuilt automatically)"; exit 1; }

# ---- first boot on this phone --------------------------------------------------------------------------
trap cleanup EXIT
say "Setting up this phone's firmware (first boot only, about a minute). Do not power off."
stage=partitions
MAINOS=$(part MainOS) || { state unsupported "no MainOS partition (Windows Phone already removed?)"; exit 1; }
DPP=$(part DPP) || { state unsupported "no DPP partition"; exit 1; }
blockdev --setro "$MAINOS"; blockdev --setro "$DPP"
mkdir -p "$MNT/mainos" "$MNT/dpp"
stage=ntfs
[ -e /sys/module/ntfs3 ] || insmod "$MODS/ntfs3.ko"
mount -t ntfs3 -o ro,noatime "$MAINOS" "$MNT/mainos"
mount -t vfat -o ro "$DPP" "$MNT/dpp"

rm -rf "$ROOT/stage" "$ROOT/store"
mkdir -p "$STORE"
stage=collect
say "1/4 Reading firmware from Windows Phone (read-only)"
found=0
: > "$ROOT/.sources"
# Inputs are found by name in system32 (or the driver store), then must match the allowlist as inputs.
for name in qcadsp8626.mbn qcdsp1v28626.mbn qcdsp28626.mbn qcvss8626.mbn qcwcnss8626.mbn; do
    src=
    for f in "$MNT"/mainos/Windows/system32/"$name" "$MNT"/mainos/Windows/System32/"$name" \
             "$MNT"/mainos/Windows/system32/DriverStore/FileRepository/*/"$name"; do
        [ -f "$f" ] || continue
        h=$(sha256sum "$f" | cut -d ' ' -f 1)
        if grep -q "^$h $name input\$" "$ALLOW"; then src=$f; break; fi
        say "unrecognised $name sha256=$h at ${f#$MNT/mainos}"
    done
    [ -n "$src" ] || { state unsupported "firmware version not supported yet: $name (hashes logged above)"; exit 1; }
    echo "$name $src" >> "$ROOT/.sources"
    found=$((found + 1))
done
src() { grep "^$1 " "$ROOT/.sources" | cut -d ' ' -f 2-; }
f=$MNT/dpp/QCOM/WLAN_CLPC.PROVISION
[ -f "$f" ] && [ "$(stat -c %s "$f")" = 29816 ] || { state unsupported "no Wi-Fi calibration in DPP"; exit 1; }
cp "$f" "$STORE/WLAN_CLPC.PROVISION"      # per-phone data: not in the allowlist, recorded by hash below

stage=copy
say "2/4 Copying ADSP, Venus and Wi-Fi firmware"
for name in qcadsp8626.mbn qcvss8626.mbn qcdsp1v28626.mbn; do cp "$(src "$name")" "$STORE/$name"; done
# WCNSS: standard PIL split. .mdt = ELF header + program headers (+ segment 1 = hash); .bNN = segment NN.
w=$(src qcwcnss8626.mbn)
u32() { od -An -tu4 -j "$2" -N 4 "$1" | tr -d ' '; }
u16() { od -An -tu2 -j "$2" -N 2 "$1" | tr -d ' '; }
phoff=$(u32 "$w" 28); phnum=$(u16 "$w" 44)
hdr=$((phoff + 32 * phnum))
seg_off() { u32 "$w" $((phoff + 32 * $1 + 4)); }
seg_len() { u32 "$w" $((phoff + 32 * $1 + 16)); }
{ head -c "$hdr" "$w"; tail -c +$(($(seg_off 1) + 1)) "$w" | head -c "$(seg_len 1)"; } > "$STORE/wcnss.mdt"
i=2
while [ "$i" -lt "$phnum" ]; do
    len=$(seg_len "$i")
    if [ "$len" -gt 0 ]; then
        tail -c +$(($(seg_off "$i") + 1)) "$w" | head -c "$len" > "$STORE/$(printf 'wcnss.b%02d' "$i")"
    fi
    i=$((i + 1))
done

stage=decrypt
say "3/4 Decrypting the modem firmware with this phone's TrustZone"
# The helpers read qcom/superman/<name> through the firmware loader. The stage is hard links to the store
# (no extra space) plus the encrypted MPSS bound file-on-file straight from MainOS. While bound,
# everything except the two modem images matches the final store (the modem only starts after boot).
mkdir -p "$STAGE"
for f in "$STORE"/*; do ln "$f" "$STAGE/${f##*/}"; done
: > "$STAGE/qcdsp28626.mbn"
mount -o bind,ro "$STAGE" "$TARGET"
mount -o bind,ro "$(src qcdsp28626.mbn)" "$TARGET/qcdsp28626.mbn"
for m in mba mpss; do [ ! -e "/sys/module/pf29_$m" ]; done
insmod "$MODS/pf29-mba.ko" decrypt=1
[ "$(cat /sys/module/pf29_mba/parameters/result)" = 0 ] || { state unsupported "MBA decrypt rejected ($(cat /sys/module/pf29_mba/parameters/result))"; exit 1; }
cat /dev/pf29-mba > "$STORE/mba-pf29.b00"
rmmod pf29_mba
insmod "$MODS/pf29-mpss.ko" decrypt=1
[ "$(cat /sys/module/pf29_mpss/parameters/result)" = 0 ] || { state unsupported "MPSS decrypt rejected ($(cat /sys/module/pf29_mpss/parameters/result))"; exit 1; }
cat /dev/pf29-mpss > "$STORE/qcdsp28626.mbn"
rmmod pf29_mpss
umount "$TARGET/qcdsp28626.mbn"; umount "$TARGET"
rm -rf "$ROOT/stage"

stage=verify
say "4/4 Checking the result"
: > "$ROOT/store/.record"
for f in "$STORE"/*; do
    name=${f##*/}
    h=$(sha256sum "$f" | cut -d ' ' -f 1)
    if [ "$name" = WLAN_CLPC.PROVISION ]; then
        echo "$h $name" >> "$ROOT/store/.calibration"
        continue
    fi
    allowed "$h" "$name" || { state failed "stage=verify output $name sha256=$h not in allowlist"; exit 1; }
    grep -q "^$h $name output" "$ALLOW" || { state failed "stage=verify $name is not an output"; exit 1; }
    echo "$h $name" >> "$ROOT/store/.record"
done
[ "$(wc -l < "$ROOT/store/.record")" = "$(grep -c ' output$' "$ALLOW")" ] || { state failed "stage=verify missing outputs"; exit 1; }
chmod 0444 "$STORE"/*; chmod 0755 "$ROOT/store" "$ROOT/store/qcom" "$STORE"
mv "$ROOT/store/.calibration" "$ROOT/store/CALIBRATION.sha256"
sync
mv "$ROOT/store/.record" "$RECORD"
sync
trap - EXIT
umount "$MNT/mainos"; umount "$MNT/dpp"; rm -rf "$ROOT/stage" "$ROOT/.sources"
bind_store
state ready "store=built inputs=$found"
say "Firmware set up. Continuing boot."
run_addresses
