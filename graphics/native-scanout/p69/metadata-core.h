// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <string.h>

namespace superman_metadata {
constexpr uint64_t kWindowNs = UINT64_C(120000000000);
constexpr uint64_t kComposeIntervalNs = UINT64_C(250000000);
constexpr unsigned kComposeLimit = 480;
constexpr unsigned kMapLimit = 64;
constexpr unsigned kLayerLimit = 16;
// Caller serializes access: SF composition thread, or gralloc module mutex.
// An arm is one-shot per process lifetime. Clearing/replacing it never rearms.
struct Budget {
    char token[21] = {};
    uint64_t start = 0, last = 0;
    unsigned count = 0;
    bool exhausted = false;
    static bool Valid(const char* arm) {
        if (!arm || strlen(arm) != 20 || strncmp(arm, "m69-", 4)) return false;
        for (unsigned i = 4; i < 20; ++i)
            if (!((arm[i] >= '0' && arm[i] <= '9') || (arm[i] >= 'a' && arm[i] <= 'f')))
                return false;
        return true;
    }
    unsigned Begin(const char* arm, uint64_t now, unsigned limit, uint64_t interval) {
        if (exhausted || !now || !Valid(arm)) return 0;
        if (!start) { memcpy(token, arm, sizeof(token)); start = now; }
        if (now < start || now - start >= kWindowNs || count >= limit) {
            exhausted = true; return 0;
        }
        if (strcmp(token, arm)) return 0;
        if (last && (now < last || now - last < interval)) return 0;
        last = now;
        return ++count;
    }
};
}
