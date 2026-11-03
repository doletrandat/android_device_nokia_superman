/*
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 * Copyright (C) 2016 Linaro, Ltd., Rob Herring <robh@kernel.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#define LOG_TAG "GRALLOC-GBM"

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

#include <hardware/gralloc.h>
#include <system/graphics.h>

#include <gbm.h>

#include "gralloc_gbm_priv.h"
#include <android/gralloc_handle.h>

#include <unordered_map>

#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#define unlikely(x) __builtin_expect(!!(x), 0)

static std::unordered_map<buffer_handle_t, struct gbm_bo *> gbm_bo_handle_map;

struct bo_data_t {
	void *map_data;
	int lock_count;
	int locked_for;
};

void gralloc_gbm_destroy_user_data(struct gbm_bo *bo, void *data)
{
	struct bo_data_t *bo_data = (struct bo_data_t *)data;
	delete bo_data;

	(void)bo;
}

static struct bo_data_t *gbm_bo_data(struct gbm_bo *bo) {
	return (struct bo_data_t *)gbm_bo_get_user_data(bo);
}


static uint32_t get_gbm_format(int format)
{
	uint32_t fmt;

	switch (format) {
	case HAL_PIXEL_FORMAT_RGBA_8888:
		fmt = GBM_FORMAT_ABGR8888;
		break;
	case HAL_PIXEL_FORMAT_RGBX_8888:
		fmt = GBM_FORMAT_XBGR8888;
		break;
	case HAL_PIXEL_FORMAT_RGB_888:
		fmt = GBM_FORMAT_RGB888;
		break;
	case HAL_PIXEL_FORMAT_RGB_565:
		fmt = GBM_FORMAT_RGB565;
		break;
	case HAL_PIXEL_FORMAT_BGRA_8888:
		fmt = GBM_FORMAT_ARGB8888;
		break;
	case HAL_PIXEL_FORMAT_YV12:
		/* YV12 is planar, but must be a single buffer so ask for GR88 */
		fmt = GBM_FORMAT_GR88;
		break;
	case HAL_PIXEL_FORMAT_YCbCr_422_SP:
	case HAL_PIXEL_FORMAT_YCrCb_420_SP:
	default:
		fmt = 0;
		break;
	}

	return fmt;
}

static int gralloc_gbm_get_bpp(int format)
{
	int bpp;

	switch (format) {
	case HAL_PIXEL_FORMAT_RGBA_8888:
	case HAL_PIXEL_FORMAT_RGBX_8888:
	case HAL_PIXEL_FORMAT_BGRA_8888:
		bpp = 4;
		break;
	case HAL_PIXEL_FORMAT_RGB_888:
		bpp = 3;
		break;
	case HAL_PIXEL_FORMAT_RGB_565:
	case HAL_PIXEL_FORMAT_YCbCr_422_I:
		bpp = 2;
		break;
	/* planar; only Y is considered */
	case HAL_PIXEL_FORMAT_YV12:
	case HAL_PIXEL_FORMAT_YCbCr_422_SP:
	case HAL_PIXEL_FORMAT_YCrCb_420_SP:
		bpp = 1;
		break;
	default:
		bpp = 0;
		break;
	}

	return bpp;
}

static unsigned int get_pipe_bind(int usage)
{
	unsigned int bind = 0;

	if (usage & (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN))
		bind |= GBM_BO_USE_LINEAR;
	if (usage & GRALLOC_USAGE_CURSOR)
		;//bind |= GBM_BO_USE_CURSOR;
	if (usage & (GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE))
		bind |= GBM_BO_USE_RENDERING;
	if (usage & GRALLOC_USAGE_HW_FB)
		bind |= GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING;
	if (usage & GRALLOC_USAGE_HW_COMPOSER)
		bind |= GBM_BO_USE_RENDERING;

	return bind;
}

static struct gbm_bo *gbm_import(struct gbm_device *gbm,
		buffer_handle_t _handle)
{
	struct gbm_bo *bo;
	struct gralloc_handle_t *handle = gralloc_handle(_handle);
	#ifdef GBM_BO_IMPORT_FD_MODIFIER
	struct gbm_import_fd_modifier_data data;
	#else
	struct gbm_import_fd_data data;
	#endif

	int format = get_gbm_format(handle->format);
	if (handle->prime_fd < 0)
		return NULL;

	memset(&data, 0, sizeof(data));
	data.width = handle->width;
	data.height = handle->height;
	data.format = format;
	/* Adjust the width and height for a GBM GR88 buffer */
	if (handle->format == HAL_PIXEL_FORMAT_YV12) {
		data.width /= 2;
		data.height += handle->height / 2;
	}

	#ifdef GBM_BO_IMPORT_FD_MODIFIER
	data.num_fds = 1;
	data.fds[0] = handle->prime_fd;
	data.strides[0] = handle->stride;
	data.modifier = handle->modifier;
	bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &data, 0);
	#else
	data.fd = handle->prime_fd;
	data.stride = handle->stride;
	bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &data, 0);
	#endif

	return bo;
}

static struct gbm_bo *gbm_alloc(struct gbm_device *gbm,
		buffer_handle_t _handle)
{
	struct gbm_bo *bo;
	struct gralloc_handle_t *handle = gralloc_handle(_handle);
	int format = get_gbm_format(handle->format);
	int usage = get_pipe_bind(handle->usage);
	int width, height;

	width = handle->width;
	height = handle->height;
	if (usage & GBM_BO_USE_CURSOR) {
		if (handle->width < 64)
			width = 64;
		if (handle->height < 64)
			height = 64;
	}

	/*
	 * For YV12, we request GR88, so halve the width since we're getting
	 * 16bpp. Then increase the height by 1.5 for the U and V planes.
	 */
	if (handle->format == HAL_PIXEL_FORMAT_YV12) {
		width /= 2;
		height += handle->height / 2;
	}

	ALOGV("create BO, size=%dx%d, fmt=%d, usage=%x",
	      handle->width, handle->height, handle->format, usage);
	bo = gbm_bo_create(gbm, width, height, format, usage);
	if (!bo) {
		ALOGE("failed to create BO, size=%dx%d, fmt=%d, usage=%x",
		      handle->width, handle->height, handle->format, usage);
		return NULL;
	}

	handle->prime_fd = gbm_bo_get_fd(bo);
	if (handle->prime_fd < 0) { gbm_bo_destroy(bo); return nullptr; }
    handle->stride = gbm_bo_get_stride(bo);
	#ifdef GBM_BO_IMPORT_FD_MODIFIER
	handle->modifier = gbm_bo_get_modifier(bo);
	#endif

	return bo;
}

// Validate the native header before reading allocator-specific fields.
static bool valid_handle(buffer_handle_t handle)
{
    if (!handle || handle->version != sizeof(native_handle_t) ||
        handle->numFds != GRALLOC_HANDLE_NUM_FDS ||
        handle->numInts != GRALLOC_HANDLE_NUM_INTS) return false;
    const auto *h = gralloc_handle(handle);
    const int bpp = gralloc_gbm_get_bpp(h->format);
    return h->magic == GRALLOC_HANDLE_MAGIC && h->version == GRALLOC_HANDLE_VERSION &&
        h->prime_fd >= 0 && fcntl(h->prime_fd, F_GETFD) >= 0 &&
        h->width > 0 && h->height > 0 && h->width <= 16384 && h->height <= 16384 &&
        get_gbm_format(h->format) && bpp > 0 && h->stride >= h->width * bpp &&
        h->stride % bpp == 0 && uint64_t(h->stride) * h->height <= SIZE_MAX &&
        (h->format != HAL_PIXEL_FORMAT_YV12 || !(h->width % 2 || h->height % 2));
}

int gbm_free(buffer_handle_t handle)
{
    auto found = gbm_bo_handle_map.find(handle);
    if (found == gbm_bo_handle_map.end()) return -EINVAL;
    const auto *data = gbm_bo_data(found->second);
    if (data && data->lock_count) return -EBUSY;
    struct gbm_bo *bo = found->second;
    gbm_bo_handle_map.erase(found);
    gbm_bo_destroy(bo);
    return 0;
}

/* Unknown handles must not insert null objects into the registry. */
struct gbm_bo *gralloc_gbm_bo_from_handle(buffer_handle_t handle)
{
    auto found = gbm_bo_handle_map.find(handle);
    return found == gbm_bo_handle_map.end() ? nullptr : found->second;
}

static int gbm_map(buffer_handle_t handle, int x, int y, int w, int h,
		int enable_write, void **addr)
{
	int err = 0;
	int flags = GBM_BO_TRANSFER_READ;
	struct gralloc_gbm_handle_t *gbm_handle = gralloc_handle(handle);
	struct gbm_bo *bo = gralloc_gbm_bo_from_handle(handle);
	struct bo_data_t *bo_data = gbm_bo_data(bo);
	uint32_t stride;

	if (bo_data->map_data)
		return -EINVAL;

	if (gbm_handle->format == HAL_PIXEL_FORMAT_YV12) {
		if (x || y)
			ALOGE("can't map with offset for planar %p", bo);
		w /= 2;
		h += h / 2;
	}

	if (enable_write)
		flags |= GBM_BO_TRANSFER_WRITE;

	*addr = gbm_bo_map(bo, 0, 0, x + w, y + h, flags, &stride, &bo_data->map_data);
	ALOGV("mapped bo %p (%d, %d)-(%d, %d) at %p", bo, x, y, w, h, *addr);
	if (*addr == NULL)
		return -ENOMEM;

	if (stride != gbm_bo_get_stride(bo) || stride != gbm_handle->stride) {
        gbm_bo_unmap(bo, bo_data->map_data);
        bo_data->map_data = nullptr;
        *addr = nullptr;
        return -EINVAL;
    }

	return err;
}

static void gbm_unmap(struct gbm_bo *bo)
{
	struct bo_data_t *bo_data = gbm_bo_data(bo);

	gbm_bo_unmap(bo, bo_data->map_data);
	bo_data->map_data = NULL;
}

void gbm_dev_destroy(struct gbm_device *gbm)
{
	int fd = gbm_device_get_fd(gbm);

	gbm_device_destroy(gbm);
	close(fd);
}

struct gbm_device *gbm_dev_create(void)
{
	struct gbm_device *gbm;
	char path[PROPERTY_VALUE_MAX];
	int fd;

	property_get("gralloc.gbm.device", path, "/dev/dri/renderD128");
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ALOGE("failed to open %s", path);
		return NULL;
	}

	gbm = gbm_create_device(fd);
	if (!gbm) {
		ALOGE("failed to create gbm device");
		close(fd);
	}

	return gbm;
}

/*
 * Register a buffer handle.
 */
int gralloc_gbm_handle_register(buffer_handle_t _handle, struct gbm_device *gbm)
{
	struct gbm_bo *bo;

	if (!valid_handle(_handle))
		return -EINVAL;

	if (gbm_bo_handle_map.count(_handle))
		return -EINVAL;

	bo = gbm_import(gbm, _handle);
	if (!bo)
		return -EINVAL;

	// Imported metadata must agree with the actual GBM layout.
    const auto *h = gralloc_handle(_handle);
    if (gbm_bo_get_stride(bo) != h->stride) {
        gbm_bo_destroy(bo);
        return -EINVAL;
    }
    gbm_bo_handle_map.emplace(_handle, bo);

	return 0;
}

/*
 * Unregister a buffer handle.  It is no-op for handles created locally.
 */
int gralloc_gbm_handle_unregister(buffer_handle_t handle)
{
	return gbm_free(handle);
}

/*
 * Create a bo.
 */
buffer_handle_t gralloc_gbm_bo_create(struct gbm_device *gbm,
		int width, int height, int format, int usage, int *stride)
{
	struct gbm_bo *bo;
	native_handle_t *handle;

	if (!stride || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        !get_gbm_format(format) || (format == HAL_PIXEL_FORMAT_YV12 && (width % 2 || height % 2)) ||
        (usage & GRALLOC_USAGE_PROTECTED)) { errno = EINVAL; return nullptr; }
    handle = gralloc_handle_create(width, height, format, usage);
	if (!handle)
		return NULL;

	gralloc_handle(handle)->data_owner = 0;
    gralloc_handle(handle)->reserved = 0;
    gralloc_handle(handle)->modifier = 0;
    errno = 0;
    bo = gbm_alloc(gbm, handle);
	if (!bo) {
		if (!errno) errno = ENOMEM;
		native_handle_delete(handle);
		return NULL;
	}

	gbm_bo_handle_map.emplace(handle, bo);

	/* in pixels */
	struct gralloc_handle_t *ghandle = gralloc_handle(handle);
	*stride = ghandle->stride / gralloc_gbm_get_bpp(format);

	return handle;
}

/*
 * Lock a bo.  XXX thread-safety?
 */
int gralloc_gbm_bo_lock(buffer_handle_t handle,
		int usage, int x, int y, int w, int h,
		void **addr)
{
	if (!addr) return -EINVAL;
    *addr = nullptr;
    // The module mutex serializes this bounded diagnostic counter.
    static unsigned rejectedLocks = 0;
    auto reject = [&](const char* stage, int error) {
        if (rejectedLocks++ < 24) {
            ALOGE("SUPERMAN_GRALLOC lock_failed stage=%s error=%d usage=0x%x region=%d,%d,%d,%d",
                  stage, error, usage, x, y, w, h);
        }
        return error;
    };
    if (!valid_handle(handle)) return reject("handle", -EINVAL);
    // IMapper 2.0: an accessRegion of all zeros means the entire buffer.
    // Gralloc0Hal forwards that rectangle unchanged to this legacy lock.
    if (x == 0 && y == 0 && w == 0 && h == 0) {
        w = gralloc_handle(handle)->width;
        h = gralloc_handle(handle)->height;
    }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        uint64_t(x) + w > gralloc_handle(handle)->width ||
        uint64_t(y) + h > gralloc_handle(handle)->height) return reject("region", -EINVAL);
    struct gralloc_handle_t *gbm_handle = gralloc_handle(handle);
    struct gbm_bo *bo = gralloc_gbm_bo_from_handle(handle);
    struct bo_data_t *bo_data;

	if (!bo)
		return reject("not_registered", -EINVAL);

	if ((gbm_handle->usage & usage) != (uint32_t)usage) {
		/* make FB special for testing software renderer with */

		if (!(gbm_handle->usage & GRALLOC_USAGE_SW_READ_OFTEN) &&
				!(gbm_handle->usage & GRALLOC_USAGE_HW_FB) &&
				!(gbm_handle->usage & GRALLOC_USAGE_HW_TEXTURE)) {
			ALOGE("bo.usage:x%X/usage:x%X is not GRALLOC_USAGE_HW_FB or GRALLOC_USAGE_HW_TEXTURE",
				gbm_handle->usage, usage);
			return reject("usage", -EINVAL);
		}
	}

	bo_data = gbm_bo_data(bo);
	if (!bo_data) {
		bo_data = new struct bo_data_t();
		gbm_bo_set_user_data(bo, bo_data, gralloc_gbm_destroy_user_data);
	}

	ALOGV("lock bo %p, cnt=%d, usage=%x", bo, bo_data->lock_count, usage);

	// A nested map cannot provide a second independently owned mapping.
    if (bo_data->lock_count) return reject("already_locked", -EBUSY);

	usage |= bo_data->locked_for;

	if (usage & (GRALLOC_USAGE_SW_WRITE_MASK |
		     GRALLOC_USAGE_SW_READ_MASK)) {
		/* the driver is supposed to wait for the bo */
		int write = !!(usage & GRALLOC_USAGE_SW_WRITE_MASK);
		int err = gbm_map(handle, x, y, w, h, write, addr);
		if (err)
			return reject("map", err);
	}
	else {
		/* kernel handles the synchronization here */
	}

	bo_data->lock_count++;
	bo_data->locked_for |= usage;

	return 0;
}

/*
 * Unlock a bo.
 */
int gralloc_gbm_bo_unlock(buffer_handle_t handle)
{
	struct gbm_bo *bo = gralloc_gbm_bo_from_handle(handle);
	struct bo_data_t *bo_data;
	if (!bo)
		return -EINVAL;

	bo_data = gbm_bo_data(bo);
    if (!bo_data || !bo_data->lock_count) return -EINVAL;

	int mapped = bo_data->locked_for &
		(GRALLOC_USAGE_SW_WRITE_MASK | GRALLOC_USAGE_SW_READ_MASK);

	if (!bo_data->lock_count)
		return 0;

	if (mapped)
		gbm_unmap(bo);

	bo_data->lock_count--;
	if (!bo_data->lock_count)
		bo_data->locked_for = 0;

	return 0;
}
