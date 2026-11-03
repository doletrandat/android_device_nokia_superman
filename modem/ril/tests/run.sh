#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
root=${AOSP_ROOT:-$(CDPATH= cd -- "$src/../../../../.." && pwd)}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"${CC:-cc}" -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -fno-omit-frame-pointer -I"$root/hardware/ril/include" \
 "$src/tests/model-test.c" "$src/model.c" "$src/sim-io.c" "$src/calls.c" "$src/subscription.c" -lm -o "$tmp/model-test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/model-test"

"${CC:-cc}" -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -fno-omit-frame-pointer "$src/tests/sms-test.c" "$src/sms.c" -lm -o "$tmp/sms-test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/sms-test"

"${CC:-cc}" -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -fno-omit-frame-pointer -I"$root/hardware/ril/include" -I"$src/tests/include" \
 "$src/tests/sms-ril-test.c" "$src/model.c" "$src/sim-io.c" "$src/calls.c" "$src/subscription.c" "$src/sms.c" "$src/data-call.c" "$src/data-link.c" -pthread -lm -o "$tmp/sms-ril-test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/sms-ril-test"

"${CC:-cc}" -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -fno-omit-frame-pointer -I"$src/tests/include" \
 "$src/tests/sms-transport-test.c" "$src/sms.c" -pthread -lm -o "$tmp/sms-transport-test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/sms-transport-test"

"${CC:-cc}" -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -fno-omit-frame-pointer -I"$root/hardware/ril/include" "$src/tests/data-call-test.c" "$src/data-call.c" -lm -o "$tmp/data-call-test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/data-call-test"
"${CC:-cc}" -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -fno-omit-frame-pointer -I"$src/tests/include" "$src/tests/async-transport-test.c" -pthread -lm -o "$tmp/async-transport-test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/async-transport-test"
"${CC:-cc}" -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -fno-omit-frame-pointer -I"$root/hardware/ril/include" -I"$src/tests/include" \
 "$src/tests/data-ril-test.c" "$src/model.c" "$src/sim-io.c" "$src/calls.c" "$src/subscription.c" "$src/sms.c" "$src/data-call.c" -pthread -lm -o "$tmp/data-ril-test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/data-ril-test"
