#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Publish success only after the real configfs bind succeeds.
set -eu
mode=${1:?USB mode required}
stage=validate
gadget=/config/usb_gadget/g1
fail() {
    echo "SUPERMAN_USB_CONFIG_FAILED mode=$mode stage=$stage" > /dev/kmsg
}
trap fail EXIT
[ "$(getprop sys.usb.config)" = "$mode" ] || { trap - EXIT; exit 0; }
case "$mode" in
    none) product=0x4ee7; first=; second=; descriptors=0 ;;
    adb) product=0x4ee7; first=ffs.adb; second=; descriptors=0 ;;
    mtp) product=0x4ee1; first=ffs.mtp; second=; descriptors=1 ;;
    mtp,adb) product=0x4ee2; first=ffs.mtp; second=ffs.adb; descriptors=1 ;;
    ptp) product=0x4ee5; first=ffs.ptp; second=; descriptors=1 ;;
    ptp,adb) product=0x4ee6; first=ffs.ptp; second=ffs.adb; descriptors=1 ;;
    *) exit 1 ;;
esac
# This kernel returns ENODEV when asked to unbind an already unbound gadget.
# Read failures and genuine unbind failures must still stop configuration.
stage=read-binding
bound_controller=$(cat "$gadget/UDC")
# FunctionFS readiness can retrigger this action after binding.
# configfs returns relative symlink targets; resolve them before comparison.
# Unbinding the same configuration closes endpoints and can trigger another
# readiness cycle.
if [ "$mode" != none ] && [ -n "$bound_controller" ] &&
        [ "$bound_controller" = "$(getprop sys.usb.controller)" ] &&
        [ "$(getprop sys.usb.state)" = "$mode" ] &&
        [ "$(readlink -f "$gadget/configs/b.1/f1")" = "$gadget/functions/$first" ]; then
    if { [ -z "$second" ] && [ ! -e "$gadget/configs/b.1/f2" ] &&
            [ ! -L "$gadget/configs/b.1/f2" ]; } ||
            { [ -n "$second" ] &&
            [ "$(readlink -f "$gadget/configs/b.1/f2")" = "$gadget/functions/$second" ]; }; then
        trap - EXIT
        exit 0
    fi
fi
if [ -n "$bound_controller" ]; then
    stage=unbind
    printf '\n' > "$gadget/UDC"
fi
stage=configure
rm -f "$gadget/configs/b.1/f1" "$gadget/configs/b.1/f2"
write_value() { printf '%s\n' "$2" > "$gadget/$1"; }
write_value os_desc/use "$descriptors"
if [ "$mode" != none ]; then
    controller=$(getprop sys.usb.controller)
    case "$controller" in ''|*[!a-zA-Z0-9_.:-]*) exit 1 ;; esac
    [ -d "/sys/class/udc/$controller" ]
    write_value idProduct "$product"
    write_value configs/b.1/strings/0x409/configuration "$mode"
    ln -s "$gadget/functions/$first" "$gadget/configs/b.1/f1"
    if [ -n "$second" ]; then
        ln -s "$gadget/functions/$second" "$gadget/configs/b.1/f2"
    fi
    stage=bind
    write_value UDC "$controller"
    [ "$(cat "$gadget/UDC")" = "$controller" ]
fi
stage=publish
setprop sys.usb.state "$mode"
echo "SUPERMAN_USB_CONFIG_APPLIED mode=$mode" > /dev/kmsg
trap - EXIT
