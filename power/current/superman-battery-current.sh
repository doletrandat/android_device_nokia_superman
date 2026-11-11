#!/system/bin/sh
# Measured current and calibrated BMS capacity. Keep charger policy and suspend guard unchanged.
set -eu
mode=${1:-}
[ "$#" -le 1 ] || exit 64
case "$mode" in ''|--check) ;; *) exit 64 ;; esac
stage=board
provider_owned=
bridge_owned=
gauge_owned=
mutating=
lock=/dev/.superman-battery-current
sample=/sys/class/power_supply/smbb-bif/current_now
fail() {
    rc=$?
    trap - EXIT
    echo "SUPERMAN_BATTERY_CURRENT_FAILED stage=$stage" >&2
    [ -n "$mutating" ] || exit "$rc"
    # Never unload a dependency if its consumer could not be removed.
    if [ -n "$gauge_owned" ]; then
        if rmmod superman_bms_gauge; then gauge_owned=; else
            setprop vendor.superman.battery_current.state failed || true
            exit "$rc"
        fi
    fi
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
# CC52: no SD-card CID pin.
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
read_capacity() {
    value=$(cat /sys/class/power_supply/smbb-bif/capacity)
    case "$value" in ''|*[!0-9]*) return 1 ;; esac
    [ "$value" -le 100 ]
    [ "$(cat /sys/module/superman_bms_gauge/parameters/last_error)" = 0 ]
    [ "$(cat /sys/module/superman_bms_gauge/parameters/latch_retained)" = N ]
}
stage=ownership
if [ -e "$lock" ]; then
    [ "$(getprop vendor.superman.battery_current.state)" = ready ]
    [ -d /sys/module/qcom_spmi_iadc/holders/superman_iadc_current ]
    [ -d /sys/module/qcom_spmi_iadc/holders/superman_bms_gauge ]
    read_current
    read_capacity
    trap - EXIT
    echo 'SUPERMAN_BATTERY_CURRENT_ALREADY_READY'
    exit 0
fi
[ ! -e /sys/module/qcom_spmi_iadc ]
[ ! -e /sys/module/superman_iadc_current ]
[ ! -e /sys/module/superman_bms_gauge ]
[ ! -e /sys/class/power_supply/smbb-bif/capacity ]
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
# During early boot the IADC offset calibration can time out (-110) while the
# modem and other PMIC users start; the same probe succeeds minutes later.
# Reprobe the device for up to 60 s instead of failing the whole boot.
iadc=fc4cf000.spmi:pm8226@0:adc@3600
iadc_driver=/sys/bus/platform/drivers/qcom-spmi-iadc
count=0
while [ ! -e "$iadc_driver/$iadc" ]; do
    [ "$count" -lt 20 ]
    count=$((count + 1)); sleep 3
    echo "$iadc" > "$iadc_driver/bind" 2>/dev/null || true
done
[ "$count" = 0 ] || echo "SUPERMAN_BATTERY_CURRENT iadc_reprobes=$count" > /dev/kmsg
stage=current-bridge
# The first conversions after the IADC binds can still time out, which fails
# the bridge's probe-time reading; retry the load like the provider reprobe.
count=0
until insmod /vendor/lib/modules/battery-current/superman-iadc-current.ko; do
    [ "$count" -lt 10 ]
    count=$((count + 1)); sleep 3
done
[ "$count" = 0 ] || echo "SUPERMAN_BATTERY_CURRENT bridge_retries=$count" > /dev/kmsg
bridge_owned=1
[ -d /sys/module/qcom_spmi_iadc/holders/superman_iadc_current ]
stage=measured-current
count=0
until read_current; do
    [ "$count" -lt 10 ]
    count=$((count + 1)); sleep 1
done
stage=bms-capacity
count=0
until insmod /vendor/lib/modules/battery-current/superman-bms-gauge.ko; do
    [ "$count" -lt 10 ]
    count=$((count + 1)); sleep 3
done
[ "$count" = 0 ] || echo "SUPERMAN_BATTERY_CURRENT gauge_retries=$count" > /dev/kmsg
gauge_owned=1
[ -d /sys/module/qcom_spmi_iadc/holders/superman_bms_gauge ]
read_capacity
setprop vendor.superman.battery_current.state ready
echo 'SUPERMAN_BATTERY_CURRENT_READY measured_microamps=1 BMS_capacity=1 nominal_FCC=2220mAh no_charge_policy=1' > /dev/kmsg
trap - EXIT
