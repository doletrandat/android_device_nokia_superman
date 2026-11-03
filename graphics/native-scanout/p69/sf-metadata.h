// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "metadata-runtime.h"
#include <compositionengine/LayerFE.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#include "../../../../../../external/libdrm/android/gralloc_handle.h"
#pragma clang diagnostic pop

namespace android::superman_sf_metadata {
inline superman_metadata::Runtime& State() { static superman_metadata::Runtime s; return s; }
inline bool Enabled() {
    superman_metadata::ErrnoGuard guard;
    char selector[PROPERTY_VALUE_MAX] = {};
    property_get("ro.superman.native_metadata_trace", selector, "");
    return !strcmp(selector, "p69");
}
inline bool Protected(const sp<GraphicBuffer>& b) {
    return b && (b->getUsage() & GRALLOC_USAGE_PROTECTED);
}
inline superman_metadata::Backing Backing(const sp<GraphicBuffer>& b) {
    if (!b || Protected(b)) return {};
    const auto* h = b->handle;
    if (!h || h->version != sizeof(native_handle_t) ||
        h->numFds != GRALLOC_HANDLE_NUM_FDS || h->numInts != GRALLOC_HANDLE_NUM_INTS) return {};
    const auto* g = gralloc_handle(h);
    if (g->magic != GRALLOC_HANDLE_MAGIC || g->version != GRALLOC_HANDLE_VERSION ||
        (g->usage & GRALLOC_USAGE_PROTECTED)) return {};
    return superman_metadata::Identity(g->prime_fd);
}
inline void Matrix(unsigned id, size_t index, const char* kind, const mat4& m) {
    ALOGI("superman_meta role=sf phase=matrix arm=%s pid=%d seq=%u index=%zu kind=%s"
          " values=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
          State().budget.token, getpid(), id, index, kind,
          double(m[0][0]), double(m[0][1]), double(m[0][2]), double(m[0][3]),
          double(m[1][0]), double(m[1][1]), double(m[1][2]), double(m[1][3]),
          double(m[2][0]), double(m[2][1]), double(m[2][2]), double(m[2][3]),
          double(m[3][0]), double(m[3][1]), double(m[3][2]), double(m[3][3]));
}
inline unsigned Begin(bool internal, const sp<GraphicBuffer>& target,
                      const std::vector<compositionengine::LayerFE::LayerSettings>& layers) {
    superman_metadata::ErrnoGuard guard;
    if (!internal || !target || Protected(target)) return 0;
    for (const auto& l : layers) if (Protected(l.source.buffer.buffer)) return 0;
    unsigned id = State().Begin(superman_metadata::kComposeLimit,
                                superman_metadata::kComposeIntervalNs);
    if (!id) return 0;
    auto backing = Backing(target);
    ALOGI("superman_meta role=sf phase=begin arm=%s pid=%d seq=%u ns=%" PRIu64
          " target=%" PRIu64 " known=%d dev=%" PRIu64 " ino=%" PRIu64
          " width=%u height=%u stride_pixels=%u format=%d requests=%zu truncated=%d",
          State().budget.token, getpid(), id, superman_metadata::Now(), target->getId(),
          backing.known, backing.dev, backing.ino, target->getWidth(), target->getHeight(),
          target->getStride(), int(target->getPixelFormat()), layers.size(),
          layers.size() > superman_metadata::kLayerLimit);
    for (size_t i = 0; i < layers.size() && i < superman_metadata::kLayerLimit; ++i) {
        const auto& l = layers[i]; const auto& b = l.source.buffer;
        backing = Backing(b.buffer);
        ALOGI("superman_meta role=sf phase=input arm=%s pid=%d seq=%u index=%zu"
              " request_id=%" PRIu64 " actual_id=%" PRIu64 " frame=%" PRIu64
              " known=%d dev=%" PRIu64 " ino=%" PRIu64 " buffer=%d acquire_present=%d"
              " alpha=%.9g bounds=%.9g,%.9g,%.9g,%.9g texture=%u opaque=%d filter=%d"
              " premult=%d blend_disabled=%d dataspace=%d blur=%d rounded=%.9g shadow=%.9g",
              State().budget.token, getpid(), id, i, l.bufferId,
              b.buffer ? b.buffer->getId() : UINT64_C(0), l.frameNumber, backing.known,
              backing.dev, backing.ino, bool(b.buffer), b.fence && b.fence->get() >= 0,
              double(float(l.alpha)), double(l.geometry.boundaries.left),
              double(l.geometry.boundaries.top), double(l.geometry.boundaries.right),
              double(l.geometry.boundaries.bottom), b.textureName, b.isOpaque, b.useTextureFiltering,
              b.usePremultipliedAlpha, l.disableBlending, int(l.sourceDataspace),
              l.backgroundBlurRadius, double(l.geometry.roundedCornersRadius), double(l.shadow.length));
        Matrix(id, i, "position", l.geometry.positionTransform);
        Matrix(id, i, "texture", b.textureTransform);
    }
    return id;
}
inline void Finish(unsigned id, const char* cache, bool drawn, int status, int ready_fd) {
    if (!id) return;
    superman_metadata::ErrnoGuard guard;
    ALOGI("superman_meta role=sf phase=end arm=%s pid=%d seq=%u ns=%" PRIu64
          " cache=%s drawn=%d draw_status=%d ready_present=%d",
          State().budget.token, getpid(), id, superman_metadata::Now(), cache, drawn,
          status, ready_fd >= 0);
}
}
