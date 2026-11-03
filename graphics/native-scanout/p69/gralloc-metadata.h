// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "metadata-runtime.h"
namespace superman_gralloc_metadata {
inline superman_metadata::Runtime& State() { static superman_metadata::Runtime s; return s; }
inline unsigned Begin(const gralloc_handle_t* h, int usage, int x, int y, int w, int height) {
    superman_metadata::ErrnoGuard guard;
    if (h->usage & GRALLOC_USAGE_PROTECTED) return 0;
    unsigned id = State().Begin(superman_metadata::kMapLimit, 0);
    if (!id) return 0;
    auto backing = superman_metadata::Identity(h->prime_fd);
    ALOGI("superman_meta role=gralloc phase=map_begin arm=%s pid=%d seq=%u ns=%" PRIu64
          " known=%d dev=%" PRIu64 " ino=%" PRIu64 " width=%u height=%u stride_bytes=%u"
          " format=%u usage=0x%x region=%d,%d,%d,%d read=%d write=%d",
          State().budget.token, getpid(), id, superman_metadata::Now(), backing.known,
          backing.dev, backing.ino, h->width, h->height, h->stride, h->format, usage,
          x, y, w, height, bool(usage & GRALLOC_USAGE_SW_READ_MASK),
          bool(usage & GRALLOC_USAGE_SW_WRITE_MASK));
    return id;
}
inline void Event(unsigned id, const char* phase, int status) {
    if (!id) return;
    superman_metadata::ErrnoGuard guard;
    ALOGI("superman_meta role=gralloc phase=%s arm=%s pid=%d seq=%u ns=%" PRIu64 " status=%d",
          phase, State().budget.token, getpid(), id, superman_metadata::Now(), status);
}
}
