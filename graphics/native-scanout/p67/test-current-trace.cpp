// SPDX-License-Identifier: Apache-2.0
#include "current-trace-core.h"
#include "../p34/composer/include/autofd.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>

int main() {
  using superman_current_trace::Budget;
  const char *token = "0123456789abcdef";
  constexpr uint64_t window = superman_current_trace::kArmWindowNs;
  Budget b;
  assert(!b.Current("p66", token, 100));
  assert(!b.Current("p67", "../0123456789abc", 100));
  assert(!b.Current("p67", token, 0));
  assert(!b.token[0]);
  assert(!b.Commit("p67", token, 100)); // Cannot start post-flip-only trace.
  assert(b.Current("p67", token, 100));
  assert(!b.Current("p67", "fedcba9876543210", 101));
  assert(!b.Current("p67", "", 101));
  assert(b.Current("p67", token, 102));
  assert(!b.Current("p67", token, 103));
  assert(b.Commit("p67", token, 104));
  assert(b.Commit("p67", token, 105));
  assert(!b.Commit("p67", token, 106));
  assert(b.current_attempts == 2 && b.commit_attempts == 2);
  Budget expired;
  assert(expired.Current("p67", token, 10));
  assert(expired.Commit("p67", token, 10 + window));
  assert(!expired.Commit("p67", token, 11 + window));
  assert(!expired.Current("p67", "fedcba9876543210", 11 + window));
  assert(!expired.Current("p67", token, 9)); // Backward clock cannot revive arm.
  Budget overflow;
  assert(overflow.Current("p67", token, UINT64_MAX - 1));
  assert(!overflow.Current("p67", token, 1));

  // Use the actual HWC UniqueFd wrapper: independent duplicate survives normal
  // present-fence transfer/close, is CLOEXEC, and closes at retirement. This
  // checks descriptor ownership only, not Linux sync_file fence semantics.
  int p[2];
  assert(pipe(p) == 0);
  int retained = -1;
  {
    android::UniqueFd present(p[0]);
    retained = fcntl(present.get(), F_DUPFD_CLOEXEC, 3);
    assert(retained >= 3);
    android::UniqueFd diagnostic(retained);
    assert(fcntl(diagnostic.get(), F_GETFD) & FD_CLOEXEC);
    assert(close(present.Release()) == 0);
    assert(write(p[1], "x", 1) == 1);
    char value = 0;
    assert(read(diagnostic.get(), &value, 1) == 1 && value == 'x');
  }
  assert(fcntl(retained, F_GETFD) == -1);
  assert(close(p[1]) == 0);
  assert(fcntl(-1, F_DUPFD_CLOEXEC, 3) == -1);
  puts("PASS: immutable arm, six-read bound, expiry/backward-clock and retained FD ownership");
}
