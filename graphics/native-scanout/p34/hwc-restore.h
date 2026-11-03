// SPDX-License-Identifier: Apache-2.0
#pragma once
// Caller owns the warm DRM master. No timing/panel/DPMS operation is used.
static int superman_hwc_restore(int fd, uint32_t crtc, uint32_t conn,
                                uint32_t plane, int32_t *fence) {
    if (!fence) return -EINVAL;
    *fence = -1;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) return -ENOMEM;
    int ret = 0, out = -1;
    auto add = [&](uint32_t object, uint32_t type, const char *name, uint64_t value) {
        if (ret) return;
        auto *props = drmModeObjectGetProperties(fd, object, type);
        uint32_t id = 0;
        if (props) for (uint32_t i = 0; i < props->count_props; ++i) {
            auto *p = drmModeGetProperty(fd, props->props[i]);
            if (p && !strcmp(p->name, name)) id = p->prop_id;
            if (p) drmModeFreeProperty(p);
        }
        if (props) drmModeFreeObjectProperties(props);
        if (!id || drmModeAtomicAddProperty(req, object, id, value) < 0) ret = -EINVAL;
    };
    add(conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", 0);
    add(crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", 0);
    add(crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", 0);
    add(crtc, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR", reinterpret_cast<uintptr_t>(&out));
    add(plane, DRM_MODE_OBJECT_PLANE, "FB_ID", 0);
    add(plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0);
    if (!ret) ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
    drmModeAtomicFree(req);
    if (!ret && (out < 0 || sync_wait(out, 1000))) ret = -ETIMEDOUT;
    if (ret) { if (out >= 0) close(out); return ret; }
    *fence = out;
    return 0;
}
