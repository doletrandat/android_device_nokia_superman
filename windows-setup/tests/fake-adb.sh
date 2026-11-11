#!/bin/bash
# fake adb for testing the PowerShell tool
F="$(dirname "$0")/../../../phone"; R=/data/local/tmp/superman-internal
echo "adb $*" >> "$F/calls.log"
case "$1" in
start-server|wait-for-device) ;;
devices) printf 'List of devices attached\r\nsuperman\tdevice\r\n\r\n' ;;
root) echo "restarting adbd as root" ;;
push) cp "$2" "$F/$(basename "$3")" ;;
pull) cp "$F/${2#$R/}" "$3" ;;
exec-out) [ "$2 $3" = "cat /dev/block/mmcblk0p27" ] || { echo "FAKE ADB: unhandled exec-out: $*" >&2; exit 97; }
  if [ -e "$F/short-data" ]; then head -c 1000000 "$F/data.dev"; else cat "$F/data.dev"; fi ;;
shell)
  c="$2"
  case "$c" in
  "getprop ro.product.device") printf 'superman\r\n' ;;
  "getprop ro.hardware") echo superman ;;
  "id -u") echo 0 ;;
  "rm -rf "*|"chmod "*|"rm -f "*) ;;
  *"superman-install-internal.sh check") echo "phone: Lumia 730, alpha from SD"; echo "internal storage: layout=windows" >&2; echo "check OK" ;;
  *"superman-install-internal.sh prepare") mkdir -p "$F/plan"; for n in old-primary old-backup new-primary new-backup; do head -c 17408 /dev/urandom > "$F/plan/$n.bin"; done; echo "OLD_DATA_FIRST=5292032" > "$F/plan/plan.env"; (cd "$F/plan" && sha256sum old-primary.bin old-backup.bin new-primary.bin new-backup.bin > PLAN.sha256); echo "plan written" ;;
  "for u in "*"PARTNAME=Data "*) [ -e "$F/data.dev" ] || head -c 150994944 /dev/urandom > "$F/data.dev"; echo "DEVNAME=mmcblk0p27" ;;
  "blockdev --getsz /dev/block/mmcblk0p27") echo 294912 ;;
  "sha256sum /dev/block/mmcblk0p27") echo "$(sha256sum < "$F/data.dev" | cut -d' ' -f1)  /dev/block/mmcblk0p27" ;;
  "for u in "*) echo "DEVNAME=mmcblk0p25" ;;
  "dd if=/dev/block/mmcblk0p25 "*) head -c 3000000 /dev/urandom > "$F/EFIESP.img"; cp "$F/EFIESP.img" "$F/efiesp.dev" ;;
  "sha256sum /dev/block/mmcblk0p25") echo "$(sha256sum < "$F/efiesp.dev" | cut -d' ' -f1)  /dev/block/mmcblk0p25" ;;
  *"superman-install-internal.sh install "*) h=${c##* }; [ "$h" = "$(sha256sum < "$F/plan/old-primary.bin" | cut -d' ' -f1)" ] || { echo "ERROR: backup confirmation does not match" >&2; exit 1; }; echo "1/4 partition table"; echo "2/4 boot"; echo "3/4 system"; echo "4/4 data"; echo log > "$F/install.log"; [ -e "$F/fail-install" ] && { echo "ERROR: simulated failure" >&2; exit 1; }; echo "INSTALL OK" ;;
  *) echo "FAKE ADB: unhandled shell: $c" >&2; exit 99 ;;
  esac ;;
*) echo "FAKE ADB: unhandled: $*" >&2; exit 98 ;;
esac
