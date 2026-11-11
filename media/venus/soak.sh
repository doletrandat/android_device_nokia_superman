#!/system/bin/sh
# Venus soak: alternate 720p/1080p timing runs (each = open/stream/close); every 25th
# iteration a 1080p hashed run compared with the reference. Stops after $1 seconds.
cd /data/local/tmp/cc18
end=$(( $(cut -d. -f1 /proc/uptime) + $1 )); i=0; bad=0; hk=0
while [ $(cut -d. -f1 /proc/uptime) -lt $end ]; do
  i=$((i+1))
  if [ $((i % 25)) -eq 0 ]; then
    ./vdec /dev/video0 clip1080.au - 1920 1080 | grep ^frame | sort -k2n > soakh.txt
    cmp -s soakh.txt ref1080.sorted && hk=$((hk+1)) || { bad=$((bad+1)); echo "HASHFAIL i=$i"; }
  else
    if [ $((i % 2)) -eq 0 ]; then c=clip720.au; w=1280; h=720; n=60; else c=clip1080.au; w=1920; h=1080; n=30; fi
    r=$(./vdec /dev/video0 $c n $w $h | tail -1)
    case "$r" in "fed $n decoded $n last"*) ;; *) bad=$((bad+1)); echo "FAIL i=$i $r";; esac
  fi
  [ $((i % 100)) -eq 0 ] && echo "i=$i bad=$bad hashok=$hk temp=$(cat /sys/class/thermal/thermal_zone0/temp) memfree=$(grep MemAvailable /proc/meminfo | tr -s ' ' | cut -d' ' -f2)"
done
echo "DONE iterations=$i bad=$bad hashok=$hk"
