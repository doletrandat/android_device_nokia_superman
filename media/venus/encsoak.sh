#!/system/bin/sh
# Encoder soak: 720p encode loop for $1 seconds while soak.sh decodes in parallel.
# Each output must match the first run byte for byte (encoder is deterministic).
cd /data/local/tmp/cc18
sh soak.sh $1 > soak-par.txt 2>&1 &
end=$(( $(cut -d. -f1 /proc/uptime) + $1 )); i=0; bad=0; ref=""
while [ $(cut -d. -f1 /proc/uptime) -lt $end ]; do
  i=$((i+1))
  r=$(./venc /dev/video1 src720.nv12 encsoak.h264 1280 720 4000000 11 | tail -1)
  h=$(sha256sum encsoak.h264 | cut -c1-16)
  [ -z "$ref" ] && ref=$h
  case "$r" in "fed 30 encoded 30"*) [ "$h" = "$ref" ] || { bad=$((bad+1)); echo "DIFF i=$i $h"; } ;; *) bad=$((bad+1)); echo "FAIL i=$i $r";; esac
  [ $((i % 50)) -eq 0 ] && echo "enc i=$i bad=$bad temp=$(cat /sys/class/thermal/thermal_zone0/temp) mem=$(grep MemAvailable /proc/meminfo | tr -s ' ' | cut -d' ' -f2)"
done
wait
echo "ENC DONE iterations=$i bad=$bad ref=$ref"; tail -1 soak-par.txt
