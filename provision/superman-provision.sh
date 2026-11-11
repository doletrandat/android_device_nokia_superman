#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# CC52 per-phone provisioning, run once per boot at post-fs-data (cheap when already done).
#
# Modem EFS seeds: rmtfs serves read-only copies of THIS phone's MODEM_FSG/FS1/FS2/FSC partitions
# (the modem's writes stay in RAM). A shared image must never carry them, so each phone copies its
# own on first boot. The internal eMMC is only ever READ here.
#
#   superman-provision.sh            provision /data/vendor/rmtfs-pf29 (normal boot)
#   superman-provision.sh --test DIR copy the seeds into DIR instead and leave the real state alone
set -eu
PATH=/system/bin:/vendor/bin
export PATH
umask 077

mode=boot
SEEDS=/data/vendor/rmtfs-pf29
STATE=/data/vendor/superman
case "$#:${1:-}" in
    0:) ;;
    2:--test) mode=test; SEEDS=$2/rmtfs-pf29; STATE=$2/superman ;;
    *) exit 64 ;;
esac
CONTRACT=$STATE/modem-seeds.contract
LEGACY=/vendor/etc/superman/modem-seeds.contract   # pre-CC52 phones: hash of the existing seeds

log() { echo "SUPERMAN_PROVISION $*" > /dev/kmsg; }
state() { [ "$mode" = test ] || setprop vendor.superman.provision.modem "$1"; log "modem=$1 $2"; }
fail() { state failed "stage=$1"; exit 1; }

seeds_valid() {
    [ -f "$SEEDS/baselines.sha256" ] && [ ! -L "$SEEDS/baselines.sha256" ] || return 1
    [ "$(wc -l < "$SEEDS/baselines.sha256")" -eq 4 ] || return 1
    for name in modem_fs1 modem_fs2 modem_fsg modem_fsc; do
        [ -f "$SEEDS/$name" ] && [ ! -L "$SEEDS/$name" ] || return 1
        want=$(grep "  /data/vendor/rmtfs-pf29/$name\$" "$SEEDS/baselines.sha256" | cut -d ' ' -f 1)
        [ "${#want}" = 64 ] && [ "$(sha256sum "$SEEDS/$name" | cut -d ' ' -f 1)" = "$want" ] || return 1
    done
}

mkdir -p "$SEEDS" "$STATE"
chmod 0700 "$SEEDS" "$STATE"

# 1. Already provisioned: the contract pins the baseline list, the list pins the four files.
if [ -f "$CONTRACT" ] && seeds_valid; then
    read -r want extra < "$CONTRACT"
    [ -z "$extra" ] && [ "$(sha256sum "$SEEDS/baselines.sha256" | cut -d ' ' -f 1)" = "$want" ] || fail contract_mismatch
    state ready "seeds=present"
    exit 0
fi
[ ! -f "$CONTRACT" ] || fail contract_without_valid_seeds   # never silently replace a phone's seeds

# 2. Migration: seeds made before CC52 are pinned by the old /vendor contract. Adopt them unchanged.
if [ "$mode" = boot ] && [ -f "$LEGACY" ] && seeds_valid; then
    read -r want extra < "$LEGACY"
    if [ -z "$extra" ] && [ "$(sha256sum "$SEEDS/baselines.sha256" | cut -d ' ' -f 1)" = "$want" ]; then
        echo "$want" > "$CONTRACT.tmp"; chmod 0400 "$CONTRACT.tmp"; mv "$CONTRACT.tmp" "$CONTRACT"
        state ready "seeds=migrated"
        exit 0
    fi
fi

# 3. First boot on this phone: copy the four partitions, found by GPT name and checked by size.
for name in modem_fs1 modem_fs2 modem_fsg modem_fsc baselines.sha256; do
    [ ! -e "$SEEDS/$name" ] || fail "partial_seeds_present:$name"   # leave half-made state for a human
done
part() {  # GPT name -> /dev/block node on the internal eMMC
    for u in /sys/block/mmcblk0/mmcblk0p*/uevent; do
        if grep -qx "PARTNAME=$1" "$u"; then echo "/dev/block/$(grep '^DEVNAME=' "$u" | cut -d= -f2)"; return 0; fi
    done
    return 1
}
: > "$SEEDS/.baselines.tmp"
for pair in modem_fsg:MODEM_FSG:3072 modem_fs1:MODEM_FS1:3072 modem_fs2:MODEM_FS2:3072 modem_fsc:MODEM_FSC:32; do
    file=${pair%%:*}; rest=${pair#*:}; gpt=${rest%%:*}; sectors=${rest#*:}
    dev=$(part "$gpt") || fail "missing_partition:$gpt"
    [ "$(cat "/sys/block/mmcblk0/${dev##*/}/size")" = "$sectors" ] || fail "size:$gpt"
    h1=$(sha256sum "$dev" | cut -d ' ' -f 1)
    cat "$dev" > "$SEEDS/.$file.tmp"
    h2=$(sha256sum "$dev" | cut -d ' ' -f 1)
    [ "$h1" = "$h2" ] && [ "$(sha256sum "$SEEDS/.$file.tmp" | cut -d ' ' -f 1)" = "$h1" ] || fail "unstable_read:$gpt"
    chmod 0400 "$SEEDS/.$file.tmp"
    echo "$h1  /data/vendor/rmtfs-pf29/$file" >> "$SEEDS/.baselines.tmp"
done
for file in modem_fsg modem_fs1 modem_fs2 modem_fsc; do mv "$SEEDS/.$file.tmp" "$SEEDS/$file"; done
# The baseline list always names the real paths, which is what superman-modem-start.sh checks.
sort -k2 "$SEEDS/.baselines.tmp" > "$SEEDS/.baselines.sorted"; rm "$SEEDS/.baselines.tmp"
chmod 0400 "$SEEDS/.baselines.sorted"; mv "$SEEDS/.baselines.sorted" "$SEEDS/baselines.sha256"
sha256sum "$SEEDS/baselines.sha256" | cut -d ' ' -f 1 > "$CONTRACT.tmp"
chmod 0400 "$CONTRACT.tmp"; mv "$CONTRACT.tmp" "$CONTRACT"
sync
if [ "$mode" = test ]; then
    (cd "$SEEDS" && for f in modem_fs1 modem_fs2 modem_fsg modem_fsc; do sha256sum "$f"; done) | while read -r h f; do
        grep -q "^$h  /data/vendor/rmtfs-pf29/$f\$" "$SEEDS/baselines.sha256" || fail "test_readback:$f"
    done
else
    seeds_valid || fail readback
fi
state ready "seeds=provisioned"
