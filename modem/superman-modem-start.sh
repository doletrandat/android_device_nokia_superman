#!/system/bin/sh
# One modem attempt per boot. Filesystem writes stay in RAM; RF stays under RIL control.
set -eu
mode=${1:-}
case "$mode" in ''|--check) ;; *) exit 64 ;; esac
stage=boot-contract
read -r boot_hash fdt_hash config_hash notes_hash extra < /vendor/etc/superman/audio-boot.contract
[ -z "$extra" ]
for digest in "$boot_hash" "$fdt_hash" "$config_hash" "$notes_hash"; do
    [ "${#digest}" = 64 ]
    case "$digest" in *[!0-9a-f]*) exit 1 ;; esac
done
[ "$(getprop sys.boot_completed)" = 1 ]
[ "$(getprop vendor.superman.audio_core.state)" = ready ]
# CC52: no SD-card CID or fixed mmcblk1 node pins (other phones, other cards, later the eMMC).
[ "$(sha256sum /lumia_sd/boot.img | cut -d ' ' -f1)" = "$boot_hash" ]
[ "$(sha256sum /sys/firmware/fdt | cut -d ' ' -f1)" = "$fdt_hash" ]
[ "$(zcat /proc/config.gz | sha256sum | cut -d ' ' -f1)" = "$config_hash" ]
[ "$(sha256sum /sys/kernel/notes | cut -d ' ' -f1)" = "$notes_hash" ]
(cd / && sha256sum -c /vendor/etc/superman-native-components.sha256)
(cd / && sha256sum -c /vendor/etc/superman/modem-files.sha256)
stage=shared-memory
[ "$(cat /sys/class/rmtfs/qcom_rmtfs_mem1/phys_addr)" = 0x0fd80000 ]
[ "$(cat /sys/class/rmtfs/qcom_rmtfs_mem1/size)" = 0x00180000 ]
[ -f /sys/firmware/devicetree/base/reserved-memory/rmtfs@fd80000/no-map ]
stage=baselines
# CC52: the seeds are this phone's own MODEM_FS* copies, made by superman-provision.sh; the
# contract that pins them lives on /data next to them, not in the shared image.
[ "$(getprop vendor.superman.provision.modem)" = ready ]
read -r seed_hash extra < /data/vendor/superman/modem-seeds.contract
[ -z "$extra" ] && [ "${#seed_hash}" = 64 ]
[ "$(sha256sum /data/vendor/rmtfs-pf29/baselines.sha256 | cut -d ' ' -f1)" = "$seed_hash" ]
(cd / && sha256sum -c /data/vendor/rmtfs-pf29/baselines.sha256)
for name in modem_fs1 modem_fs2 modem_fsg modem_fsc; do
    test -f "/data/vendor/rmtfs-pf29/$name"
    test ! -L "/data/vendor/rmtfs-pf29/$name"
done
stage=ownership
daemon_binary=$(readlink -f /vendor/bin/superman-rmtfs)
[ -n "$daemon_binary" ]
[ ! -e /dev/.superman-modem ]
[ ! -e /sys/module/qcom_q6v5_mss ]
[ -z "$(pidof superman-rmtfs)" ]
if [ "$mode" = --check ]; then
    echo 'Qualified modem preflight passed (no modem or filesystem start)'
    exit 0
fi
mkdir /dev/.superman-modem
exec > /data/vendor/rmtfs-pf29/boot-service.log 2>&1
printf 'Modem boot %s uptime %s\n' "$(cat /proc/sys/kernel/random/boot_id)" "$(cat /proc/uptime)"
setprop vendor.superman.modem.state starting
rproc=
daemon=
module_owned=
data_module_owned=
stop_daemon() {
    [ -n "$daemon" ] || return 0
    if [ "$(readlink "/proc/$daemon/exe")" = "$daemon_binary" ]; then
        kill "$daemon"
        wait "$daemon" || true
    elif kill -0 "$daemon" 2>/dev/null; then
        echo 'Daemon identity changed; refusing to signal or wait indefinitely'
        daemon=
        return 1
    else
        wait "$daemon" || true
    fi
    daemon=
}
cleanup() {
    rc=$?
    trap - EXIT
    trap '' TERM INT
    set +e
    echo "Modem service exit rc=$rc stage=$stage"
    # A crashed remoteproc cannot be stopped in this kernel. Remove RMTFS first,
    # recover once solely for orderly stop, and never retry operational startup.
    if [ -n "$rproc" ] && [ "$(cat "$rproc/state")" = crashed ]; then
        stop_daemon || rc=1
        timeout 20 sh -c 'echo recover > "$1/recovery"' sh "$rproc"
    fi
    if [ -n "$data_module_owned" ]; then
        # Release DMA while the modem can still acknowledge the power handshake.
        if timeout 15 rmmod qcom_bam_dmux; then
            data_module_owned=
            setprop vendor.superman.data_transport.state stopped
        else
            echo 'Data driver cleanup incomplete; retaining modem and RMTFS'
            setprop vendor.superman.data_transport.state failed
            setprop vendor.superman.modem.state failed
            while :; do sleep 5; done
        fi
    fi
    if [ -n "$rproc" ] && [ "$(cat "$rproc/state")" = running ]; then
        timeout 15 sh -c 'echo stop > "$1/state"' sh "$rproc"
    fi
    stop_daemon || rc=1
    if [ -n "$module_owned" ] && [ -n "$rproc" ] && [ "$(cat "$rproc/state")" = offline ]; then
        rmmod qcom_q6v5_mss
    fi
    if [ -e /sys/module/qcom_q6v5_mss ]; then
        echo 'Modem cleanup incomplete; module retained for supported recovery'
        rc=1
    fi
    if [ "$rc" = 0 ]; then setprop vendor.superman.modem.state stopped
    else setprop vendor.superman.modem.state failed; fi
    exit "$rc"
}
trap cleanup EXIT
trap 'stage=terminated; exit 0' TERM INT
stage=filesystem
/vendor/bin/superman-rmtfs -r -v -o /data/vendor/rmtfs-pf29 > /data/vendor/rmtfs-pf29/boot-rmtfs.log 2>&1 &
daemon=$!
printf '%s\n' "$daemon" > /dev/.superman-modem/rmtfs.pid
# rmtfs needs a moment to register its QRTR service (14); probing once raced it on the eMMC install (CC59).
tries=0
while :; do
    kill -0 "$daemon"
    /vendor/bin/superman-qrtr-probe > /dev/.superman-modem/services-before
    grep -q '"service":14,' /dev/.superman-modem/services-before && break
    tries=$((tries + 1))
    [ "$tries" -lt 30 ]
    sleep 0.5
done
stage=driver
insmod /vendor/lib/modules/modem/qcom_q6v5_mss.ko
module_owned=1
for p in /sys/class/remoteproc/remoteproc*; do
    [ "$(cat "$p/firmware")" = qcom/superman/mba-pf29.b00 ] || continue
    [ -z "$rproc" ]
    rproc=$p
done
[ -n "$rproc" ]
[ "$(cat "$rproc/state")" = offline ]
echo disabled > "$rproc/recovery"
[ "$(cat "$rproc/recovery")" = disabled ]
stage=modem-start
timeout 45 sh -c 'echo start > "$1/state"' sh "$rproc"
[ "$(cat "$rproc/state")" = running ]
stage=data-transport
setprop vendor.superman.data_transport.state unavailable
if [ ! -e /sys/module/qcom_bam_dmux ] && insmod /vendor/lib/modules/modem/qcom_bam_dmux.ko; then
    data_module_owned=1
    for i in 1 2 3 4 5; do
        [ -e /sys/class/net/wwan0/dev_port ] && break
        sleep 1
    done
    if [ "$(readlink -f /sys/bus/platform/devices/fc880000.remoteproc:bam-dmux/driver)" = /sys/bus/platform/drivers/bam-dmux ] &&
       [ "$(cat /sys/class/net/wwan0/dev_port 2>/dev/null)" = 0 ] &&
       [ "$(cat /sys/class/net/wwan0/type 2>/dev/null)" = 519 ]; then
        setprop vendor.superman.data_transport.state ready
    else
        echo 'Data driver loaded but the expected raw IP channel is unavailable'
        setprop vendor.superman.data_transport.state failed
    fi
else
    echo 'Data driver unavailable; retaining the independent QMUX radio service'
fi
stage=control-transport
/vendor/bin/superman-qmux-probe > /data/vendor/rmtfs-pf29/boot-qmi.log 2>&1
stage=monitor
setprop vendor.superman.modem.state transport_ready
while :; do
    sleep 3
    stage=filesystem-liveness
    kill -0 "$daemon"
    [ "$(readlink "/proc/$daemon/exe")" = "$daemon_binary" ]
    stage=modem-liveness
    [ "$(cat "$rproc/state")" = running ]
done
