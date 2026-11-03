#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
python3 - "$tmp" <<'PY'
import sys
from pathlib import Path
root=Path(sys.argv[1])
for name,size in [('modem_fs1',1572864),('modem_fs2',1572864),('modem_fsg',1572864),('modem_fsc',16384)]:
 (root/name).write_bytes(b'\x33'*size)
PY
"${CC:-cc}" -g -O1 -Werror=format -fsanitize=address,undefined -fno-omit-frame-pointer \
 -ffunction-sections -fdata-sections -Wl,--gc-sections \
 -I"$src/upstream" -I"$src/qrtr/include" -I"$src/qrtr/lib" \
 -DPF29_STORAGE_ROOT=\""$tmp"\" "$src/tests/protocol.c" \
 "$src/upstream/qmi_rmtfs.c" "$src/qrtr/lib/qmi.c" "$src/qrtr/lib/logging.c" \
 -lm -o "$tmp/protocol-test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/protocol-test"
