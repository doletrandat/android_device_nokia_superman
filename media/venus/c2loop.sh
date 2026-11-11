#!/system/bin/sh
# HAL create/decode/destroy loop with hash check. Usage: c2loop.sh N
cd /data/local/tmp/c2
R240=$(sed -n '/^frame/p' c2-v240.mp4.txt | md5sum | cut -c1-8)
R720=$(sed -n '/^frame/p' c2-v720.mp4.txt | md5sum | cut -c1-8)
P=$(pidof android.hardware.media.c2@1.0-service-v4l2)
bad=0; i=0
while [ $i -lt $1 ]; do
  i=$((i+1))
  if [ $((i % 3)) -eq 0 ]; then v=v720; r=$R720; else v=v240; r=$R240; fi
  timeout 60 ./superman-codec-test $v.mp4 video/ c2.v4l2.avc.decoder > /data/local/tmp/c2/loop.out 2>&1
  h=$(sed -n '/^frame/p' loop.out | md5sum | cut -c1-8)
  [ "$h" = "$r" ] || { bad=$((bad+1)); echo "BAD i=$i $v $(tail -2 loop.out | tr "\n" " ")"; }
  [ $((i % 5)) -eq 0 ] && sleep 3
  [ $((i % 10)) -eq 0 ] && echo "i=$i bad=$bad rss=$(sed -n '/VmRSS/p' /proc/$P/status | tr -s ' ' | cut -d' ' -f2) fds=$(ls /proc/$P/fd | wc -l) pid=$(pidof android.hardware.media.c2@1.0-service-v4l2) temp=$(cat /sys/class/thermal/thermal_zone1/temp)"
done
echo "LOOP DONE n=$i bad=$bad"
