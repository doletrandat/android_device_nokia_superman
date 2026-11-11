#!/system/bin/sh
# ab.sh <libpath> <label> <runs>: install plugin store lib, make Venus default, run gseek3 N times, count fatals.
mount -o remount,rw /; cp $1 /vendor/lib/libc2plugin_store.so; chmod 644 /vendor/lib/libc2plugin_store.so; chcon u:object_r:vendor_file:s0 /vendor/lib/libc2plugin_store.so
cp /data/local/tmp/mcc2-venus.xml /vendor/etc/media_codecs_c2.xml; chmod 644 /vendor/etc/media_codecs_c2.xml; chcon u:object_r:vendor_configs_file:s0 /vendor/etc/media_codecs_c2.xml
sync; mount -o remount,ro /
setprop ctl.restart android-hardware-media-c2-v4l2-hal-1-0; sleep 2; setprop ctl.restart media; sleep 5
svc power stayon true
f=0; s=0
r=0; while [ $r -lt $3 ]; do r=$((r+1))
  dmesg -c > /dev/null; dmesg -w > /data/local/tmp/abk.txt &
  out=$(sh /data/local/tmp/gseek3.sh 2>&1 | tail -1)
  pkill -x dmesg
  n=$(sed -n '/Err_Fatal\|Exception/p' /data/local/tmp/abk.txt | wc -l)
  rs=$(echo "$out" | sed 's/.*restarts=\([0-9]*\).*/\1/'); f=$((f+n)); s=$((s+rs))
  echo "$2 run $r: $out fatal=$n"
done
echo "$2 TOTAL seeks~$s fatals=$f"
