#!/system/bin/sh
# One owned duplex PCM setup per boot; physical speaker and DMIC2 qualified.
set -eu
stage=ownership
loaded=
fail() {
    rc=$?
    trap - EXIT
    echo "SUPERMAN_AUDIO_PLAYBACK_FAILED stage=$stage" >&2
    echo "SUPERMAN_AUDIO_PLAYBACK_FAILED stage=$stage" > /dev/kmsg
    # Never unload providers if a dependent module refuses normal removal.
    for name in $loaded; do
        sysname=$(echo "$name" | tr '-' '_')
        rmmod "$sysname" || break
    done
    setprop vendor.superman.audio_playback.state failed || true
    exit "$rc"
}
trap fail EXIT
[ "$#" = 0 ]
mkdir /dev/.superman-audio-playback
stage=core-wait
i=0
while [ "$(getprop vendor.superman.audio_core.state)" != ready ]; do
    [ "$(getprop vendor.superman.audio_core.state)" != failed ]
    [ "$i" -lt 45 ]
    i=$((i + 1)); sleep 1
done
stage=boot-contract
tr '\000' '\n' < /sys/firmware/devicetree/base/compatible | grep -qx nokia,superman
tr '\000' '\n' < /sys/firmware/devicetree/base/compatible | grep -qx qcom,msm8226
read -r boot_hash fdt_hash config_hash notes_hash extra < /vendor/etc/superman/audio-boot.contract
[ -z "$extra" ]
for digest in "$boot_hash" "$fdt_hash" "$config_hash" "$notes_hash"; do
    [ "${#digest}" = 64 ]
    case "$digest" in *[!0-9a-f]*) exit 1 ;; esac
done
[ "$(sha256sum /lumia_sd/boot.img | cut -d ' ' -f 1)" = "$boot_hash" ]
[ "$(sha256sum /sys/firmware/fdt | cut -d ' ' -f 1)" = "$fdt_hash" ]
[ "$(zcat /proc/config.gz | sha256sum | cut -d ' ' -f 1)" = "$config_hash" ]
[ "$(sha256sum /sys/kernel/notes | cut -d ' ' -f 1)" = "$notes_hash" ]
stage=provider-hashes
(cd / && sha256sum -c /vendor/etc/superman/audio-playback.sha256)
[ "$(cat /sys/module/apr/parameters/superman_legacy_core)" = Y ]
[ "$(cat /sys/module/apr/parameters/superman_legacy_audio)" = Y ]
stage=module-ownership
modules='slimbus superman_wcd9302_reset superman_slim_pio superman_amp superman_audio_board_power superman_wcd9302 q6dsp-common q6afe q6asm q6adm superman-pcm'
for name in $modules; do
    sysname=$(echo "$name" | tr '-' '_')
    [ ! -e "/sys/module/$sysname" ]
done
stage=modules
for name in $modules; do
    insmod "/vendor/lib/modules/audio-pf31/$name.ko"
    loaded="$name $loaded"
done
stage=pcm
count=0
for card in /proc/asound/card*/id; do
    [ -f "$card" ] || continue
    [ "$(cat "$card")" = SupermanAudio ] || continue
    count=$((count + 1))
    number=${card%/id}; number=${number##*card}
    [ -c "/dev/snd/pcmC${number}D0p" ]
    [ -c "/dev/snd/pcmC${number}D0c" ]
done
[ "$count" = 1 ]
[ "$(cat /sys/module/superman_pcm/parameters/retained)" = N ]
setprop vendor.superman.audio_playback.state ready
echo 'SUPERMAN_AUDIO_PLAYBACK_READY speaker_heard_prior=1 physical_capture_available=1' > /dev/kmsg
trap - EXIT
