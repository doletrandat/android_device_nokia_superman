#!/system/bin/sh
# Qualify DSP/CORE before the separate PCM loader starts Android audio.
set -eu
mode=${1:-}
case "$mode" in ''|--check) ;; *) exit 64 ;; esac
stage=profile
loaded=
bound=
override_owned=
channel=
fail() {
    rc=$?
    trap - EXIT
    echo "SUPERMAN_AUDIO_CORE_FAILED stage=$stage" >&2
    # --check must be read-only even when a guard rejects the candidate.
    [ "$mode" != --check ] || exit "$rc"
    echo "SUPERMAN_AUDIO_CORE_FAILED stage=$stage" > /dev/kmsg
    if [ -n "$bound" ]; then
        printf '%s\n' "${channel##*/}" > /sys/bus/rpmsg/drivers/qcom,apr/unbind || true
    fi
    if [ -n "$override_owned" ] && [ -f "$channel/driver_override" ]; then
        printf '\n' > "$channel/driver_override" || true
    fi
    for name in $loaded; do rmmod "$name" || true; done
    setprop vendor.superman.audio_core.state failed || true
}
trap fail EXIT
[ "$(getprop ro.superman.audio_profile)" = pf24-core ]
stage=board
tr '\000' '\n' < /sys/firmware/devicetree/base/compatible | grep -qx nokia,superman
tr '\000' '\n' < /sys/firmware/devicetree/base/compatible | grep -qx qcom,msm8226
stage=boot-contract
# Update this separate contract only after qualifying the new boot/DT/config.
# Bind the acquired running kernel notes, FDT and config as well as the stored image.
read -r boot_hash fdt_hash config_hash notes_hash extra < /vendor/etc/superman/audio-boot.contract
[ -z "$extra" ]
for digest in "$boot_hash" "$fdt_hash" "$config_hash" "$notes_hash"; do
    [ "${#digest}" = 64 ]
    case "$digest" in *[!0-9a-f]*) exit 1 ;; esac
done
stage=stored-boot
[ "$(sha256sum /lumia_sd/boot.img | cut -d ' ' -f 1)" = "$boot_hash" ]
stage=running-fdt
[ "$(sha256sum /sys/firmware/fdt | cut -d ' ' -f 1)" = "$fdt_hash" ]
stage=running-config
[ "$(zcat /proc/config.gz | sha256sum | cut -d ' ' -f 1)" = "$config_hash" ]
stage=running-kernel-notes
[ "$(sha256sum /sys/kernel/notes | cut -d ' ' -f 1)" = "$notes_hash" ]
stage=firmware
[ "$(sha256sum /vendor/firmware/qcom/superman/qcadsp8626.mbn | cut -d ' ' -f 1)" = b8b989a2512c210193c40cb7e674cffafa32497e5ebfa6bcdf828a9400fdf897 ]
stage=module-hashes
(cd / && sha256sum -c /vendor/etc/superman/audio-core.sha256)
command -v timeout >/dev/null
stage=processor
processor=
count=0
for p in /sys/class/remoteproc/remoteproc*; do
    [ "$(cat "$p/firmware")" = qcom/superman/qcadsp8626.mbn ] || continue
    processor=$p
    count=$((count + 1))
done
[ "$count" = 1 ]
stage=module-ownership
for name in qrtr qrtr_smd qmi_helpers qcom_pdr_msg pdr_interface apr q6core core_smoke; do
    [ ! -e "/sys/module/$name" ]
done
stage=processor-ownership
[ "$(cat "$processor/state")" = offline ]
[ ! -e /dev/.superman-audio-core ]
case "${1:-}" in
    --check) trap - EXIT; echo 'Qualified audio CORE preflight passed (no DSP start)'; exit 0 ;;
    '') ;;
    *) exit 64 ;;
esac
# Keep the per-boot lock after failure: restarting init must not retry DSP boot.
mkdir /dev/.superman-audio-core
[ "$(cat "$processor/state")" = offline ]
# Serialize initial WCNSS and ADSP PAS firmware starts.
# Both init services start early; overlapping boots produced WCNSS
# authentication failures while a single later WCNSS start succeeded.
stage=radio-start-order
radio_wait=0
while :; do
    case "$(getprop sys.superman.radio.state)" in
        enumerated|partial|failed) break ;;
        ''|starting) ;;
        *) exit 1 ;;
    esac
    [ "$radio_wait" -lt 15 ]
    radio_wait=$((radio_wait + 1))
    sleep 1
done
echo "SUPERMAN_AUDIO_RADIO_GATE_READY waited=$radio_wait state=$(getprop sys.superman.radio.state)" > /dev/kmsg
stage=dsp
printf 'disabled\n' > "$processor/recovery"
[ "$(cat "$processor/recovery")" = disabled ]
timeout 15 sh -c 'printf "start\n" > "$1/state"' sh "$processor"
[ "$(cat "$processor/state")" = running ]
stage=modules
for name in qrtr qrtr_smd qmi_helpers qcom_pdr_msg pdr_interface apr q6core; do
    if [ "$name" = apr ]; then
        insmod "/vendor/lib/modules/audio-pf31/$name.ko" superman_legacy_core=1 superman_legacy_audio=1
    elif [ "$name" = q6core ]; then
        insmod "/vendor/lib/modules/audio-pf31/$name.ko"
    else
        insmod "/vendor/lib/modules/audio/$name.ko"
    fi
    loaded="$name $loaded"
done
stage=channel
real_processor=$(readlink -f "$processor")
i=0
while [ "$i" -lt 10 ]; do
    count=0
    for p in /sys/bus/rpmsg/devices/*apr_audio_svc*; do
        [ -e "$p" ] || continue
        case "$(readlink -f "$p")" in
            "$real_processor"/*) channel=$p; count=$((count + 1)) ;;
        esac
    done
    [ "$count" -le 1 ]
    [ "$count" = 0 ] || break
    i=$((i + 1)); sleep 1
done
[ "$count" = 1 ]
[ ! -e "$channel/driver" ]
case "$(cat "$channel/driver_override")" in ''|'(null)') ;; *) exit 1 ;; esac
stage=bind
override_owned=1
printf 'qcom,apr\n' > "$channel/driver_override"
printf '%s\n' "${channel##*/}" > /sys/bus/rpmsg/drivers/qcom,apr/bind
bound=1
stage=query
insmod /vendor/lib/modules/audio/core_smoke.ko
loaded="core_smoke $loaded"
rmmod core_smoke
loaded=${loaded#core_smoke }
setprop vendor.superman.audio_core.state ready
echo 'SUPERMAN_AUDIO_CORE_READY codec_pcm_unconfigured=1' > /dev/kmsg
trap - EXIT
