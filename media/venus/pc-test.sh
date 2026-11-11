#!/system/bin/sh
# Venus power-collapse test. $1 = dir with venus-core/dec/enc.ko (matched set),
# $2 = cycles. Each cycle: idle 2 s (expect suspended, clocks off), decode (resume,
# bit-exact), then a back-to-back decode (fast suspend/resume race), then an encode.
cd /data/local/tmp/cc18
M=/vendor/lib/modules/venus; K=${1:-$M}; N=${2:-5}
D=/sys/devices/platform/soc/fdc00000.video-codec
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
clk() { cat /sys/kernel/debug/clk/venus0_vcodec0_clk/clk_enable_count; }
for m in videobuf2-common videobuf2-memops videobuf2-v4l2 videobuf2-dma-contig v4l2-mem2mem; do insmod $M/$m.ko 2>/dev/null; done
grep -q "^venus_core " /proc/modules || { insmod $K/venus-core.ko || exit 1; sleep 2; }
insmod $K/venus-dec.ko 2>/dev/null; insmod $K/venus-enc.ko 2>/dev/null
[ "$(cat /sys/class/video4linux/video0/name)" = qcom-venus-decoder ] || exit 2
echo auto > $D/power/control
ok=0; sus=0
dec() { timeout 30 ./vdec /dev/video0 clip.au - 320 240 | grep ^frame | sort -k2n > pc.txt; cmp -s pc.txt ref240.sorted; }
for i in $(seq 1 $N); do
  sleep 2
  s=$(cat $D/power/runtime_status); c=$(clk); [ "$s" = suspended ] && [ "$c" = 0 ] && sus=$((sus+1))
  if dec && dec && timeout 30 ./venc /dev/video1 src240.nv12 pcenc.h264 320 240 1000000 | grep -q "fed 60 encoded 60"; then
    ok=$((ok+1))
  else echo "cycle $i FAIL (idle was $s clk=$c)"; break; fi
done
sleep 2; echo "end: $(cat $D/power/runtime_status) clk=$(clk) idle-suspended=$sus ok=$ok/$N"
