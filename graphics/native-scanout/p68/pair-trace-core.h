// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../p66/target-trace-core.h"

namespace superman_pair_trace {
constexpr uint64_t kWindowNs = UINT64_C(5000000000);
constexpr int kWaitMs = 250;
// Serialized by the SF composition thread. One token for this process lifetime.
struct Budget {
  char token[17] = {};
  uint64_t begin_ns = 0;
  unsigned attempts = 0;
  uint64_t attempt_begin_ns = 0;
  bool Begin(const char *selector, const char *arm, uint64_t now) {
    if (!selector || strcmp(selector, "p68") ||
        !superman_trace::TokenValid(arm) || !now) return false;
    if (!token[0]) { memcpy(token, arm, sizeof(token)); begin_ns = now; }
    if (strcmp(token, arm) || now < begin_ns ||
        now - begin_ns > kWindowNs || attempts >= 2) return false;
    attempt_begin_ns = now;
    ++attempts; // All selected failures consume their slot.
    return true;
  }
};
inline bool SettingsName(const char *name) {
  constexpr char prefix[] = "com.android.settings/com.android.settings.SubSettings#";
  if (!name || strncmp(name, prefix, sizeof(prefix) - 1)) return false;
  const char *suffix = name + sizeof(prefix) - 1;
  size_t n = strlen(suffix);
  if (!n || n > 10) return false;
  for (size_t i = 0; i < n; ++i) if (suffix[i] < '0' || suffix[i] > '9') return false;
  return true;
}
inline bool Layout(uint32_t width, uint32_t height, uint32_t stride, uint64_t size,
                   uint32_t format, bool target) {
  // HAL_RGBA_8888=1, HAL_BGRA_8888=5. Exact live Settings geometry only.
  return superman_trace::LayoutValid(width, height, stride, size) &&
         (target ? format == 5 : (format == 1 || format == 5));
}
inline bool Fences(int draw, int wait, int input, int output) {
  return draw == 0 && wait == 0 && input == 1 && output == 1;
}
} // namespace superman_pair_trace
