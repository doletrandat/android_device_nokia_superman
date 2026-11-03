// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace superman_trace {
constexpr size_t kRowBytes = 720 * 4;
constexpr size_t kHeight = 1280;
constexpr size_t kBytes = kRowBytes * kHeight;

inline bool TokenValid(const char *token) {
  if (!token || strlen(token) != 16) return false;
  for (size_t i = 0; i < 16; ++i)
    if (!((token[i] >= '0' && token[i] <= '9') ||
          (token[i] >= 'a' && token[i] <= 'f'))) return false;
  return true;
}

// One arm per HAL process, two attempts (including failed commits).
// Reading a different token cannot reset the budget.
struct Budget {
  char token[17] = {};
  unsigned attempts = 0;
  bool Begin(const char *selector, const char *arm) {
    if (!selector || strcmp(selector, "p66") || !TokenValid(arm)) return false;
    if (!token[0]) memcpy(token, arm, sizeof(token));
    if (strcmp(token, arm) || attempts >= 2) return false;
    ++attempts;
    return true;
  }
};

inline bool LayoutValid(uint32_t width, uint32_t height, uint32_t stride,
                        uint64_t size) {
  return width == 720 && height == kHeight && stride >= kRowBytes &&
         stride <= 65535 && !(stride % 4) && size >= uint64_t(stride) * height;
}

inline bool ReadAllowed(int acquire, bool after_latch, int latch) {
  return acquire == 1 && (!after_latch || latch == 1);
}

inline bool CopyRows(uint8_t *dst, size_t dst_size, const uint8_t *src,
                     uint32_t stride, uint64_t src_size) {
  if (!dst || !src || dst_size != kBytes ||
      !LayoutValid(720, kHeight, stride, src_size)) return false;
  for (size_t y = 0; y < kHeight; ++y)
    memcpy(dst + y * kRowBytes, src + y * stride, kRowBytes);
  return true;
}

inline uint64_t Hash(const uint8_t *data, size_t size) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}
}  // namespace superman_trace
