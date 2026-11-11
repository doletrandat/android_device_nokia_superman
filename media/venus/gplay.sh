#!/system/bin/sh
# Full Gallery play with fps sampling. Prints one line per 20 s window.
L="SurfaceView - com.android.gallery3d/com.android.gallery3d.app.MovieActivity#0"
input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard
am force-stop com.android.gallery3d
logcat -c; dmesg -c > /dev/null
am start -a android.intent.action.VIEW -d file:///sdcard/Movies/mck_test_1080p.mp4 -t video/mp4 > /dev/null 2>&1
sleep 3
n=0
while [ $n -lt 15 ]; do
  n=$((n+1))
  dumpsys SurfaceFlinger --latency-clear > /dev/null
  sleep 5
  dumpsys SurfaceFlinger --latency "$L" > /data/local/tmp/lat-$n.txt
  top -b -n 1 -m 12 | sed -n '/mediaserver\|v4l2\|surfaceflinger\|audioserver/p' | awk '{print $9"% "$NF}' | tr '\n' ' ' > /data/local/tmp/cpu-$n.txt
  echo "w=$n t=$((n*20)) temp=$(cat /sys/class/thermal/thermal_zone1/temp) act=$(dumpsys activity activities | sed -n 's/.*mResumedActivity.*\(gallery3d[^ ]*\).*/\1/p' | head -1)"
  sleep 15
done
