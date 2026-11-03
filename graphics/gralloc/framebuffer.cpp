// SPDX-License-Identifier: Apache-2.0
// Bridge an explicitly selected DRM allocator to the preserved simplefb output.
// No modesetting, panel programming, GPU clock changes or framebuffer panning.
#include <hardware/gralloc.h>
#include <hardware/fb.h>
#include <android/gralloc_handle.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>
#include <chrono>
#include <cutils/properties.h>
#include <log/log.h>
#include "copy_rows.h"

struct SupermanFramebuffer {
    framebuffer_device_t device{};
    int fd = -1;
    uint8_t* pixels = nullptr;
    size_t length = 0;
    size_t line = 0;
    size_t offset = 0;
    bool timingEnabled = false;
    unsigned timingPosts = 0;
    unsigned rejectedPosts = 0;
};

static int post(framebuffer_device_t* device, buffer_handle_t buffer) {
    auto* fb = reinterpret_cast<SupermanFramebuffer*>(device);
    auto reject = [&](const char* stage, int error) {
        if (fb->rejectedPosts++ < 16) {
            ALOGE("SUPERMAN_FB rejected stage=%s error=%d display=%dx%d required_format=%d",
                  stage, error, device->width, device->height, device->format);
        }
        return error;
    };
    if (!buffer || buffer->version != static_cast<int>(sizeof(native_handle_t)) || buffer->numFds != GRALLOC_HANDLE_NUM_FDS ||
        buffer->numInts != GRALLOC_HANDLE_NUM_INTS) return reject("native_header", -EINVAL);
    const auto* handle = gralloc_handle(buffer);
    if (handle->magic != GRALLOC_HANDLE_MAGIC || handle->version != GRALLOC_HANDLE_VERSION ||
        handle->width != device->width || handle->height != device->height ||
        handle->format != static_cast<uint32_t>(device->format) ||
        handle->stride < device->width * 4 || handle->prime_fd < 0 ||
        uint64_t(handle->height) * handle->stride > SIZE_MAX) {
        if (fb->rejectedPosts < 16) ALOGE("SUPERMAN_FB buffer=%ux%u format=%u stride_bytes=%u prime_fd=%d",
            handle->width, handle->height, handle->format, handle->stride, handle->prime_fd);
        return reject("buffer_layout", -EINVAL);
    }
    auto* module = reinterpret_cast<const gralloc_module_t*>(device->common.module);
    void* source = nullptr;
    // HWC2OnFbAdapter waits on the client-target acquire fence before post().
    const bool timing = fb->timingEnabled && fb->timingPosts < 32;
    const auto begin = timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    int error = module->lock(module, buffer, GRALLOC_USAGE_SW_READ_OFTEN,
                             0, 0, device->width, device->height, &source);
    if (error) return reject("cpu_lock", error);
    if (!source) { module->unlock(module, buffer); return reject("null_mapping", -EFAULT); }
    const auto copyStart = timing ? std::chrono::steady_clock::now() : begin;
    superman_copy_rows(fb->pixels + fb->offset, fb->line,
                       static_cast<const uint8_t*>(source), handle->stride,
                       size_t(device->width) * 4, device->height);
    const auto copyEnd = timing ? std::chrono::steady_clock::now() : begin;
    error = module->unlock(module, buffer);
    if (timing) {
        const auto end = std::chrono::steady_clock::now();
        auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        ALOGI("SUPERMAN_FB post=%u map_ms=%.3f copy_ms=%.3f unlock_ms=%.3f error=%d",
              fb->timingPosts++, ms(begin, copyStart), ms(copyStart, copyEnd), ms(copyEnd, end), error);
    }
    return error ? reject("cpu_unlock", error) : 0;
}

static int interval(framebuffer_device_t*, int value) {
    return value >= 0 && value <= 1 ? 0 : -EINVAL;
}

static int closeDevice(hw_device_t* device) {
    auto* fb = reinterpret_cast<SupermanFramebuffer*>(device);
    if (fb->pixels) munmap(fb->pixels, fb->length);
    if (fb->fd >= 0) close(fb->fd);
    delete fb;
    return 0;
}

int superman_fb_open(const hw_module_t* module, hw_device_t** output) {
    auto* fb = new (std::nothrow) SupermanFramebuffer;
    if (!fb) return -ENOMEM;
    fb->fd = open("/dev/graphics/fb0", O_RDWR | O_CLOEXEC);
    if (fb->fd < 0) { int error = -errno; delete fb; return error; }
    fb_fix_screeninfo fix{};
    fb_var_screeninfo var{};
    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) ||
        ioctl(fb->fd, FBIOGET_VSCREENINFO, &var)) {
        int error = -errno; closeDevice(&fb->device.common); return error;
    }
    // Only the existing packed BGRA-compatible 32-bit simplefb layout is supported.
    uint64_t offset = uint64_t(var.yoffset) * fix.line_length + uint64_t(var.xoffset) * 4;
    uint64_t row = uint64_t(var.xres) * 4;
    uint64_t end = offset + uint64_t(var.yres ? var.yres - 1 : 0) * fix.line_length + row;
    if (!var.xres || !var.yres || var.xres > 16384 || var.yres > 16384 ||
        var.bits_per_pixel != 32 || var.red.offset != 16 || var.green.offset != 8 ||
        var.blue.offset != 0 || var.red.length != 8 || var.green.length != 8 ||
        var.blue.length != 8 || var.nonstd || var.grayscale ||
        fix.type != FB_TYPE_PACKED_PIXELS || fix.line_length > INT32_MAX ||
        uint64_t(var.xoffset) * 4 + row > fix.line_length ||
        end > fix.smem_len) { closeDevice(&fb->device.common); return -ENOTSUP; }
    void* pixels = mmap(nullptr, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (pixels == MAP_FAILED) { int error = -errno; closeDevice(&fb->device.common); return error; }
    fb->pixels = static_cast<uint8_t*>(pixels);
    fb->length = fix.smem_len;
    fb->line = fix.line_length;
    fb->offset = offset;
    fb->timingEnabled = property_get_bool("debug.superman.fb_timing", false);
    // HAL API declares its display properties const; initialize a complete descriptor.
    framebuffer_device_t descriptor = {
        .common = {.tag = HARDWARE_DEVICE_TAG, .version = 0,
                   .module = const_cast<hw_module_t*>(module), .close = closeDevice},
        .flags = 0,
        .width = var.xres, .height = var.yres, .stride = static_cast<int>(fix.line_length / 4),
        .format = HAL_PIXEL_FORMAT_BGRA_8888,
        .xdpi = var.width ? float(var.xres) * 25.4f / var.width : 320.0f,
        .ydpi = var.height ? float(var.yres) * 25.4f / var.height : 320.0f,
        .fps = 60.0f, .minSwapInterval = 0, .maxSwapInterval = 1,
        .numFramebuffers = 1,
        .setSwapInterval = interval, .post = post,
    };
    std::memcpy(&fb->device, &descriptor, sizeof(descriptor));
    *output = &fb->device.common;
    return 0;
}
