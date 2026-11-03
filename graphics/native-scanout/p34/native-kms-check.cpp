// SPDX-License-Identifier: Apache-2.0
// Inactive P34 laboratory gate. --probe queries KMS/actual vblank only.
// --flip-test writes three CPU-produced test targets, imports PRIME and verifies
// latched output fences then restores inherited simplefb. No GPU rendering test.
// --gpu-test renders imported targets, exports GPU fences and tests a separate
// one-shot delayed acquire gate before restoring.
// Never invoke on P30: requires an explicit native boot and absent SurfaceFlinger.
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <sync/sync.h>
#include <cutils/properties.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sstream>
#include <unistd.h>
#include <vector>

static uint32_t property(int fd, uint32_t object, uint32_t type, const char *name) {
    auto *props = drmModeObjectGetProperties(fd, object, type);
    if (!props) return 0;
    uint32_t result = 0;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        auto *p = drmModeGetProperty(fd, props->props[i]);
        if (p && !strcmp(p->name, name)) result = p->prop_id;
        if (p) drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return result;
}
static bool version(int fd, const char *name) {
    auto *v = drmGetVersion(fd);
    bool result = v && v->name && v->name_len >= 0 && static_cast<size_t>(v->name_len) == strlen(name) && !memcmp(v->name, name, v->name_len);
    if (!result) fprintf(stderr, "NATIVE_KMS_ERROR stage=driver_identity expected=%s actual=%s errno=%d\n", name, v && v->name ? v->name : "unavailable", v ? 0 : errno);
    if (v) drmFreeVersion(v);
    return result;
}
static int probe_error(const char *stage, int status, int error) {
    fprintf(stderr, "NATIVE_KMS_ERROR stage=%s status=%d errno=%d (%s)\n", stage, status, error, strerror(error));
    return status;
}
static int display_fd(char *path, size_t size) {
    // MSM open initializes the GPU. Identify the display via passive sysfs first.
    // A missing expected node gets a bounded wait; ambiguity or other errors fail.
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        unsigned matches = 0;
        for (unsigned card = 0; card < 8; ++card) {
            char sysfs[96], driver[256];
            snprintf(sysfs, sizeof(sysfs), "/sys/class/drm/card%u/device/driver", card);
            ssize_t length = readlink(sysfs, driver, sizeof(driver) - 1);
            if (length < 0) {
                if (errno == ENOENT) continue;
                probe_error("discovery_sysfs", 3, errno); return -1;
            }
            if (static_cast<size_t>(length) == sizeof(driver) - 1) {
                probe_error("discovery_driver_length", 3, ENAMETOOLONG); return -1;
            }
            driver[length] = 0;
            const char *leaf = strrchr(driver, '/');
            if (strcmp(leaf ? leaf + 1 : driver, "superman-fw-kms")) continue;
            ++matches;
            snprintf(path, size, "/dev/dri/card%u", card);
        }
        if (matches > 1) { probe_error("discovery_ambiguous", 3, EEXIST); return -1; }
        if (matches == 1) {
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                fprintf(stderr, "NATIVE_KMS_STAGE display_selected device=%s attempt=%u\n", path, attempt);
                return fd;
            }
            if (errno != ENOENT) { probe_error("display_open", 3, errno); return -1; }
        }
        if (attempt + 1 < 20) usleep(50000);
    }
    probe_error("discovery_timeout", 3, ETIMEDOUT); return -1;
}
static bool boot_flags_ok(const std::string &cmdline, bool flips, bool gpu = false, bool hwc = false) {
    std::istringstream f(cmdline); std::string token;
    unsigned native = 0, stage = 0;
    std::string selected_stage;
    while (f >> token) {
        if (token == "androidboot.superman_native" || token.find("androidboot.superman_native=") == 0) {
            if (token != "androidboot.superman_native=p34") return false;
            ++native;
        }
        if (token == "androidboot.superman_native_stage" || token.find("androidboot.superman_native_stage=") == 0) {
            if (token != "androidboot.superman_native_stage=probe" &&
                token != "androidboot.superman_native_stage=flip" &&
                token != "androidboot.superman_native_stage=gpu" &&
                token != "androidboot.superman_native_stage=hwc") return false;
            std::string value = token.substr(strlen("androidboot.superman_native_stage="));
            if (stage && value != selected_stage) return false;
            selected_stage = value;
            ++stage;
        }
    }
    // LK repeats the complete command line on P41. Identical values agree;
    // conflicting, missing or bare values fail just like the startup contract.
    return native >= 1 && stage >= 1 && (!flips || selected_stage == "flip") && (!gpu || selected_stage == "gpu") && (!hwc || selected_stage == "hwc");
}
static bool allowed(bool flips, bool gpu = false, bool hwc = false) {
    char p[PROPERTY_VALUE_MAX];
    property_get("ro.superman.native_candidate", p, "");
    if (strcmp(p, "p34")) return false;
    std::ifstream f("/proc/cmdline");
    std::string cmdline((std::istreambuf_iterator<char>(f)), {});
    if (!boot_flags_ok(cmdline, flips, gpu, hwc)) return false;
    property_get("ro.superman.native_stage", p, "");
    const char *stage = cmdline.find("androidboot.superman_native_stage=hwc") != std::string::npos ? "hwc" : cmdline.find("androidboot.superman_native_stage=gpu") != std::string::npos ? "gpu" : cmdline.find("androidboot.superman_native_stage=flip") != std::string::npos ? "flip" : "probe";
    if (strcmp(p, stage)) return false;
    property_get("init.svc.surfaceflinger", p, "");
    if (!strcmp(p,"running") || !strcmp(p,"restarting")) return false;
    std::ifstream root("/proc/device-tree/compatible", std::ios::binary);
    std::string compatible((std::istreambuf_iterator<char>(root)), {});
    static const char exact[] = "nokia,superman\0qcom,msm8226";
    return compatible.size() == sizeof(exact) && !memcmp(compatible.data(), exact, sizeof(exact));
}
static bool mode_ok(const drmModeModeInfo &m) {
    return m.clock == 64549 && m.hdisplay == 720 && m.hsync_start == 780 &&
        m.hsync_end == 784 && m.htotal == 824 && m.vdisplay == 1280 &&
        m.vsync_start == 1295 && m.vsync_end == 1296 && m.vtotal == 1304 &&
        m.flags == (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC);
}
struct Target { gbm_bo *bo = nullptr; int prime = -1; uint32_t handle = 0, fb = 0; };
static int flip_test(int fd, const drmModeModeInfo &mode, uint32_t crtc, uint32_t conn, uint32_t pid) {
    int render = -1, result = 0;
    gbm_device *gbm = nullptr;
    Target target[3];
    uint32_t blob = 0;
    bool attempted = false, restored = false;
    auto commit = [&](uint32_t fb, bool test, bool restore) -> int {
        const char *stage = restore ? "restore" : test ? "test_only" : "flip";
        auto *req = drmModeAtomicAlloc();
        if (!req) { probe_error(stage, 12, ENOMEM); return -ENOMEM; }
        int fence = -1, ret = 0;
        auto add = [&](uint32_t obj, uint32_t type, const char *name, uint64_t val) {
            if (ret) return;
            uint32_t prop = property(fd, obj, type, name);
            if (!prop || drmModeAtomicAddProperty(req, obj, prop, val) < 0) {
                fprintf(stderr, "NATIVE_KMS_ERROR stage=atomic_property commit=%s name=%s errno=%d\n", stage, name, prop ? errno : ENOENT);
                ret = -EINVAL;
            }
        };
        add(conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", restore ? 0 : crtc);
        add(crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", restore ? 0 : 1);
        add(crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", restore ? 0 : blob);
        // TEST_ONLY validates state without requesting a completion fence.
        if (!test) add(crtc, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR", reinterpret_cast<uintptr_t>(&fence));
        add(pid, DRM_MODE_OBJECT_PLANE, "FB_ID", fb);
        add(pid, DRM_MODE_OBJECT_PLANE, "CRTC_ID", restore ? 0 : crtc);
        if (!restore) {
            add(pid, DRM_MODE_OBJECT_PLANE, "SRC_X", 0); add(pid, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
            add(pid, DRM_MODE_OBJECT_PLANE, "SRC_W", 720U << 16); add(pid, DRM_MODE_OBJECT_PLANE, "SRC_H", 1280U << 16);
            add(pid, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0); add(pid, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
            add(pid, DRM_MODE_OBJECT_PLANE, "CRTC_W", 720); add(pid, DRM_MODE_OBJECT_PLANE, "CRTC_H", 1280);
            // CPU writes were published by unmap; this is not a GPU acquire-fence test.
            add(pid, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", static_cast<uint64_t>(-1LL));
        }
        if (!ret) {
            if (!test && !restore) attempted = true;
            errno = 0;
            ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET | (test ? DRM_MODE_ATOMIC_TEST_ONLY : 0), nullptr);
            if (ret) fprintf(stderr, "NATIVE_KMS_ERROR stage=atomic_commit commit=%s ret=%d errno=%d\n", stage, ret, errno);
        }
        drmModeAtomicFree(req);
        if (!ret && !test) {
            if (fence < 0) { probe_error("output_fence_missing", 12, ENOENT); ret = -EIO; }
            else if (sync_wait(fence, 500)) { probe_error(restore ? "restore_fence_wait" : "flip_fence_wait", 12, errno); ret = -ETIMEDOUT; }
        }
        if (fence >= 0) close(fence);
        fprintf(stderr, "NATIVE_KMS_STAGE commit=%s fb=%u result=%d fence=%s\n", stage, fb, ret, test ? "not_requested" : !ret ? "signaled" : "unconfirmed");
        return ret;
    };
    auto run = [&]() -> int {
        errno = 0;
        render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (render < 0) return probe_error("render_open", 6, errno);
        if (!version(render, "msm")) return probe_error("render_version", 6, ENODEV);
        gbm = gbm_create_device(render);
        if (!gbm) return probe_error("gbm_device", 6, errno);
        for (unsigned i = 0; i < 3; ++i) {
            auto &t = target[i];
            errno = 0;
            t.bo = gbm_bo_create(gbm, 720, 1280, GBM_FORMAT_ARGB8888,
                GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
            if (!t.bo) return probe_error("target_allocate", 7, errno);
            if (gbm_bo_get_modifier(t.bo) != DRM_FORMAT_MOD_LINEAR) return probe_error("target_modifier", 7, EINVAL);
            uint32_t stride = 0; void *map_data = nullptr;
            auto *pixels = static_cast<uint8_t*>(gbm_bo_map(t.bo, 0, 0, 720, 1280, GBM_BO_TRANSFER_WRITE, &stride, &map_data));
            if (!pixels) return probe_error("target_map", 7, errno);
            uint32_t pitch = gbm_bo_get_stride(t.bo);
            if (stride < 2880 || stride % 4 || stride > 65535 || stride != pitch) {
                gbm_bo_unmap(t.bo, map_data);
                return probe_error("target_stride", 7, EINVAL);
            }
            for (unsigned y = 0; y < 1280; ++y) {
                auto *row = reinterpret_cast<uint32_t*>(pixels + y * stride);
                for (unsigned x = 0; x < 720; ++x) row[x] = 0xff000000U | (0x60U << (i * 8));
            }
            gbm_bo_unmap(t.bo, map_data);
            t.prime = gbm_bo_get_fd(t.bo);
            if (t.prime < 0) return probe_error("target_export", 7, errno);
            if (drmPrimeFDToHandle(fd, t.prime, &t.handle)) return probe_error("target_import", 7, errno);
            uint32_t handles[4] = {t.handle, 0, 0, 0}, pitches[4] = {pitch, 0, 0, 0}, offsets[4] = {};
            if (drmModeAddFB2(fd, 720, 1280, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &t.fb, 0)) return probe_error("target_framebuffer", 7, errno);
            fprintf(stderr, "NATIVE_KMS_STAGE target=%u size=720x1280 stride=%u modifier=linear PRIME_import=passed fb=%u producer=CPU\n", i, pitch, t.fb);
        }
        if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &blob)) return probe_error("mode_blob", 8, errno);
        if (commit(target[0].fb, true, false)) return 9;
        for (unsigned frame = 0; frame < 6; ++frame) {
            if (commit(target[frame % 3].fb, false, false)) return 10;
            printf("NATIVE_FLIP frame=%u target=%u latch_fence=signaled\n", frame, frame % 3);
            fflush(stdout);
        }
        return 0;
    };
    result = run();
    // An actual commit can have changed state even if its fence fails. Always
    // try restore after that attempt; never pretend an inactive no-op restored it.
    if (attempted) restored = !commit(0, false, true);
    printf("NATIVE_TEST result=%d original_restore=%s producer=CPU GPU_producer_test=false\n",
        result, attempted ? restored ? "confirmed" : "unconfirmed" : "not_needed");
    fflush(stdout);
    // The driver independently pins every submitted backing and quarantines it
    // on failure. Closing client references cannot free an unconfirmed scanout.
    for (auto &t : target) {
        if (t.fb && drmModeRmFB(fd, t.fb)) result = probe_error("cleanup_framebuffer", 12, errno);
        if (t.handle) { drm_gem_close close_req{}; close_req.handle = t.handle; if (drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_req)) result = probe_error("cleanup_handle", 12, errno); }
        if (t.prime >= 0) close(t.prime);
        if (t.bo) gbm_bo_destroy(t.bo);
    }
    if (blob && drmModeDestroyPropertyBlob(fd, blob)) result = probe_error("cleanup_blob", 12, errno);
    if (gbm) gbm_device_destroy(gbm);
    if (render >= 0) close(render);
    return attempted && !restored ? 11 : result;
}
#include "gpu-acquire-test.h"
#include "hwc-client-target-test.h"

int main(int argc, char **argv) {
    bool churn = argc == 2 && !strcmp(argv[1], "--hwc-churn-test");
    bool hwc = churn || (argc == 2 && !strcmp(argv[1], "--hwc-test"));
    if (churn) {
        char value[PROPERTY_VALUE_MAX]{};
        property_get("ro.superman.native_ui_candidate", value, "");
        if (strcmp(value, "p69")) return 2;
    }
    bool gpu = argc == 2 && !strcmp(argv[1], "--gpu-test");
    bool flips = argc == 2 && !strcmp(argv[1], "--flip-test");
    if (!(hwc || gpu || flips || (argc == 2 && !strcmp(argv[1], "--probe"))) || !allowed(flips, gpu, hwc)) {
        fprintf(stderr, "Exact P34 native/stage boot and property, stopped SurfaceFlinger, and explicit command required\n");
        return 2;
    }
    char path[64] = {};
    int fd = display_fd(path, sizeof(path));
    if (fd < 0) return 3;
    if (!version(fd, "superman-fw-kms")) return probe_error("display_version", 3, ENODEV);
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) return probe_error("universal_planes", 3, errno);
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) return probe_error("atomic_capability", 3, errno);
    errno = 0;
    auto *r = drmModeGetResources(fd); auto *ps = drmModeGetPlaneResources(fd);
    fprintf(stderr, "NATIVE_KMS_STAGE resources connectors=%d crtcs=%d planes=%d\n", r ? r->count_connectors : -1, r ? r->count_crtcs : -1, ps ? static_cast<int>(ps->count_planes) : -1);
    if (!r || !ps || r->count_connectors != 1 || r->count_crtcs != 1 || ps->count_planes != 1) return probe_error("resources", 4, errno);
    errno = 0;
    auto *connector = drmModeGetConnector(fd, r->connectors[0]);
    auto *plane = drmModeGetPlane(fd, ps->planes[0]);
    fprintf(stderr, "NATIVE_KMS_STAGE connector_plane connection=%d modes=%d possible_crtcs=0x%x formats=%u\n", connector ? static_cast<int>(connector->connection) : -1, connector ? connector->count_modes : -1, plane ? plane->possible_crtcs : 0, plane ? plane->count_formats : 0);
    if (connector && connector->count_modes > 0) {
        const auto &m = connector->modes[0];
        fprintf(stderr, "NATIVE_KMS_STAGE mode clock=%u h=%u/%u/%u/%u v=%u/%u/%u/%u flags=0x%x\n", m.clock, m.hdisplay, m.hsync_start, m.hsync_end, m.htotal, m.vdisplay, m.vsync_start, m.vsync_end, m.vtotal, m.flags);
    }
    if (!connector || !plane || connector->connection != DRM_MODE_CONNECTED ||
        connector->count_modes != 1 || !mode_ok(connector->modes[0]) ||
        !(plane->possible_crtcs & 1) ||
        std::find(plane->formats, plane->formats + plane->count_formats, DRM_FORMAT_ARGB8888) == plane->formats + plane->count_formats)
        return probe_error("connector_plane_mode", 4, errno);
    uint32_t crtc = r->crtcs[0], conn = connector->connector_id, pid = plane->plane_id;
    uint32_t out = property(fd, crtc, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR");
    uint32_t in = property(fd, pid, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD");
    fprintf(stderr, "NATIVE_KMS_STAGE fences out_property=%u in_property=%u\n", out, in);
    if (!out || !in) return probe_error("fence_properties", 4, 0);
    drmVBlank a{}, b{}; a.request.type = b.request.type = DRM_VBLANK_RELATIVE;
    a.request.sequence = 1; b.request.sequence = 2;
    if (drmWaitVBlank(fd, &a)) return probe_error("vblank_first", 5, errno);
    if (drmWaitVBlank(fd, &b)) return probe_error("vblank_second", 5, errno);
    fprintf(stderr, "NATIVE_KMS_STAGE vblank first=%u last=%u\n", a.reply.sequence, b.reply.sequence);
    if (static_cast<int32_t>(b.reply.sequence - a.reply.sequence) < 2) return probe_error("vblank_progress", 5, 0);
    printf("NATIVE_KMS device=%s real_vblank_first=%u last=%u mode=720x1280@64549\n", path, a.reply.sequence, b.reply.sequence);
    if (hwc) {
        drmModeFreePlane(plane); drmModeFreeConnector(connector);
        drmModeFreePlaneResources(ps); drmModeFreeResources(r);
        close(fd); // HWC must independently acquire the sole display master.
        return hwc_client_target_test(churn);
    }
    if (!flips && !gpu) { close(fd); return 0; }
    int result = gpu ? gpu_flip_test(fd, connector->modes[0], crtc, conn, pid) : flip_test(fd, connector->modes[0], crtc, conn, pid);
    drmModeFreePlane(plane); drmModeFreeConnector(connector);
    drmModeFreePlaneResources(ps); drmModeFreeResources(r);
    close(fd); return result;
}
