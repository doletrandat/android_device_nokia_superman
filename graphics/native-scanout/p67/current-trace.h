// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "current-trace-core.h"
#include <cutils/properties.h>
#include <gralloc_handle.h>
#include <drm_fourcc.h>
#include <hardware/gralloc.h>
#include <ui/GraphicBufferMapper.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <memory>

namespace android {
namespace superman_current_target_trace {
inline int FenceStatus(int fd) {
  if (fd < 0) return -ENODATA;
  auto *info = sync_file_info(fd);
  if (!info) return -EIO;
  int status = info->status;
  sync_file_info_free(info);
  return status;
}

inline uint64_t Now() {
  struct timespec ts = {};
  if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
  return uint64_t(ts.tv_sec) * UINT64_C(1000000000) + ts.tv_nsec;
}

// Called only for display 0 under the existing compositor lock.
inline superman_current_trace::Budget &State() {
  static superman_current_trace::Budget budget;
  return budget;
}

inline bool Enabled() {
  char selector[PROPERTY_VALUE_MAX] = {};
  property_get("ro.superman.native_target_trace", selector, "");
  return !strcmp(selector, "p67");
}

inline bool Begin(bool current) {
  char selector[PROPERTY_VALUE_MAX] = {}, arm[PROPERTY_VALUE_MAX] = {};
  property_get("ro.superman.native_target_trace", selector, "");
  if (strcmp(selector, "p67")) return false;
  property_get("debug.superman.target_trace", arm, "");
  uint64_t now = Now();
  bool selected = current ? State().Current(selector, arm, now)
                          : State().Commit(selector, arm, now);
  if (selected)
    ALOGI("superman_current_trace arm=%s attempt=%u kind=%s arm_begin_ns=%" PRIu64
          " begin_ns=%" PRIu64, State().token, State().attempt,
          current ? "current" : "commit", State().begin_ns, now);
  return selected;
}

inline bool WriteAll(int fd, const void *data, size_t bytes) {
  auto *p = static_cast<const uint8_t *>(data);
  while (bytes) {
    ssize_t written = write(fd, p, bytes);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    p += written;
    bytes -= size_t(written);
  }
  return true;
}

inline void Snapshot(DrmDisplayComposition *composition, const char *stage,
                     uint64_t latches, int latch_fd, uint64_t vblank_ns = 0) {
  int latch_status = FenceStatus(latch_fd);
  if (!stage || (strcmp(stage, "current") && strcmp(stage, "latched") &&
                 strcmp(stage, "retiring"))) return;
  if (!composition || composition->layers().size() != 1) {
    ALOGW("superman_current_trace stage=%s skip=layer_count", stage);
    return;
  }
  auto &layer = composition->layers()[0];
  // Never fall back to the borrowed producer handle.
  buffer_handle_t handle = layer.handle.get();
  const auto *bo = layer.buffer.operator->();
  int acquire = FenceStatus(layer.acquire_fence.get());
  int own_latch_fd = composition->diagnostic_latch_fence();
  int own_latch = FenceStatus(own_latch_fd);
  uint64_t begin = Now();
  ALOGI("superman_current_trace arm=%s attempt=%u stage=%s frame=%" PRIu64
        " latches=%" PRIu64 " fb=%u gem=%u acquire_fd=%d acquire=%d"
        " latch_fd=%d latch=%d own_latch_fd=%d own_latch=%d ns=%" PRIu64
        " vblank_ns=%" PRIu64,
        State().token, State().attempt, stage, composition->frame_no(),
        latches, bo ? bo->fb_id : 0, bo ? bo->gem_handles[0] : 0,
        layer.acquire_fence.get(), acquire, latch_fd, latch_status,
        own_latch_fd, own_latch, begin, vblank_ns);
  // Every P67 read is of a latched target, including the old target that
  // remains active before the next commit. Both latch proofs must succeed.
  bool after_latch = true;
  if (!superman_trace::ReadAllowed(acquire, after_latch, latch_status) ||
      own_latch != 1 || !handle || !bo || handle->version != sizeof(native_handle_t) ||
      handle->numFds != GRALLOC_HANDLE_NUM_FDS ||
      handle->numInts != GRALLOC_HANDLE_NUM_INTS || layer.protected_usage()) {
    ALOGW("superman_current_trace stage=%s skip=fence_or_handle", stage);
    return;
  }
  const auto *h = gralloc_handle(handle);
  struct stat backing = {}, imported = {};
  if (h->magic != GRALLOC_HANDLE_MAGIC || h->version != GRALLOC_HANDLE_VERSION ||
      h->format != HAL_PIXEL_FORMAT_BGRA_8888 ||
      !(h->usage & GRALLOC_USAGE_HW_FB) || (h->usage & GRALLOC_USAGE_PROTECTED) ||
      h->modifier != DRM_FORMAT_MOD_LINEAR || h->prime_fd < 0 ||
      fstat(h->prime_fd, &backing) || backing.st_size < 0 ||
      fstat(static_cast<int>(bo->prime_fds[0]), &imported) ||
      backing.st_dev != imported.st_dev || backing.st_ino != imported.st_ino ||
      !superman_trace::LayoutValid(h->width, h->height, h->stride, backing.st_size) ||
      bo->width != h->width || bo->height != h->height ||
      bo->hal_format != h->format || bo->format != DRM_FORMAT_ARGB8888 ||
      bo->pitches[0] != h->stride || bo->offsets[0] != 0 ||
      bo->modifiers[0] != DRM_FORMAT_MOD_LINEAR) {
    ALOGW("superman_current_trace stage=%s skip=backing_layout", stage);
    return;
  }
  // Directory must be explicitly prepared for this system-UID HAL. No mkdir,
  // arbitrary paths, symlink following, overwrites or capture of protected BOs.
  int directory = open("/data/vendor/superman-target-trace",
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  struct stat dir_stat = {};
  if (directory < 0) {
    ALOGW("superman_current_trace stage=%s skip=directory errno=%d", stage, errno);
    return;
  }
  if (geteuid() != 1000 || fstat(directory, &dir_stat) ||
      !S_ISDIR(dir_stat.st_mode) || dir_stat.st_uid != 1000 ||
      (dir_stat.st_mode & 0777) != 0700) {
    close(directory);
    ALOGW("superman_current_trace stage=%s skip=directory_owner_mode", stage);
    return;
  }
  std::unique_ptr<uint8_t[]> pixels(new (std::nothrow) uint8_t[superman_trace::kBytes]);
  if (!pixels) { close(directory); return; }
  auto &mapper = GraphicBufferMapper::get();
  void *address = nullptr;
  int32_t bpp = -1, stride = -1;
  int locked = mapper.lock(handle, GRALLOC_USAGE_SW_READ_OFTEN,
                          Rect(720, 1280), &address, &bpp, &stride);
  if (locked) {
    close(directory);
    ALOGW("superman_current_trace stage=%s skip=mapper_lock status=%d", stage, locked);
    return;
  }
  // Mapper 2 reports -1 for both fields. In that case use the validated
  // owned GBM handle; this gralloc checks actual GBM map stride internally.
  bool copied = (bpp == -1 || bpp == 4) &&
                (stride == -1 || stride == static_cast<int32_t>(h->stride)) &&
                superman_trace::CopyRows(pixels.get(), superman_trace::kBytes,
                                        static_cast<const uint8_t *>(address),
                                        h->stride, backing.st_size);
  int unlocked = mapper.unlock(handle);
  uint64_t read_end = Now();
  if (!copied || unlocked) {
    close(directory);
    ALOGW("superman_current_trace stage=%s skip=mapper_layout_or_unlock status=%d",
          stage, unlocked);
    return;
  }
  // Copy has finished and the read mapping is unlocked before any file write.
  char name[128];
  snprintf(name, sizeof(name), "%s-%d-%u-%s-%" PRIu64 ".bgra", State().token,
           getpid(), State().attempt, stage, composition->frame_no());
  int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  int error = fd < 0 ? errno : 0;
  bool written = fd >= 0 && WriteAll(fd, pixels.get(), superman_trace::kBytes);
  if (fd >= 0) {
    if (!written) error = errno ? errno : EIO;
    if (close(fd) && written) { written = false; error = errno; }
  }
  uint64_t hash = superman_trace::Hash(pixels.get(), superman_trace::kBytes);
  char metadata[1536];
  int count = snprintf(metadata, sizeof(metadata),
      "version=p67\narm=%s\nattempt=%u\nstage=%s\nframe=%" PRIu64
      "\nlatches=%" PRIu64 "\nfb=%u\ngem=%u\ndev=%" PRIu64
      "\nino=%" PRIu64 "\nbacking_size=%" PRIu64 "\nstride=%u\nformat=BGRA8888\n"
      "acquire_fd=%d\nacquire=%d\nlatch=%d\nbegin_ns=%" PRIu64
      "\nread_end_ns=%" PRIu64 "\nwrite_end_ns=%" PRIu64 "\nbytes=%zu\n"
      "fnv1a64=%016" PRIx64 "\ncomplete=%u\nerrno=%d\n"
      "arm_begin_ns=%" PRIu64 "\nsample_vblank_ns=%" PRIu64
      "\nlatch_fd=%d\ncomposition_latch_fd=%d\ncomposition_latch=%d\n"
      "sample_origin=%s\n",
      State().token, State().attempt, stage, composition->frame_no(), latches,
      bo->fb_id, bo->gem_handles[0], uint64_t(backing.st_dev), uint64_t(backing.st_ino),
      uint64_t(backing.st_size), h->stride, layer.acquire_fence.get(), acquire,
      latch_status, begin, read_end, Now(), superman_trace::kBytes, hash, written, error, State().begin_ns,
      vblank_ns, latch_fd, own_latch_fd, own_latch,
      strcmp(stage, "current") ? "post_commit" :
          (vblank_ns ? "real_vblank" : "before_commit"));
  char meta_name[136];
  snprintf(meta_name, sizeof(meta_name), "%s.txt", name);
  int meta = openat(directory, meta_name,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  bool recorded = meta >= 0 && count > 0 && size_t(count) < sizeof(metadata) &&
                  WriteAll(meta, metadata, size_t(count));
  if (meta >= 0 && close(meta)) recorded = false;
  close(directory);
  ALOGI("superman_current_trace file=%s complete=%u metadata=%u errno=%d"
        " dev=%" PRIu64 " ino=%" PRIu64 " fnv1a64=%016" PRIx64
        " read_ns=%" PRIu64 " total_ns=%" PRIu64,
        name, written, recorded, error, uint64_t(backing.st_dev), uint64_t(backing.st_ino),
        hash, read_end - begin, Now() - begin);
}
}  // namespace superman_current_target_trace
}  // namespace android
