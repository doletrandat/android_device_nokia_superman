// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../p66/target-trace-core.h"

namespace superman_current_trace {
constexpr uint64_t kArmWindowNs = UINT64_C(5000000000);

// Serialized by the primary compositor lock. One immutable arm per HAL process:
// two current reads and two commit attempts, including failures. Never reset.
struct Budget {
  char token[17] = {};
  uint64_t begin_ns = 0;
  unsigned current_attempts = 0;
  unsigned commit_attempts = 0;
  unsigned attempt = 0;

  bool Selected(const char *selector, const char *arm, uint64_t now) {
    if (!selector || strcmp(selector, "p67") ||
        !superman_trace::TokenValid(arm) || !now) return false;
    if (!token[0]) {
      memcpy(token, arm, sizeof(token));
      begin_ns = now;
    }
    return !strcmp(token, arm) && now >= begin_ns &&
           now - begin_ns <= kArmWindowNs;
  }
  bool Current(const char *selector, const char *arm, uint64_t now) {
    if (!Selected(selector, arm, now) || current_attempts >= 2) return false;
    attempt = ++current_attempts;
    return true;
  }
  bool Commit(const char *selector, const char *arm, uint64_t now) {
    if (!Selected(selector, arm, now) || !current_attempts ||
        commit_attempts >= 2) return false;
    attempt = ++commit_attempts;
    return true;
  }
};
}  // namespace superman_current_trace
