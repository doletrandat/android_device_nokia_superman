// SPDX-License-Identifier: Apache-2.0
#include "target-trace-core.h"
#include <assert.h>
#include <vector>
#include <stdio.h>

int main() {
  using namespace superman_trace;
  Budget b;
  assert(!b.Begin("p65", "0123456789abcdef"));
  assert(!b.Begin(nullptr, "0123456789abcdef"));
  assert(!b.Begin("p66", nullptr));
  assert(!b.Begin("p66", "../0123456789abc"));
  assert(!b.Begin("p66", "0123456789abcdeF"));
  assert(!b.Begin("p66", "0123456789abcde"));
  assert(!b.Begin("p66", "0123456789abcdef0"));
  assert(b.attempts == 0 && !b.token[0]);
  assert(b.Begin("p66", "0123456789abcdef"));
  assert(!b.Begin("p66", "fedcba9876543210"));
  assert(b.Begin("p66", "0123456789abcdef"));
  assert(!b.Begin("p66", "0123456789abcdef"));
  assert(!b.Begin("p66", "fedcba9876543210"));
  assert(b.attempts == 2);

  assert(LayoutValid(720, 1280, 2880, kBytes));
  assert(LayoutValid(720, 1280, 65532, uint64_t(65532) * kHeight));
  assert(!LayoutValid(719, 1280, 2880, kBytes));
  assert(!LayoutValid(720, 1279, 2880, kBytes));
  assert(!LayoutValid(720, 1280, 2876, kBytes));
  assert(!LayoutValid(720, 1280, 2881, kBytes + kHeight));
  assert(!LayoutValid(720, 1280, 65536, UINT64_MAX));
  assert(!LayoutValid(720, 1280, 2880, kBytes - 1));
  assert(!LayoutValid(UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT64_MAX));
  assert(!ReadAllowed(0, false, 1));
  assert(!ReadAllowed(-62, false, 1));
  assert(!ReadAllowed(-61, true, 1));
  assert(ReadAllowed(1, false, 0));
  assert(ReadAllowed(1, true, 1));
  assert(!ReadAllowed(1, true, 0));
  assert(!ReadAllowed(1, true, -62));

  // Padded rows: exclude padding, preserve both guard bytes and every pixel.
  constexpr size_t stride = 2912;
  std::vector<uint8_t> input(stride * kHeight, 0xde), output(kBytes + 2, 0xa5);
  for (size_t y = 0; y < kHeight; ++y)
    for (size_t x = 0; x < kRowBytes; ++x) input[y * stride + x] = (x + 17 * y) % 251;
  assert(CopyRows(output.data() + 1, kBytes, input.data(), stride, input.size()));
  assert(output.front() == 0xa5 && output.back() == 0xa5);
  for (size_t y = 0; y < kHeight; ++y)
    for (size_t x = 0; x < kRowBytes; ++x)
      assert(output[1 + y * kRowBytes + x] == (x + 17 * y) % 251);
  assert(!CopyRows(nullptr, kBytes, input.data(), stride, input.size()));
  assert(!CopyRows(output.data(), kBytes, nullptr, stride, input.size()));
  assert(!CopyRows(output.data(), kBytes - 1, input.data(), stride, input.size()));
  assert(!CopyRows(output.data(), kBytes, input.data(), stride, input.size() - 1));
  assert(Hash(reinterpret_cast<const uint8_t *>("hello"), 5) == UINT64_C(0xa430d84680aabd0b));
  puts("PASS: arm exhaustion, invalid selectors/tokens/layout/fences, padded row copy and hash vector");
}
