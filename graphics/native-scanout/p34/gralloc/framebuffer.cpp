// SPDX-License-Identifier: Apache-2.0
// The native allocator must never fall back to CPU writes into simplefb.
#include <hardware/gralloc.h>
#include <cerrno>
int superman_fb_open(const hw_module_t*, hw_device_t** output) {
    if (output) *output = nullptr;
    return -ENOTSUP;
}
