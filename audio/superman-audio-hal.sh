#!/system/bin/sh
# audioserver may explicitly start the HAL before post-fs-data setup finishes.
set -eu
i=0
while :; do
    case "$(getprop vendor.superman.audio_playback.state)" in ready|failed) break ;; esac
    [ "$i" -lt 60 ] || break
    i=$((i + 1)); sleep 1
done
exec /vendor/bin/hw/android.hardware.audio.service
