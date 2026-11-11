#!/system/bin/sh
# Power-collapse soak: random pauses of 0-3 s (around the 2 s autosuspend) between
# 320x240 decodes (hashed) / 720p+1080p timing decodes / 320x240 encodes for $1 seconds.
cd /data/local/tmp/cc18
end=$(( $(cut -d. -f1 /proc/uptime) + $1 )); i=0; bad=0
s0=$(dmesg | grep -c "PCDBG suspended")
while [ $(cut -d. -f1 /proc/uptime) -lt $end ]; do
  i=$((i+1)); r=$(( $(od -An -N2 -tu2 /dev/urandom) % 4 ))
  case $r in
    0) timeout 30 ./vdec /dev/video0 clip.au - 320 240 | grep ^frame | sort -k2n > ps.txt; cmp -s ps.txt ref240.sorted || { bad=$((bad+1)); echo "FAIL dec240 i=$i"; } ;;
    1) timeout 30 ./vdec /dev/video0 clip720.au n 1280 720 | grep -q "fed 60 decoded 60 last" || { bad=$((bad+1)); echo "FAIL dec720 i=$i"; } ;;
    2) timeout 30 ./vdec /dev/video0 clip1080.au n 1920 1080 | grep -q "fed 30 decoded 30 last" || { bad=$((bad+1)); echo "FAIL dec1080 i=$i"; } ;;
    3) timeout 30 ./venc /dev/video1 src240.nv12 ps.h264 320 240 1000000 | grep -q "fed 60 encoded 60" || { bad=$((bad+1)); echo "FAIL enc i=$i"; } ;;
  esac
  ms=$(( $(od -An -N2 -tu2 /dev/urandom) % 3000 )); sleep $((ms/1000)).$(printf %03d $((ms%1000)))
done
echo "PCSOAK DONE ops=$i bad=$bad suspends=$(( $(dmesg | grep -c "PCDBG suspended") - s0 )) warnings=$(dmesg | grep -ciE "WARNING|stuck|failed")"
