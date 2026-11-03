// SPDX-License-Identifier: Apache-2.0
#include "metadata-core.h"
#include <assert.h>
#include <initializer_list>
#include <stdio.h>
using namespace superman_metadata;
int main() {
    constexpr const char* arm = "m69-0123456789abcdef";
    assert(Budget::Valid(arm));
    for (auto bad : {"", "m69-0123", "m69-0123456789abcdeG", "m68-0123456789abcdef", "m69-0123456789abcdef0"})
        assert(!Budget::Valid(bad));
    Budget b;
    assert(!b.Begin(arm, 0, 2, 10));
    assert(b.Begin(arm, 100, 2, 10) == 1);
    assert(!b.Begin(arm, 109, 2, 10));
    assert(!b.Begin("m69-fedcba9876543210", 110, 2, 10));
    assert(b.Begin(arm, 110, 2, 10) == 2);
    assert(!b.Begin(arm, 120, 2, 10) && b.exhausted);
    assert(!b.Begin("m69-fedcba9876543210", 130, 2, 0));
    b = {}; assert(b.Begin(arm, 1, 3, 0) == 1);
    assert(!b.Begin(arm, 1 + kWindowNs, 3, 0) && b.exhausted);
    b = {}; assert(b.Begin(arm, 100, 3, 0) == 1);
    assert(!b.Begin(arm, 99, 3, 0) && b.exhausted);
    b = {}; for (unsigned i = 1; i <= kMapLimit; ++i) assert(b.Begin(arm, i, kMapLimit, 0) == i);
    assert(!b.Begin(arm, 65, kMapLimit, 0));
    b = {}; for (unsigned i = 1; i <= kComposeLimit; ++i)
        assert(b.Begin(arm, 1 + uint64_t(i-1) * kComposeIntervalNs, kComposeLimit, kComposeIntervalNs) == i);
    assert(!b.Begin(arm, kWindowNs, kComposeLimit, kComposeIntervalNs));
    puts("PASS token, cadence, one-shot replacement, limit, deadline, backward clock");
}
