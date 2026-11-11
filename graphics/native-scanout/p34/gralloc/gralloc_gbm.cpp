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
#include <xf86drm.h>

#include "gralloc_gbm_priv.h"
#include <android/gralloc_handle.h>

#include <unordered_map>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include "../../p69/gralloc-metadata.h"

#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#define unlikely(x) __builtin_expect(!!(x), 0)

/*
 * HAL_PIXEL_FORMAT_BLOB (Codec2 C2AllocatorBlob linear blocks, width = bytes,
 * height = 1) is stored as a linear R8 GBM buffer of rows of at most
 * kBlobPitch bytes. The row pitch must equal the row width so the bytes are
 * contiguous; allocation fails otherwise.
 */
static const uint32_t kBlobPitch = 4096;
static const uint32_t kBlobMaxBytes = kBlobPitch * 4096;

static bool blob_shape(uint32_t bytes, uint32_t *w, uint32_t *h)
{
	if (!bytes || bytes > kBlobMaxBytes)
		return false;
	*w = bytes > kBlobPitch ? kBlobPitch : ((bytes + 63) & ~63u);
	*h = (bytes + *w - 1) / *w;
	return true;
}

/*
 * HAL_PIXEL_FORMAT_YCBCR_420_888 is allocated as NV12 in the Qualcomm Venus
 * layout so the hardware codec can decode into / encode from it directly
 * (V4L2 DMABUF): Y pitch ALIGN(w, 128), ALIGN(h, 32) Y rows, interleaved CbCr
 * plane right after them with ALIGN(h / 2, 16) rows, and the codec's 4 KiB +
 * 8 KiB tail, rounded to 4 KiB (venus get_framesize_raw_nv12()). Stored as a
 * linear R8 buffer of `pitch` columns.
 */
struct nv12_layout {
	uint32_t pitch, uv_offset, size, rows;
};

static bool nv12_layout(uint32_t w, uint32_t h, struct nv12_layout *l)
{
	if (!w || !h || w > 4096 || h > 4096)
		return false;
	l->pitch = (w + 127) & ~127u;
	l->uv_offset = l->pitch * ((h + 31) & ~31u);
	const uint32_t uv = l->pitch * (((h + 1) / 2 + 15) & ~15u) + 4096;
	l->size = (l->uv_offset + uv + 8192 + 4095) & ~4095u;
	l->rows = (l->size + l->pitch - 1) / l->pitch;
	return true;
}

static std::unordered_map<buffer_handle_t, struct gbm_bo *> gbm_bo_handle_map;

struct bo_data_t {
	void *map_data;
	int lock_count;
	int locked_for;
	unsigned metadata_sequence;
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
	case HAL_PIXEL_FORMAT_BLOB:
	case HAL_PIXEL_FORMAT_YCBCR_420_888:
		fmt = GBM_FORMAT_R8;
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
	case HAL_PIXEL_FORMAT_BLOB:
	case HAL_PIXEL_FORMAT_YCBCR_420_888:
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

static unsigned int get_pipe_bind(int usage, bool native_scanout)
{
	unsigned int bind = 0;

	if (usage & (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN))
		bind |= GBM_BO_USE_LINEAR;
	if (usage & GRALLOC_USAGE_CURSOR)
		;//bind |= GBM_BO_USE_CURSOR;
	if (usage & (GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE))
		bind |= GBM_BO_USE_RENDERING;
	if (usage & GRALLOC_USAGE_HW_FB) {
		bind |= GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING;
		if (native_scanout)
			bind |= GBM_BO_USE_SCANOUT;
	}
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
	} else if (handle->format == HAL_PIXEL_FORMAT_BLOB &&
		   !blob_shape(handle->width, &data.width, &data.height)) {
		return NULL;
	} else if (handle->format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
		struct nv12_layout l;
		if (!nv12_layout(handle->width, handle->height, &l))
			return NULL;
		data.width = l.pitch;
		data.height = l.rows;
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
	// Unset in P30; only a future verified native startup may select this.
	const bool native_scanout = property_get_bool("ro.superman.native_scanout", false);
	int usage = get_pipe_bind(handle->usage, native_scanout);
	if ((usage & GBM_BO_USE_SCANOUT) &&
	    (handle->format != HAL_PIXEL_FORMAT_BGRA_8888 ||
	     handle->width != 720 || handle->height != 1280)) {
		errno = EINVAL;
		return NULL;
	}
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
        // Packed GR88 storage must contain both separately aligned chroma
        // planes. A 32-byte Y pitch makes ALIGN(Y/2,16) exactly Y/2.
        width = ((width + 31) & ~31) / 2;
        height += handle->height / 2;
        usage |= GBM_BO_USE_LINEAR;
    } else if (handle->format == HAL_PIXEL_FORMAT_BLOB) {
        uint32_t bw, bh;
        if (handle->height != 1 || !blob_shape(handle->width, &bw, &bh)) {
            errno = EINVAL;
            return NULL;
        }
        width = bw;
        height = bh;
        usage |= GBM_BO_USE_LINEAR;
    } else if (handle->format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
        struct nv12_layout l;
        if (!nv12_layout(handle->width, handle->height, &l)) {
            errno = EINVAL;
            return NULL;
        }
        width = l.pitch;
        height = l.rows;
        usage |= GBM_BO_USE_LINEAR;
    }

	ALOGV("create BO, size=%dx%d, fmt=%d, usage=%x",
	      handle->width, handle->height, handle->format, usage);
	bo = gbm_bo_create(gbm, width, height, format, usage);
	if (!bo) {
		ALOGE("failed to create BO, size=%dx%d, fmt=%d, usage=%x",
		      handle->width, handle->height, handle->format, usage);
		return NULL;
	}

	// Validate before exporting a DMA-BUF; failures have no fallback or FD.
	if ((usage & GBM_BO_USE_SCANOUT) &&
	    (gbm_bo_get_stride(bo) < 720 * 4 || gbm_bo_get_stride(bo) % 4 ||
	     gbm_bo_get_modifier(bo) != 0)) {
		gbm_bo_destroy(bo);
		errno = EINVAL;
		return NULL;
	}
    if (handle->format == HAL_PIXEL_FORMAT_YV12 &&
        (gbm_bo_get_stride(bo) < uint32_t(handle->width) ||
         gbm_bo_get_stride(bo) % 32 || gbm_bo_get_modifier(bo) != 0)) {
        gbm_bo_destroy(bo);
        errno = EINVAL;
        return nullptr;
    }
    if ((handle->format == HAL_PIXEL_FORMAT_BLOB ||
         handle->format == HAL_PIXEL_FORMAT_YCBCR_420_888) &&
        (gbm_bo_get_stride(bo) != uint32_t(width) || gbm_bo_get_modifier(bo) != 0)) {
        // Rows would not be contiguous bytes.
        gbm_bo_destroy(bo);
        errno = EINVAL;
        return nullptr;
    }
    if (gbm_bo_get_modifier(bo) == 0) {
        // Linear buffers are CPU-mapped by importers straight from the DMA-BUF
        // (writable MAP_SHARED), so export it read-write; gbm_bo_get_fd() does not.
        int fd = -1;
        if (drmPrimeHandleToFD(gbm_device_get_fd(gbm), gbm_bo_get_handle(bo).u32,
                               DRM_CLOEXEC | DRM_RDWR, &fd))
            fd = -1;
        handle->prime_fd = fd;
    } else {
        handle->prime_fd = gbm_bo_get_fd(bo);
    }
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
    if (h->format == HAL_PIXEL_FORMAT_BLOB) {
        uint32_t bw, bh;
        return h->magic == GRALLOC_HANDLE_MAGIC && h->version == GRALLOC_HANDLE_VERSION &&
            h->prime_fd >= 0 && fcntl(h->prime_fd, F_GETFD) >= 0 && h->height == 1 &&
            blob_shape(h->width, &bw, &bh) && h->stride == bw;
    }
    if (h->format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
        struct nv12_layout l;
        return h->magic == GRALLOC_HANDLE_MAGIC && h->version == GRALLOC_HANDLE_VERSION &&
            h->prime_fd >= 0 && fcntl(h->prime_fd, F_GETFD) >= 0 &&
            nv12_layout(h->width, h->height, &l) && h->stride == l.pitch;
    }
    return h->magic == GRALLOC_HANDLE_MAGIC && h->version == GRALLOC_HANDLE_VERSION &&
        h->prime_fd >= 0 && fcntl(h->prime_fd, F_GETFD) >= 0 &&
        h->width > 0 && h->height > 0 && h->width <= 16384 && h->height <= 16384 &&
        get_gbm_format(h->format) && bpp > 0 && h->stride >= h->width * bpp &&
        h->stride % bpp == 0 && uint64_t(h->stride) * h->height <= SIZE_MAX &&
        (h->format != HAL_PIXEL_FORMAT_YV12 || !(h->width % 2 || h->height % 2));
}

/*
 * Imported LINEAR buffers are CPU-mapped straight from their DMA-BUF, with
 * DMA_BUF_IOCTL_SYNC waiting on the implicit GPU fences like gbm_bo_map().
 * Importing processes (codecs, input surfaces, apps) then never bring up the
 * GPU driver (Mesa) just to register or map a buffer: that needs DRM access
 * and syscalls the media seccomp sandboxes block. Non-linear layouts keep the
 * GBM import path.
 */
struct direct_buf {
	size_t size;		/* DMA-BUF size */
	void *addr;		/* CPU mapping while locked for SW access */
	int lock_count;
	int locked_for;
};
static std::unordered_map<buffer_handle_t, direct_buf> direct_map;

static uint64_t linear_bytes(const struct gralloc_handle_t *h)
{
	if (h->format == HAL_PIXEL_FORMAT_BLOB) {
		uint32_t bw, bh;
		return blob_shape(h->width, &bw, &bh) ? uint64_t(bw) * bh : 0;
	}
	if (h->format == HAL_PIXEL_FORMAT_YV12) {
		const uint64_t ysize = uint64_t(h->stride) * h->height;
		const uint64_t cstride = ((uint64_t(h->stride) / 2 + 15) & ~UINT64_C(15));
		return ysize + 2 * cstride * (h->height / 2);
	}
	if (h->format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
		struct nv12_layout l;
		return nv12_layout(h->width, h->height, &l) ? l.size : 0;
	}
	/* Single-plane formats */
	return gralloc_gbm_get_bpp(h->format) > 0 ? uint64_t(h->stride) * h->height : 0;
}

bool gralloc_gbm_handle_is_direct(buffer_handle_t handle)
{
	if (!valid_handle(handle))
		return false;
	const auto *h = gralloc_handle(handle);
	/* DRM_FORMAT_MOD_LINEAR only: Mesa reports tiled layouts otherwise. */
	return h->modifier == 0 && linear_bytes(h) &&
	       (h->format != HAL_PIXEL_FORMAT_YV12 || h->stride % 32 == 0);
}

int gralloc_gbm_handle_register_direct(buffer_handle_t handle)
{
	if (!gralloc_gbm_handle_is_direct(handle) || direct_map.count(handle) ||
	    gbm_bo_handle_map.count(handle))
		return -EINVAL;
	const auto *h = gralloc_handle(handle);
	const off_t size = lseek(h->prime_fd, 0, SEEK_END);
	const uint64_t need = linear_bytes(h);
	if (size <= 0 || !need || need > uint64_t(size))
		return -EINVAL;
	direct_map.emplace(handle, direct_buf{size_t(size), nullptr, 0, 0});
	return 0;
}

static int direct_sync(int fd, uint64_t flags)
{
	struct dma_buf_sync sync = { flags };
	int ret;
	do {
		ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	} while (ret && (errno == EINTR || errno == EAGAIN));
	return ret ? -errno : 0;
}

static uint64_t direct_sync_flags(int usage)
{
	uint64_t f = 0;
	if (usage & GRALLOC_USAGE_SW_READ_MASK)
		f |= DMA_BUF_SYNC_READ;
	if (usage & GRALLOC_USAGE_SW_WRITE_MASK)
		f |= DMA_BUF_SYNC_WRITE;
	return f;
}

static int direct_lock(buffer_handle_t handle, direct_buf &d, int usage, void **addr)
{
	const auto *h = gralloc_handle(handle);
	if (d.lock_count)
		return -EBUSY;
	const int sw = usage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK);
	if (sw) {
		const int prot = PROT_READ |
			((usage & GRALLOC_USAGE_SW_WRITE_MASK) ? PROT_WRITE : 0);
		void *p = mmap(nullptr, d.size, prot, MAP_SHARED, h->prime_fd, 0);
		if (p == MAP_FAILED)
			return -errno;
		int err = direct_sync(h->prime_fd, DMA_BUF_SYNC_START | direct_sync_flags(usage));
		if (err) {
			munmap(p, d.size);
			return err;
		}
		d.addr = p;
	}
	*addr = d.addr;
	d.lock_count = 1;
	d.locked_for = usage;
	return 0;
}

static int direct_unlock(buffer_handle_t handle, direct_buf &d)
{
	if (!d.lock_count)
		return -EINVAL;
	int err = 0;
	if (d.addr) {
		err = direct_sync(gralloc_handle(handle)->prime_fd,
				  DMA_BUF_SYNC_END | direct_sync_flags(d.locked_for));
		munmap(d.addr, d.size);
		d.addr = nullptr;
	}
	d.lock_count = 0;
	d.locked_for = 0;
	return err;
}

int gbm_free(buffer_handle_t handle)
{
    auto direct = direct_map.find(handle);
    if (direct != direct_map.end()) {
        if (direct->second.lock_count) return -EBUSY;
        direct_map.erase(direct);
        return 0;
    }
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
        // The access rectangle was validated in image coordinates. Map all
        // three planes from their origins, even for a cropped video frame.
        x = y = 0;
        w = gbm_handle->width / 2;
        h = gbm_handle->height + gbm_handle->height / 2;
    } else if (gbm_handle->format == HAL_PIXEL_FORMAT_BLOB) {
        // Map every row from the origin: callers address the bytes linearly.
        uint32_t bw, bh;
        if (!blob_shape(gbm_handle->width, &bw, &bh))
            return -EINVAL;
        x = y = 0;
        w = bw;
        h = bh;
    } else if (gbm_handle->format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
        struct nv12_layout l;
        if (!nv12_layout(gbm_handle->width, gbm_handle->height, &l))
            return -EINVAL;
        x = y = 0;
        w = l.pitch;
        h = l.rows;
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

	/*
	 * Camera/video producers (the external camera HAL's preview and video streams) ask for
	 * IMPLEMENTATION_DEFINED: give them the Venus NV12 layout, which lock_ycbcr and Mesa's EGL
	 * YUV importer (IMPLEMENTATION_DEFINED -> lock_ycbcr) already handle. Other uses of
	 * IMPLEMENTATION_DEFINED stay unsupported, as before.
	 */
	if (format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED &&
	    (usage & (GRALLOC_USAGE_HW_CAMERA_WRITE | GRALLOC_USAGE_HW_VIDEO_ENCODER)))
		format = HAL_PIXEL_FORMAT_YCBCR_420_888;
	const bool blob = format == HAL_PIXEL_FORMAT_BLOB;
	if (!stride || width <= 0 || height <= 0 ||
        (blob ? (height != 1 || uint32_t(width) > kBlobMaxBytes) : (width > 16384 || height > 16384)) ||
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
    auto direct = direct_map.find(handle);
    if (direct != direct_map.end()) {
        int err = direct_lock(handle, direct->second, usage, addr);
        return err ? reject("direct", err) : 0;
    }
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
        unsigned metadataSequence = superman_gralloc_metadata::Begin(gbm_handle, usage, x, y, w, h);
		int err = gbm_map(handle, x, y, w, h, write, addr);
        superman_gralloc_metadata::Event(metadataSequence, "map_end", err);
        bo_data->metadata_sequence = err ? 0 : metadataSequence;
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

// YV12: Y, then Cr, then Cb; pitches are bytes, not the visible width.
// The caller holds the module mutex, including across the ordinary BO lock.
int gralloc_gbm_bo_lock_ycbcr(buffer_handle_t handle, int usage,
        int x, int y, int w, int h, android_ycbcr *layout)
{
    if (!layout) return -EINVAL;
    memset(layout, 0, sizeof(*layout));
    if (!valid_handle(handle)) return -EINVAL;
    const auto *gh = gralloc_handle(handle);
    const bool nv12 = gh->format == HAL_PIXEL_FORMAT_YCBCR_420_888;
    if ((gh->format != HAL_PIXEL_FORMAT_YV12 && !nv12) || gh->modifier != 0 ||
        (!nv12 && gh->stride % 32))
        return -EINVAL;
    const int read = usage & GRALLOC_USAGE_SW_READ_MASK;
    const int write = usage & GRALLOC_USAGE_SW_WRITE_MASK;
    // Only the CPU access bits matter here; callers such as the camera HAL pass the
    // buffer's full usage (camera, texture, ...), which a CPU mapping does not depend on.
    if ((read && read != GRALLOC_USAGE_SW_READ_RARELY && read != GRALLOC_USAGE_SW_READ_OFTEN) ||
        (write && write != GRALLOC_USAGE_SW_WRITE_RARELY && write != GRALLOC_USAGE_SW_WRITE_OFTEN))
        return -EINVAL;
    if (!x && !y && !w && !h) { w = gh->width; h = gh->height; }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        uint64_t(x) + w > gh->width || uint64_t(y) + h > gh->height)
        return -EINVAL;
    struct nv12_layout nl = {};
    if (nv12 && !nv12_layout(gh->width, gh->height, &nl))
        return -EINVAL;
    const uint64_t ysize = nv12 ? nl.uv_offset : uint64_t(gh->stride) * gh->height;
    const uint64_t cstride = nv12 ? nl.pitch : ((uint64_t(gh->stride) / 2 + 15) & ~UINT64_C(15));
    const uint64_t csize = cstride * (gh->height / 2);
    const uint64_t size = nv12 ? nl.size : ysize + csize * 2;
    struct stat backing = {};
    if (size > SIZE_MAX || fstat(gh->prime_fd, &backing) || backing.st_size < 0 ||
        size > uint64_t(backing.st_size)) return -EINVAL;
    void *addr = nullptr;
    int err = gralloc_gbm_bo_lock(handle, usage, 0, 0, gh->width, gh->height, &addr);
    if (err) return err;
    // Mesa's EGL YUV importer queries usage=0 and expects offsets from zero,
    // followed by an ordinary unlock. Never do arithmetic on a null pointer.
    const uintptr_t base = reinterpret_cast<uintptr_t>(addr);
    if (((read || write) && !addr) || base > UINTPTR_MAX - size) {
        gralloc_gbm_bo_unlock(handle);
        return -EINVAL;
    }
    layout->y = addr;
    layout->ystride = gh->stride;
    layout->cstride = cstride;
    if (nv12) {
        // Interleaved CbCr after the aligned luma rows.
        layout->cb = reinterpret_cast<void *>(base + ysize);
        layout->cr = reinterpret_cast<void *>(base + ysize + 1);
        layout->chroma_step = 2;
        return 0;
    }
    layout->cr = reinterpret_cast<void *>(base + ysize);
    layout->cb = reinterpret_cast<void *>(base + ysize + csize);
    layout->chroma_step = 1;
    return 0;
}

/*
 * Unlock a bo.
 */
int gralloc_gbm_bo_unlock(buffer_handle_t handle)
{
	auto direct = direct_map.find(handle);
	if (direct != direct_map.end())
		return direct_unlock(handle, direct->second);
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

    if (mapped) {
        superman_gralloc_metadata::Event(bo_data->metadata_sequence, "unmap_begin", 0);
        gbm_unmap(bo);
        // The underlying unmap returns void; status is unavailable, not success.
        superman_gralloc_metadata::Event(bo_data->metadata_sequence, "unmap_end", -ENODATA);
        bo_data->metadata_sequence = 0;
    }

	bo_data->lock_count--;
	if (!bo_data->lock_count)
		bo_data->locked_for = 0;

	return 0;
}
