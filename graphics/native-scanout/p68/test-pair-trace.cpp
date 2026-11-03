// SPDX-License-Identifier: Apache-2.0
#include "pair-trace-core.h"
#include <assert.h>
#include <stdio.h>
#include <limits>
#include <initializer_list>

int main() {
  using namespace superman_pair_trace;
  constexpr char arm[] = "0123456789abcdef";
  Budget b;
  assert(!b.Begin("p67", arm, 1));
  assert(!b.Begin("p68", "0123456789abcdeg", 1));
  assert(!b.Begin("p68", arm, 0));
  assert(b.attempts == 0 && !b.token[0]);
  assert(b.Begin("p68", arm, 100));
  assert(b.attempts == 1 && b.attempt_begin_ns == 100);
  assert(!b.Begin("p68", "1111111111111111", 101));
  assert(!b.Begin("p68", "", 101));
  assert(!b.Begin("p68", arm, 99));
  assert(b.Begin("p68", arm, 100 + kWindowNs));
  assert(!b.Begin("p68", arm, 100 + kWindowNs));
  Budget expired;
  assert(expired.Begin("p68", arm, 2));
  assert(!expired.Begin("p68", arm, 3 + kWindowNs));
  assert(expired.attempts == 1);
  Budget overflow;
  auto maximum = std::numeric_limits<uint64_t>::max();
  assert(overflow.Begin("p68", arm, maximum - 2));
  assert(overflow.Begin("p68", arm, maximum));
  assert(SettingsName("com.android.settings/com.android.settings.SubSettings#0"));
  assert(SettingsName("com.android.settings/com.android.settings.SubSettings#1234567890"));
  assert(!SettingsName(nullptr));
  assert(!SettingsName("com.android.settings/com.android.settings.SubSettings#"));
  assert(!SettingsName("com.android.settings/com.android.settings.SubSettings#12345678901"));
  assert(!SettingsName("com.android.settings/com.android.settings.SubSettings#0\n"));
  assert(!SettingsName("com.android.systemui#0"));
  assert(Layout(720, 1280, 2944, 4194304, 1, false));
  assert(Layout(720, 1280, 2944, 4194304, 5, true));
  assert(!Layout(720, 1280, 2944, 4194304, 1, true));
  assert(!Layout(720, 1280, 2944, 4194304, 2, false));
  assert(!Layout(719, 1280, 2944, 4194304, 5, true));
  assert(!Layout(720, 1279, 2944, 4194304, 5, true));
  assert(!Layout(720, 1280, 2945, 4194304, 5, true));
  assert(!Layout(720, 1280, 2944, 2944 * 1280 - 1, 5, true));
  for (int draw : {0, -1}) for (int wait : {0, -110})
    for (int input : {-62, 0, 1}) for (int output : {-62, 0, 1})
      assert(Fences(draw, wait, input, output) ==
             (draw == 0 && wait == 0 && input == 1 && output == 1));
  puts("PASS pair budget/name/layout/fence guards including expiry/overflow/failure consumption");
}
