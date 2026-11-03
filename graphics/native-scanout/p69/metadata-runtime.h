// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "metadata-core.h"
#include <cutils/properties.h>
#include <cutils/log.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
namespace superman_metadata {
// Hooks preserve errno even when property/clock/stat/log operations fail.
struct ErrnoGuard { int saved = errno; ~ErrnoGuard() { errno = saved; } };
inline uint64_t Now() {
    timespec ts = {};
    return clock_gettime(CLOCK_MONOTONIC, &ts) ? 0 :
            uint64_t(ts.tv_sec) * UINT64_C(1000000000) + uint64_t(ts.tv_nsec);
}
inline bool BoardAndPipeline() {
    char compatible[64] = {}, gralloc[PROPERTY_VALUE_MAX] = {}, hwc[PROPERTY_VALUE_MAX] = {};
    int fd = open("/proc/device-tree/compatible", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    ssize_t n = read(fd, compatible, sizeof(compatible)); close(fd);
    constexpr char expected[] = "nokia,superman\0qcom,msm8226\0";
    property_get("ro.hardware.gralloc", gralloc, "");
    property_get("ro.hardware.hwcomposer", hwc, "");
    return n == ssize_t(sizeof(expected) - 1) && !memcmp(compatible, expected, sizeof(expected) - 1) &&
            !strcmp(gralloc, "superman_native") && !strcmp(hwc, "superman_native");
}
struct Runtime {
    Budget budget;
    pid_t pid = 0;
    bool checked = false, allowed = false;
    unsigned Begin(unsigned limit, uint64_t interval) {
        ErrnoGuard guard;
        // A fork gets its own process budget, even if zygote used gralloc earlier.
        pid_t current = getpid();
        if (pid != current) { budget = {}; checked = allowed = false; pid = current; }
        if (budget.exhausted) return 0;
        char selector[PROPERTY_VALUE_MAX] = {}, arm[PROPERTY_VALUE_MAX] = {};
        property_get("ro.superman.native_metadata_trace", selector, "");
        if (strcmp(selector, "p69")) return 0;
        property_get("debug.superman.metadata_trace", arm, "");
        if (!Budget::Valid(arm)) return 0;
        if (!checked) { allowed = BoardAndPipeline(); checked = true; }
        return allowed ? budget.Begin(arm, Now(), limit, interval) : 0;
    }
};
// FD numbers and native-handle addresses are not backing identities. fstat of
// an already-owned dma-buf is metadata only; failure is explicitly unknown.
struct Backing { uint64_t dev = 0, ino = 0; bool known = false; };
inline Backing Identity(int fd) {
    struct stat s = {};
    Backing result;
    if (fd >= 0 && !fstat(fd, &s)) {
        result.dev = uint64_t(s.st_dev); result.ino = uint64_t(s.st_ino); result.known = true;
    }
    return result;
}
}
