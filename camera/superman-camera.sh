#!/system/bin/sh
# Load the camera stack (needs a CC44+ DT: camss@fda00000 with data-lanes <0 2> and edge IRQs).
# One insmod at a time; a short pause between them (a combined load once hung the phone).
set -u
M=/vendor/lib/modules/camera
V=/vendor/lib/modules/venus
E=/vendor/etc/superman
st() { setprop vendor.superman.camera.state "$1"; echo "SUPERMAN_CAMERA state=$1 $2" > /dev/kmsg; }

grep -q "^qcom_camss " /proc/modules && { st loaded "already-loaded"; exit 0; }
notes=$(sha256sum /sys/kernel/notes | cut -d' ' -f1)
grep -qx "$notes" $E/camera-kernels.txt || { st skipped "kernel=$notes not allowlisted"; exit 0; }
[ -d /sys/firmware/devicetree/base/soc/camss@fda00000 ] || { st skipped "no camss node in DT"; exit 0; }
(cd / && sha256sum -c $E/camera-files.sha256 >/dev/null 2>&1) || { st failed "file hash mismatch"; exit 1; }

# Let Venus take video0/video1 first (it restarts media after loading).
n=0
while [ $n -lt 60 ]; do
    case "$(getprop vendor.superman.venus.state)" in loaded|skipped|failed) break ;; esac
    sleep 0.5; n=$((n + 1))
done

ld() { grep -q "^$(echo ${1##*/} | tr - _) " /proc/modules && return 0; insmod $1.ko || { st failed "insmod ${1##*/}"; exit 1; }; sleep 0.5; }
for m in videobuf2-common videobuf2-memops videobuf2-v4l2 videobuf2-dma-contig; do ld $V/$m; done
for m in v4l2-cci ccs-pll i2c-qcom-cci ccs qcom-camss; do ld $M/$m; done
# Webcam-style bridge node for the Android external camera provider, fed by superman-camd.
# fps=30 is what the bridge advertises (API1 needs >= 29.97 fps sizes); the real rate is lower.
ld $M/videobuf2-vmalloc
grep -q "^superman_camfeed " /proc/modules || insmod $M/superman_camfeed.ko fps=30 devices=2 width=2816,2560 height=2112,1920 || { st failed "insmod superman_camfeed"; exit 1; }

n=0
while [ $n -lt 50 ]; do
    [ -c /dev/media0 ] && break
    sleep 0.1; n=$((n + 1))
done
[ -c /dev/media0 ] || { st failed "no media device"; exit 1; }
# Short runtime-PM autosuspend for the sensors: superman-camd waits for a power-off between a binned
# session and an unbinned one (a straight switch streams nothing), so keep that wait short.
for d in /sys/bus/i2c/devices/4-0011 /sys/bus/i2c/devices/4-0012; do
    [ -w $d/power/autosuspend_delay_ms ] && echo 100 > $d/power/autosuspend_delay_ms
done
sensor=
for v in /sys/class/video4linux/*; do
    case "$(cat $v/name)" in
        msm_*|smiapp*|ccs*|superman-rear|superman-front) chown system:camera /dev/${v##*/}; chmod 0660 /dev/${v##*/} ;;
    esac
    case "$(cat $v/name)" in smiapp\ scaler*|ccs\ scaler*) sensor=/dev/${v##*/} ;; esac
    [ "$(cat $v/name)" = msm_vfe0_video0 ] && setprop vendor.superman.camera.rdi0 /dev/${v##*/}
done
chown system:camera /dev/media0 /dev/superman-camfeed /dev/superman-camfeed1 && chmod 0660 /dev/media0 /dev/superman-camfeed /dev/superman-camfeed1
[ -n "$sensor" ] || { st failed "rear sensor not bound"; exit 1; }
setprop vendor.superman.camera.sensor "$sensor"
st loaded "sensor=$sensor"
# Rear LED (Windows Phone ACPI FLSH): TLMM 62 = flash driver enable (exported low here), TLMM 32 =
# TORCH_EN, already the DT gpio-leds "white:torch". The camera provider drives both (enable first).
[ -d /sys/class/gpio/gpio574 ] || echo 574 > /sys/class/gpio/export 2>/dev/null
if [ -d /sys/class/gpio/gpio574 ]; then
    echo low > /sys/class/gpio/gpio574/direction
    chown cameraserver:camera /sys/class/gpio/gpio574/value && chmod 0660 /sys/class/gpio/gpio574/value
fi
T=/sys/class/leds/white:torch/brightness
[ -f $T ] && echo 0 > $T && chown cameraserver:camera $T && chmod 0660 $T

if [ -c /dev/superman-camfeed ]; then
    setprop vendor.superman.camera.bridge ready
    # The provider saw video10 while it was still root-only (created before the chown above):
    # restart it so it opens the bridge node with its final permissions.
    if [ "$(getprop init.svc.vendor.camera-provider-2-4-ext)" = running ]; then
        setprop ctl.restart vendor.camera-provider-2-4-ext
        # CameraService records which cameras have a flash only when it enumerates providers, and
        # the provider adds the bridge node a moment after registering: restart cameraserver once
        # the camera exists so the torch (Quick Settings flashlight) is available.
        sleep 4
        setprop ctl.restart cameraserver
    fi
fi

# CC55: Camera2's DisableCameraReceiver runs at BOOT_COMPLETED and permanently hides the app's launcher icon
# when no camera exists yet. Our cameras only appear after boot completes (above), so on a fresh /data the
# icon was hidden for good. Re-enable it once the camera nodes exist (idempotent; ignored if the app is absent).
if [ -c /dev/superman-camfeed ] && pm path com.android.camera2 >/dev/null 2>&1; then
    pm enable com.android.camera2/com.android.camera.CameraLauncher >/dev/null 2>&1 || true
fi
