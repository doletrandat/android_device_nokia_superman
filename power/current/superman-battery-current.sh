#!/system/bin/sh
# Measured current only. Keep charger policy, SOC and the suspend guard unchanged.
set -eu
mode=${1:-}
[ "$#" -le 1 ] || exit 64
case "$mode" in ''|--check) ;; *) exit 64 ;; esac
stage=board
provider_owned=
bridge_owned=
mutating=
lock=/dev/.superman-battery-current
sample=/sys/class/power_supply/smbb-bif/current_now
fail() {
    rc=$?
    trap - EXIT
    echo "SUPERMAN_BATTERY_CURRENT_FAILED stage=$stage" >&2
    [ -n "$mutating" ] || exit "$rc"
    # Never unload a dependency if its consumer could not be removed.
    if [ -n "$bridge_owned" ]; then
        if rmmod superman_iadc_current; then bridge_owned=; else
            setprop vendor.superman.battery_current.state failed || true
            exit "$rc"
        fi
    fi
    if [ -n "$provider_owned" ]; then rmmod qcom_spmi_iadc || true; fi
    setprop vendor.superman.battery_current.state failed || true
    exit "$rc"
}
trap fail EXIT
tr '\000' '\n' < /sys/firmware/devicetree/base/compatible | grep -qx nokia,superman
tr '\000' '\n' < /sys/firmware/devicetree/base/compatible | grep -qx qcom,msm8226
[ "$(cat /sys/block/mmcblk1/device/cid)" = ad4c534d53534430610000370a719200 ]
stage=boot-contract
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
stage=file-hashes
(cd / && sha256sum -c /vendor/etc/superman/battery-current.sha256)
read_current() {
    value=$(cat "$sample")
    number=${value#-}
    case "$number" in ''|*[!0-9]*) return 1 ;; esac
    [ "$number" -le 5000000 ]
}
stage=ownership
if [ -e "$lock" ]; then
    [ "$(getprop vendor.superman.battery_current.state)" = ready ]
    [ -d /sys/module/qcom_spmi_iadc/holders/superman_iadc_current ]
    read_current
    trap - EXIT
    echo 'SUPERMAN_BATTERY_CURRENT_ALREADY_READY'
    exit 0
fi
[ ! -e /sys/module/qcom_spmi_iadc ]
[ ! -e /sys/module/superman_iadc_current ]
[ ! -e "$sample" ]
if [ "$mode" = --check ]; then
    trap - EXIT
    echo 'SUPERMAN_BATTERY_CURRENT_CHECK_OK no_load=1'
    exit 0
fi
# Leave the per-boot lock on failure; init must not retry failed PMIC probes.
mkdir "$lock"
mutating=1
setprop vendor.superman.battery_current.state starting
stage=battery-prerequisite
count=0
while [ ! -d /sys/class/power_supply/smbb-bif ]; do
    [ "$count" -lt 30 ]
    count=$((count + 1)); sleep 1
done
stage=provider
insmod /vendor/lib/modules/battery-current/qcom-spmi-iadc.ko
provider_owned=1
stage=current-bridge
insmod /vendor/lib/modules/battery-current/superman-iadc-current.ko
bridge_owned=1
[ -d /sys/module/qcom_spmi_iadc/holders/superman_iadc_current ]
stage=measured-current
read_current
setprop vendor.superman.battery_current.state ready
echo 'SUPERMAN_BATTERY_CURRENT_READY measured_microamps=1 no_charge_policy=1 no_SOC=1' > /dev/kmsg
trap - EXIT
