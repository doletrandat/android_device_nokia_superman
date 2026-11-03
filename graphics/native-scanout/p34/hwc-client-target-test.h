// SPDX-License-Identifier: Apache-2.0
// Headless HWC2 client-target laboratory. The module and its worker threads
// remain loaded until process exit; never dlclose a running composer.
#include <hardware/hardware.h>
#include <hardware/gralloc.h>
#include <hardware/hwcomposer2.h>
#include <android/gralloc_handle.h>
#include "native-hwc-lab.h"
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

static bool hwc_handle_ok(buffer_handle_t buffer) {
    if (!buffer) return false;
    const auto *h = gralloc_handle(buffer);
    return h->base.version == sizeof(native_handle_t) &&
        h->base.numFds == GRALLOC_HANDLE_NUM_FDS && h->base.numInts == GRALLOC_HANDLE_NUM_INTS &&
        h->magic == GRALLOC_HANDLE_MAGIC && h->version == GRALLOC_HANDLE_VERSION &&
        h->width == 720 && h->height == 1280 && h->format == HAL_PIXEL_FORMAT_BGRA_8888 &&
        (h->usage & GRALLOC_USAGE_HW_FB) && h->modifier == DRM_FORMAT_MOD_LINEAR &&
        h->stride >= 2880 && !(h->stride % 4) && h->stride <= 65535 && h->prime_fd >= 0;
}
static bool hwc_pixels(const gralloc_module_t *module, buffer_handle_t handle, unsigned frame) {
    void *map = nullptr;
    if (module->lock(module, handle, GRALLOC_USAGE_SW_READ_OFTEN, 0, 0, 720, 1280, &map)) return false;
    bool ok = map != nullptr;
    const uint32_t colors[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
    if (ok) for (unsigned y : {64U,640U,1216U}) for (unsigned band = 0; band < 4; ++band) {
        unsigned x = 45 + 180 * band;
        auto *row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(map) + y * gralloc_handle(handle)->stride);
        if (row[x] != colors[(band + frame) % 4]) {
            fprintf(stderr, "NATIVE_HWC_ERROR pixel frame=%u x=%u y=%u actual=%08x\n", frame, x, y, row[x]); ok = false;
        }
    }
    if (module->unlock(module, handle)) ok = false;
    return ok;
}
// Bionic dladdr returns the canonical filename, including /vendor symlinks.
// Compare canonical identities, preserving rejection of different HAL files.
static bool hwc_module_identity(const hw_module_t *module, const char *requested) {
    Dl_info info{}; char wanted[PATH_MAX], loaded[PATH_MAX];
    if (!module || !dladdr(module, &info) || !info.dli_fname) {
        fprintf(stderr, "NATIVE_HWC_ERROR stage=module_address expected=%s\n", requested);
        return false;
    }
    if (!realpath(requested, wanted) || !realpath(info.dli_fname, loaded)) {
        fprintf(stderr, "NATIVE_HWC_ERROR stage=module_realpath expected=%s loaded=%s errno=%d\n", requested, info.dli_fname, errno);
        return false;
    }
    if (strcmp(wanted, loaded)) {
        fprintf(stderr, "NATIVE_HWC_ERROR stage=module_identity expected=%s loaded=%s\n", wanted, loaded);
        return false;
    }
    fprintf(stderr, "NATIVE_HWC_MODULE requested=%s loaded=%s canonical=%s identity=passed\n", requested, info.dli_fname, loaded);
    return true;
}
static int hwc_client_target_test(bool churn = false) {
    const unsigned targets = churn ? 6 : 3, frames = churn ? 12 : 6;
    const hw_module_t *gralloc_base = nullptr, *composer_base = nullptr;
    const gralloc_module_t *gralloc = nullptr;
    alloc_device_t *allocator = nullptr; hwc2_device_t *hwc = nullptr;
    buffer_handle_t allocated[6] = {}; native_handle_t *mapped[6] = {};
    bool registered[6] = {}, reusable[6] = {true,true,true,true,true,true};
    uint64_t backing_device[6] = {}, backing_inode[6] = {};
    unsigned resident = 0, peak_resident = 0, distinct = 0, turnover_releases = 0;
    int acquire = -1, gate_device = -1, gate = -1, merged = -1, present_fence = -1, restore_fence = -1;
    unsigned presents = 0; int previous = -1, result = 0;
    bool attempted = false, restored = false, delayed_pass = false;
    hwc2_layer_t layer = 0; bool layer_created = false;
    GpuProducer gpu;
    using Restore = int(*)(hwc2_device_t*, hwc2_display_t, int32_t*);
    using Status = int(*)(hwc2_device_t*, hwc2_display_t, superman_hwc_lab_state*);
    Restore restore = nullptr; Status status = nullptr;
    superman_hwc_lab_state state{}; state.size = sizeof(state);
#define HWC_FN(type, name, descriptor) type name = nullptr;
    HWC_FN(HWC2_PFN_CREATE_LAYER, create_layer, CREATE_LAYER)
    HWC_FN(HWC2_PFN_DESTROY_LAYER, destroy_layer, DESTROY_LAYER)
    HWC_FN(HWC2_PFN_SET_LAYER_COMPOSITION_TYPE, composition_type, SET_LAYER_COMPOSITION_TYPE)
    HWC_FN(HWC2_PFN_SET_CLIENT_TARGET, set_target, SET_CLIENT_TARGET)
    HWC_FN(HWC2_PFN_VALIDATE_DISPLAY, validate, VALIDATE_DISPLAY)
    HWC_FN(HWC2_PFN_PRESENT_DISPLAY, present, PRESENT_DISPLAY)
    HWC_FN(HWC2_PFN_GET_RELEASE_FENCES, release, GET_RELEASE_FENCES)
#undef HWC_FN
    auto run = [&]() -> int {
        int module_status = hw_get_module_by_class(GRALLOC_HARDWARE_MODULE_ID, nullptr, &gralloc_base);
        if (module_status || !gralloc_base || !gralloc_base->dso) {
            fprintf(stderr, "NATIVE_HWC_ERROR stage=gralloc_load status=%d\n", module_status); return 20;
        }
        if (!hwc_module_identity(gralloc_base, "/vendor/lib/hw/gralloc.superman_native.so")) return 20;
        gralloc = reinterpret_cast<const gralloc_module_t*>(gralloc_base);
        int allocator_status = gralloc_open(gralloc_base, &allocator);
        if (allocator_status || !allocator) {
            fprintf(stderr, "NATIVE_HWC_ERROR stage=allocator_open status=%d errno=%d\n", allocator_status, errno); return 20;
        }
        if (!gralloc->registerBuffer || !gralloc->unregisterBuffer || !gralloc->lock || !gralloc->unlock) {
            fprintf(stderr, "NATIVE_HWC_ERROR stage=mapper_callbacks\n"); return 20;
        }
        if (!gpu.initialize()) { fprintf(stderr, "NATIVE_HWC_ERROR stage=gpu_initialize\n"); return 20; }
        auto allocate_target = [&](unsigned i) -> bool {
            int stride = 0;
            errno = 0;
            int allocation = allocator->alloc(allocator, 720, 1280, HAL_PIXEL_FORMAT_BGRA_8888,
                GRALLOC_USAGE_HW_FB | GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_SW_READ_OFTEN, &allocated[i], &stride);
            if (allocation || !hwc_handle_ok(allocated[i])) {
                fprintf(stderr, "NATIVE_HWC_ERROR stage=target_allocate target=%u status=%d errno=%d resident=%u peak=%u\n",
                    i, allocation, errno, resident, peak_resident); return false;
            }
            ++resident;
            peak_resident = std::max(peak_resident, resident);
            if (resident > 3) return false;
            struct stat backing{};
            if (fstat(gralloc_handle(allocated[i])->prime_fd, &backing) || !backing.st_ino) {
                fprintf(stderr, "NATIVE_HWC_ERROR stage=backing_identity target=%u errno=%d\n", i, errno); return false;
            }
            for (unsigned old = 0; old < i; ++old)
                if (backing_device[old] == uint64_t(backing.st_dev) && backing_inode[old] == uint64_t(backing.st_ino)) {
                    fprintf(stderr, "NATIVE_HWC_ERROR stage=repeated_backing target=%u prior=%u\n", i, old); return false;
                }
            backing_device[i] = backing.st_dev; backing_inode[i] = backing.st_ino; ++distinct;
            mapped[i] = native_handle_clone(allocated[i]);
            if (!mapped[i] || gralloc->registerBuffer(gralloc, mapped[i])) {
                fprintf(stderr, "NATIVE_HWC_ERROR stage=target_register target=%u errno=%d\n", i, errno); return false;
            }
            registered[i] = true;
            if (!hwc_handle_ok(mapped[i]) || !gpu.import(i, gralloc_handle(mapped[i])->prime_fd, gralloc_handle(mapped[i])->stride)) {
                fprintf(stderr, "NATIVE_HWC_ERROR stage=target_GPU_import target=%u errno=%d\n", i, errno); return false;
            }
            fprintf(stderr, "NATIVE_HWC_TARGET target=%u native_handle=cloned_registered stride=%u allocator_stride=%d FBO=complete producer=GPU\n", i, gralloc_handle(mapped[i])->stride, stride);
            fprintf(stderr, "NATIVE_HWC_BACKING target=%u device=%llu inode=%llu resident=%u peak=%u\n", i,
                static_cast<unsigned long long>(backing_device[i]), static_cast<unsigned long long>(backing_inode[i]), resident, peak_resident);
            return true;
        };
        // Three 4 MiB rounded scanout BOs fit the inherited 16 MiB VRAM pool.
        // Cumulative turnover must not require all six backings simultaneously.
        for (unsigned i = 0; i < 3; ++i) {
            if (!allocate_target(i)) return 21;
        }
        module_status = hw_get_module_by_class(HWC_HARDWARE_MODULE_ID, nullptr, &composer_base);
        if (module_status || !composer_base || !composer_base->dso) {
            fprintf(stderr, "NATIVE_HWC_ERROR stage=composer_load status=%d\n", module_status); return 22;
        }
        if (!hwc_module_identity(composer_base, "/vendor/lib/hw/hwcomposer.superman_native.so")) return 22;
        // These symbols are private laboratory gates, not HWC2 power callbacks.
        restore = reinterpret_cast<Restore>(dlsym(composer_base->dso, "superman_native_hwc_restore"));
        status = reinterpret_cast<Status>(dlsym(composer_base->dso, "superman_native_hwc_status"));
        if (!restore || !status) return 22;
        if (composer_base->methods->open(composer_base, HWC_HARDWARE_COMPOSER, reinterpret_cast<hw_device_t**>(&hwc)) || !hwc ||
            hwc->common.version != HWC_DEVICE_API_VERSION_2_0) return 22;
#define HWC_FN(type, name, descriptor) name = reinterpret_cast<type>(hwc->getFunction(hwc, HWC2_FUNCTION_##descriptor)); if (!name) return 22;
        HWC_FN(HWC2_PFN_CREATE_LAYER, create_layer, CREATE_LAYER)
        HWC_FN(HWC2_PFN_DESTROY_LAYER, destroy_layer, DESTROY_LAYER)
        HWC_FN(HWC2_PFN_SET_LAYER_COMPOSITION_TYPE, composition_type, SET_LAYER_COMPOSITION_TYPE)
        HWC_FN(HWC2_PFN_SET_CLIENT_TARGET, set_target, SET_CLIENT_TARGET)
        HWC_FN(HWC2_PFN_VALIDATE_DISPLAY, validate, VALIDATE_DISPLAY)
        HWC_FN(HWC2_PFN_PRESENT_DISPLAY, present, PRESENT_DISPLAY)
        HWC_FN(HWC2_PFN_GET_RELEASE_FENCES, release, GET_RELEASE_FENCES)
#undef HWC_FN
        if (status(hwc, 0, &state) || state.live_attempts || state.active_composition) return 22;
        if (create_layer(hwc, 0, &layer)) return 22;
        layer_created = true;
        if (composition_type(hwc, 0, layer, HWC2_COMPOSITION_CLIENT)) return 22;
        if (!churn) gate_device = open("/dev/superman-acquire-gate", O_RDWR | O_CLOEXEC);
        if (gate_device < 0 && !churn) return 23;
        hwc_region_t damage{};
        for (unsigned frame = 0; frame < frames; ++frame) {
            unsigned i = churn && frame >= 6 ? 3 + (frame - 6) % 3 : frame % 3;
            if (churn && frame >= 6 && frame < 9) {
                unsigned old = i - 3;
                // The preceding signaled present and exact retirement counts
                // establish that this old target is no longer scanned out.
                if (!reusable[old] || previous == int(old) || presents < 6 ||
                    status(hwc, 0, &state) || state.live_attempts != presents ||
                    state.latched_frames != presents || state.retired_compositions != presents - 1 ||
                    !state.active_composition) return 27;
                if (!gpu.release(old) || gralloc->unregisterBuffer(gralloc, mapped[old])) return 28;
                registered[old] = false;
                if (native_handle_close(mapped[old])) return 28;
                native_handle_delete(mapped[old]); mapped[old] = nullptr;
                if (allocator->free(allocator, allocated[old])) return 28;
                allocated[old] = nullptr; --resident; ++turnover_releases;
                fprintf(stderr, "NATIVE_HWC_TURNOVER retired_target=%u later_latch=%u resident=%u release=confirmed\n", old, presents, resident);
                if (!allocate_target(i)) return 21;
            }
            // Current-target present fence alone never permits its reuse.
            if (!reusable[i]) return 24;
            acquire = gpu.draw(i, frame);
            if (acquire < 0 || sync_wait(acquire, 1000) || !hwc_pixels(gralloc, mapped[i], frame)) return 24;
            fprintf(stderr, "NATIVE_HWC_DRAW frame=%u target=%u pixels=12-pass GPU_fence=signaled CPU_writes=false\n", frame, i);
            uint32_t types = 0, requests = 0;
            if (validate(hwc, 0, &types, &requests) || types || requests) return 24;
            int input = acquire; double created = 0;
            if (frame == 0 && !churn) {
                superman_acquire_gate request{SUPERMAN_GATE_DELAY_MS, -1};
                if (ioctl(gate_device, SUPERMAN_ACQUIRE_GATE, &request)) return 25;
                gate = request.fence_fd; created = gpu_now_ms();
                if (gate < 0 || (merged = sync_merge("superman-hwc-gate", acquire, gate)) < 0) return 25;
                input = merged;
            }
            int owned = dup(input); if (owned < 0) return 25;
            // setClientTarget consumes the passed FD, including its error path.
            if (set_target(hwc, 0, mapped[i], owned, HAL_DATASPACE_UNKNOWN, damage)) return 25;
            if (frame == 0 && !churn && (!sync_wait(gate, 0) || errno != ETIME || !sync_wait(merged, 0) || errno != ETIME || gpu_now_ms() - created > 50)) return 25;
            double begin = gpu_now_ms(); attempted = true;
            int error = present(hwc, 0, &present_fence); double elapsed = gpu_now_ms() - begin;
            if (error || present_fence < 0 || sync_wait(present_fence, 1000)) return 26;
            close(present_fence); present_fence = -1;
            if (frame == 0 && !churn) {
                delayed_pass = elapsed >= 350 && !sync_wait(gate, 0) && !sync_wait(merged, 0);
                fprintf(stderr, "NATIVE_HWC_ACQUIRE delayed_gpu_plus_software_gate=%s present_ms=%.3f software_delay_ms=500 gpu_latency_claim=false\n", delayed_pass ? "passed" : "failed", elapsed);
                if (!delayed_pass) return 26;
            }
            if (previous >= 0) reusable[previous] = true;
            reusable[i] = false; previous = i; ++presents;
            if (status(hwc, 0, &state) || state.live_attempts != presents || state.latched_frames != presents ||
                state.explicit_acquire_inputs != presents || state.retired_compositions != presents - 1 || !state.active_composition) return 27;
            uint32_t count = 0;
            if (release(hwc, 0, &count, nullptr, nullptr) || count != 1) return 27;
            hwc2_layer_t released = 0; int32_t release_fence = -1;
            if (release(hwc, 0, &count, &released, &release_fence)) { if (release_fence >= 0) close(release_fence); return 27; }
            if (release_fence >= 0) { close(release_fence); return 27; }
            if (count != 1 || released != layer) return 27;
            fprintf(stderr, "NATIVE_HWC_PRESENT frame=%u target=%u acquire=explicit present_fence=signaled retired=%u client_layer_release=none previous_target_reuse=later_latch\n", frame, i, state.retired_compositions);
            close(acquire); acquire = -1;
            if (merged >= 0) { close(merged); merged = -1; }
            if (gate >= 0) { close(gate); gate = -1; }
        }
        if (distinct != targets || peak_resident != 3 || (churn && turnover_releases != 3)) return 27;
        if (churn) fprintf(stderr, "NATIVE_HWC_CHURN_LIFETIME distinct_backings=%u peak_resident=%u retired_releases=%u presents=%u\n",
            distinct, peak_resident, turnover_releases, presents);
        return 0;
    };
    result = run();
    if (hwc && status && !status(hwc, 0, &state) && state.live_attempts) attempted = true;
    if (attempted && hwc && restore) {
        restored = !restore(hwc, 0, &restore_fence) && restore_fence >= 0 && !sync_wait(restore_fence, 1000);
        if (restored && (!status || status(hwc, 0, &state) || !state.restore_confirmed || state.active_composition ||
            (!result && state.retired_compositions != frames))) restored = false;
    }
    for (int fd : {acquire, gate, merged, gate_device, present_fence, restore_fence}) if (fd >= 0) close(fd);
    if (!gpu.cleanup()) result = 28;
    if (!attempted || restored) {
        if (layer_created && destroy_layer(hwc, 0, layer)) result = 28;
        for (unsigned i = 0; i < targets; ++i) {
            if (registered[i] && gralloc->unregisterBuffer(gralloc, mapped[i])) result = 28;
            if (mapped[i]) { if (native_handle_close(mapped[i])) result = 28; native_handle_delete(mapped[i]); }
            if (allocated[i] && allocator->free(allocator, allocated[i])) result = 28;
        }
        if (allocator && gralloc_close(allocator)) result = 28;
    } else {
        fprintf(stderr, "NATIVE_HWC_ERROR restore=unconfirmed client_references_retained_until_process_exit driver_pins_quarantined\n");
        result = 11;
    }
    // Deliberately retain HWC and both module DSOs with their worker threads.
    if (gpu.library && dlclose(gpu.library)) result = 28;
    if (churn) printf("NATIVE_HWC_CHURN result=%d targets=%u presents=%u retired=%u original_restore=%s explicit_fences=true UI_held=true\n", result, targets, presents, state.retired_compositions, restored ? "confirmed" : "unconfirmed");
    else printf("NATIVE_HWC_TEST result=%d presents=%u retired=%u original_restore=%s delayed_gate=%s UI_held=true\n",
        result, presents, state.retired_compositions, attempted ? restored ? "confirmed" : "unconfirmed" : "not_needed", delayed_pass ? "passed" : "failed");
    fflush(stdout); return result;
}
