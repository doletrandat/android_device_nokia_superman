#!/system/bin/sh
L="SurfaceView - com.android.gallery3d/com.android.gallery3d.app.MovieActivity#0"
fr() { dumpsys SurfaceFlinger --latency-clear > /dev/null; sleep 3; dumpsys SurfaceFlinger --latency "$L" | awk 'NR>1 && $2>0 && $2<9000000000000000000 {n++} END {print n+0}'; }
input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard; am force-stop com.android.gallery3d; logcat -c; dmesg -c > /dev/null
am start -a android.intent.action.VIEW -d file:///sdcard/Movies/mck_test_1080p.mp4 -t video/mp4 > /dev/null 2>&1
sleep 6; bad=0; i=0
for x in 400 200 560 130 300 170 450 140 520 250 350 480 210 430 330; do
  i=$((i+1)); input tap 360 640; sleep 0.4; input tap $x 1144; sleep 0.5; input keyevent 126; sleep 1
  n=$(fr); [ "$n" -lt 60 ] && bad=$((bad+1)); echo "seek $i x=$x frames3s=$n"
  sleep 4
done
logcat -d -v time > /data/local/tmp/sl4.txt; dmesg > /data/local/tmp/sk4.txt
echo "SEEKS=$i bad=$bad restarts=$(sed -n '/Decoding => Idle/p' /data/local/tmp/sl4.txt | wc -l) detachfail=$(sed -n '/detachBuffer() failed/p' /data/local/tmp/sl4.txt | wc -l)"
am force-stop com.android.gallery3d
