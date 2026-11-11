#!/system/bin/sh
# Load the Venus hardware video codec stack (V4L2 /dev/video0 decoder, /dev/video1 encoder).
# The power-collapse Venus modules need a kernel whose Venus SMMU resets the block on
# suspend (CC22+): only load on an allowlisted kernel. Never unloads (an unload latches
# the Venus SMMU failure until reboot).
set -u
M=/vendor/lib/modules/venus
E=/vendor/etc/superman
st() { setprop vendor.superman.venus.state "$1"; echo "SUPERMAN_VENUS state=$1 $2" > /dev/kmsg; }

grep -q "^venus_core " /proc/modules && { st loaded "already-loaded"; exit 0; }
notes=$(sha256sum /sys/kernel/notes | cut -d' ' -f1)
grep -qx "$notes" $E/venus-kernels.txt || { st skipped "kernel=$notes not allowlisted"; exit 0; }
(cd / && sha256sum -c $E/venus-files.sha256 >/dev/null 2>&1) || { st failed "file hash mismatch"; exit 1; }

for m in videobuf2-common videobuf2-memops videobuf2-v4l2 videobuf2-dma-contig v4l2-mem2mem \
         venus-core venus-dec venus-enc; do
    grep -q "^$(echo $m | tr - _) " /proc/modules && continue
    insmod $M/$m.ko || { st failed "insmod $m"; exit 1; }
done
n=0
while [ $n -lt 50 ]; do
    [ -c /dev/video0 ] && [ -c /dev/video1 ] && break
    sleep 0.1; n=$((n + 1))
done
dec= enc=
for v in /sys/class/video4linux/video*; do
    case "$(cat $v/name)" in
        qcom-venus-decoder) dec=/dev/${v##*/} ;;
        qcom-venus-encoder) enc=/dev/${v##*/} ;;
    esac
done
[ -n "$dec" ] && [ -n "$enc" ] || { st failed "no video nodes"; exit 1; }
chown media:media $dec $enc && chmod 0660 $dec $enc || { st failed "node permissions"; exit 1; }
setprop vendor.superman.venus.decoder $dec
setprop vendor.superman.venus.encoder $enc
st loaded "decoder=$dec encoder=$enc"
# The media server built its codec list before Venus existed (this runs at boot_completed):
# the V4L2 encoder's interface probes /dev/video1, so c2.v4l2.avc.encoder was left out and
# could not be created until the next media restart. Rebuild the list now.
setprop ctl.restart media
echo "SUPERMAN_VENUS media restarted for the codec list" > /dev/kmsg
