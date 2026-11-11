#!/system/bin/sh
# Undo CC23 (Venus auto-load): remove the init service, its script and allowlists.
set -eu
mount -o remount,rw /
rm -f /vendor/etc/init/superman-venus.rc /vendor/bin/superman-venus.sh \
      /vendor/etc/superman/venus-kernels.txt /vendor/etc/superman/venus-files.sha256
sync; mount -o remount,ro / || true
echo CC23_ROLLBACK_DONE reboot-required
