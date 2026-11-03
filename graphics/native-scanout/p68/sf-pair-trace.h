// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "pair-trace-core.h"
#include <compositionengine/LayerFE.h>
#include <cutils/properties.h>
#include <hardware/gralloc.h>
#include <ui/GraphicBuffer.h>
#include <sync/sync.h>
// This shared header's unused allocation helper predates SF's sign warnings.
// Keep the native handle definition exact; suppress only its signed assignments.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#include "../../../../../../external/libdrm/android/gralloc_handle.h"
#pragma clang diagnostic pop
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <memory>
#include <cmath>

namespace android::superman_sf_pair_trace {
inline uint64_t Now() {
  timespec ts = {};
  return clock_gettime(CLOCK_MONOTONIC, &ts) ? 0 :
      uint64_t(ts.tv_sec) * UINT64_C(1000000000) + uint64_t(ts.tv_nsec);
}
inline superman_pair_trace::Budget &State() {
  static superman_pair_trace::Budget state;
  return state;
}
inline bool Enabled() {
  char selector[PROPERTY_VALUE_MAX] = {};
  property_get("ro.superman.native_sf_pair_trace", selector, "");
  return !strcmp(selector, "p68");
}
inline int Status(int fd) {
  if (fd < 0) return -ENODATA;
  auto *info = sync_file_info(fd);
  if (!info) return -EIO;
  int value = info->status;
  sync_file_info_free(info);
  return value;
}
inline bool Board() {
  int fd = open("/proc/device-tree/compatible", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  char text[64] = {};
  ssize_t n = read(fd, text, sizeof(text)); close(fd);
  constexpr char expected[] = "nokia,superman\0qcom,msm8226\0";
  return n == ssize_t(sizeof(expected) - 1) && !memcmp(text, expected, sizeof(expected) - 1);
}
inline bool Properties() {
  char gralloc[PROPERTY_VALUE_MAX] = {}, hwc[PROPERTY_VALUE_MAX] = {};
  property_get("ro.hardware.gralloc", gralloc, "");
  property_get("ro.hardware.hwcomposer", hwc, "");
  return !strcmp(gralloc, "superman_native") && !strcmp(hwc, "superman_native");
}
struct Pixels {
  std::unique_ptr<uint8_t[]> bytes;
  uint64_t id = 0, dev = 0, ino = 0, size = 0, begin = 0, end = 0;
  uint32_t stride = 0, format = 0;
};
inline bool Validate(const sp<GraphicBuffer> &buffer, bool target, Pixels &out) {
  if (!buffer || buffer->getLayerCount() != 1 || !buffer->getId()) return false;
  const auto *base = buffer->handle;
  if (!base || base->version != sizeof(native_handle_t) ||
      base->numFds != GRALLOC_HANDLE_NUM_FDS || base->numInts != GRALLOC_HANDLE_NUM_INTS)
    return false;
  const auto *h = gralloc_handle(base);
  struct stat s = {};
  if (h->magic != GRALLOC_HANDLE_MAGIC || h->version != GRALLOC_HANDLE_VERSION ||
      h->modifier != 0 || h->prime_fd < 0 || (h->usage & GRALLOC_USAGE_PROTECTED) ||
      buffer->getUsage() != h->usage || (target && !(h->usage & GRALLOC_USAGE_HW_FB)) ||
      fstat(h->prime_fd, &s) || s.st_size < 0 ||
      !superman_pair_trace::Layout(h->width, h->height, h->stride, uint64_t(s.st_size),
                                  h->format, target) ||
      buffer->getWidth() != h->width || buffer->getHeight() != h->height ||
      uint32_t(buffer->getPixelFormat()) != h->format ||
      uint64_t(buffer->getStride()) * 4 != h->stride) return false;
  out.id = buffer->getId(); out.dev = uint64_t(s.st_dev); out.ino = uint64_t(s.st_ino);
  out.size = uint64_t(s.st_size); out.stride = h->stride; out.format = h->format;
  return true;
}
inline bool Copy(const sp<GraphicBuffer> &buffer, Pixels &out) {
  out.bytes.reset(new (std::nothrow) uint8_t[superman_trace::kBytes]);
  if (!out.bytes) return false;
  void *address = nullptr; int32_t bpp = -1, stride = -1;
  out.begin = Now();
  int result = buffer->lock(GRALLOC_USAGE_SW_READ_OFTEN, Rect(720, 1280),
                            &address, &bpp, &stride);
  if (result) return false;
  bool copied = (bpp == -1 || bpp == 4) &&
      (stride == -1 || stride == int32_t(out.stride)) &&
      superman_trace::CopyRows(out.bytes.get(), superman_trace::kBytes,
                              static_cast<const uint8_t *>(address), out.stride, out.size);
  int unlocked = buffer->unlock(); out.end = Now();
  return copied && unlocked == 0 && out.begin && out.end >= out.begin;
}
inline bool WriteAll(int fd, const void *data, size_t size) {
  auto *p = static_cast<const uint8_t *>(data);
  while (size) {
    ssize_t n = write(fd, p, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    size -= size_t(n); p += n;
  }
  return true;
}
inline bool Save(int dir, const char *role, const Pixels &p, const Pixels &input,
                 const Pixels &target, uint64_t frame, uint64_t compose_ns,
                 int input_fd, int ready_fd, const char *name,
                 const compositionengine::LayerFE::LayerSettings &settings) {
  char filename[128];
  snprintf(filename, sizeof(filename), "%s-%d-%u-%s-%" PRIu64 ".pixels",
           State().token, getpid(), State().attempts, role, p.id);
  int fd = openat(dir, filename, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  int error = fd < 0 ? errno : 0;
  bool complete = fd >= 0 && WriteAll(fd, p.bytes.get(), superman_trace::kBytes);
  if (fd >= 0) {
    if (!complete) error = errno ? errno : EIO;
    if (close(fd)) { error = errno; complete = false; }
  }
  char metadata[4096];
  int count = snprintf(metadata, sizeof(metadata),
      "version=p68\narm=%s\npid=%d\nattempt=%u\nrole=%s\nbuffer_id=%" PRIu64
      "\ninput_buffer_id=%" PRIu64 "\ninput_frame=%" PRIu64 "\ntarget_buffer_id=%" PRIu64
      "\ncompose_ns=%" PRIu64 "\narm_begin_ns=%" PRIu64 "\nread_begin_ns=%" PRIu64
      "\nattempt_begin_ns=%" PRIu64 "\nread_end_ns=%" PRIu64 "\nwrite_end_ns=%" PRIu64
      "\ndev=%" PRIu64 "\nino=%" PRIu64 "\nbacking_size=%" PRIu64
      "\nwidth=720\nheight=1280\nstride=%u\nformat=%s\nbytes=%zu\nfnv1a64=%016" PRIx64
      "\ndraw_status=0\nwait_result=0\nwait_limit_ms=250\ninput_fd=%d\ninput_status=1"
      "\nready_fd=%d\nready_status=1\nsecure_layers=0\nprotected=0\noutput_internal=1"
      "\nownership=post_draw_before_queue\nsettings_name=%s\ncomplete=%d\nerrno=%d\n"
      "bounds=%.9g,%.9g,%.9g,%.9g\nalpha=%.9g\n",
      State().token, getpid(), State().attempts, role, p.id, input.id, frame, target.id,
      compose_ns, State().begin_ns, p.begin, State().attempt_begin_ns, p.end, Now(), p.dev, p.ino, p.size, p.stride,
      p.format == 1 ? "RGBA8888" : "BGRA8888", superman_trace::kBytes,
      superman_trace::Hash(p.bytes.get(), superman_trace::kBytes), input_fd, ready_fd,
      name, complete, error, double(settings.geometry.boundaries.left),
      double(settings.geometry.boundaries.top), double(settings.geometry.boundaries.right),
      double(settings.geometry.boundaries.bottom), double(float(settings.alpha)));
  if (count < 0 || size_t(count) >= sizeof(metadata)) return false;
  unsigned matrix_index = 0;
  for (const auto &matrix : {settings.geometry.positionTransform,
                             settings.source.buffer.textureTransform}) {
    int n = snprintf(metadata + count, sizeof(metadata) - size_t(count),
                     "%s=", matrix_index++ ? "texture_transform" : "position_transform");
    if (n < 0 || size_t(n) >= sizeof(metadata) - size_t(count)) return false;
    count += n;
    for (size_t col = 0; col < 4; ++col) for (size_t row = 0; row < 4; ++row) {
      n = snprintf(metadata + count, sizeof(metadata) - size_t(count),
                   "%.9g%s", double(matrix[col][row]), col == 3 && row == 3 ? "\n" : ",");
      if (n < 0 || size_t(n) >= sizeof(metadata) - size_t(count)) return false;
      count += n;
    }
  }
  char metaname[140]; snprintf(metaname, sizeof(metaname), "%s.txt", filename);
  int mf = openat(dir, metaname, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  bool meta = mf >= 0 && WriteAll(mf, metadata, size_t(count));
  if (mf >= 0 && close(mf)) meta = false;
  ALOGI("superman_sf_pair file=%s complete=%d metadata=%d errno=%d dev=%" PRIu64
        " ino=%" PRIu64, filename, complete, meta, error, p.dev, p.ino);
  return complete && meta;
}

// Called on the SF composition thread after drawLayers, BEFORE queueBuffer.
// The dequeued target and the exact request's input remain owned by SF here.
// Never substitutes successful fences or changes draw/queue/release results.
inline void Capture(const std::vector<compositionengine::LayerFE::LayerSettings> &requests,
                    const sp<GraphicBuffer> &buffer, int ready_fd, int draw_status,
                    uint64_t compose_ns, const sp<GraphicBuffer> &settingsBuffer,
                    const char *settingsName, bool secure_or_protected) {
  char selector[PROPERTY_VALUE_MAX] = {}, arm[PROPERTY_VALUE_MAX] = {};
  property_get("ro.superman.native_sf_pair_trace", selector, "");
  property_get("debug.superman.target_trace", arm, "");
  if (!State().Begin(selector, arm, Now())) return;
  ALOGI("superman_sf_pair arm=%s attempt=%u compose_ns=%" PRIu64,
        State().token, State().attempts, compose_ns);
  if (secure_or_protected || !Board() || !Properties() || geteuid() != 1000 ||
      !superman_pair_trace::SettingsName(settingsName)) {
    ALOGW("superman_sf_pair skip=context"); return;
  }
  const compositionengine::LayerFE::LayerSettings *request = nullptr;
  unsigned count = 0;
  for (const auto &r : requests) if (r.source.buffer.buffer == settingsBuffer && settingsBuffer) {
    request = &r; ++count;
  }
  if (count != 1 || !request->frameNumber || request->bufferId != settingsBuffer->getId()) {
    ALOGW("superman_sf_pair skip=input_request"); return;
  }
  for (size_t col = 0; col < 4; ++col) for (size_t row = 0; row < 4; ++row) {
    if (!std::isfinite(float(request->geometry.positionTransform[col][row])) ||
        !std::isfinite(float(request->source.buffer.textureTransform[col][row]))) return;
  }
  if (!compose_ns || compose_ns > State().attempt_begin_ns ||
      !std::isfinite(float(request->alpha)) || float(request->alpha) < 0 ||
      float(request->alpha) > 1 ||
      !std::isfinite(request->geometry.boundaries.left) ||
      !std::isfinite(request->geometry.boundaries.top) ||
      !std::isfinite(request->geometry.boundaries.right) ||
      !std::isfinite(request->geometry.boundaries.bottom) ||
      request->geometry.boundaries.left >= request->geometry.boundaries.right ||
      request->geometry.boundaries.top >= request->geometry.boundaries.bottom) return;
  Pixels input, output;
  if (!Validate(settingsBuffer, false, input) || !Validate(buffer, true, output) ||
      (input.dev == output.dev && input.ino == output.ino)) {
    ALOGW("superman_sf_pair skip=backing_layout"); return;
  }
  if (draw_status || ready_fd < 0 || !request->source.buffer.fence ||
      request->source.buffer.fence->get() < 0) {
    ALOGW("superman_sf_pair skip=absent_fence_or_draw"); return;
  }
  int duplicate = fcntl(ready_fd, F_DUPFD_CLOEXEC, 3);
  if (duplicate < 0) return;
  sp<Fence> ready = new Fence(duplicate);
  int waited = ready->wait(superman_pair_trace::kWaitMs);
  int input_fd = request->source.buffer.fence->get();
  int input_status = Status(input_fd), output_status = Status(ready->get());
  ALOGI("superman_sf_pair draw=%d wait=%d input_status=%d ready_status=%d",
        draw_status, waited, input_status, output_status);
  if (!superman_pair_trace::Fences(draw_status, waited, input_status, output_status)) return;
  int dir = open("/data/vendor/superman-sf-pair-trace",
                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  struct stat s = {};
  if (dir < 0) return;
  if (fstat(dir, &s) || !S_ISDIR(s.st_mode) || s.st_uid != 1000 ||
      (s.st_mode & 0777) != 0700) { close(dir); return; }
  // Both read mappings are unlocked before output; never maps for SW_WRITE.
  if (!Copy(settingsBuffer, input) || !Copy(buffer, output)) { close(dir); return; }
  bool a = Save(dir, "input", input, input, output, request->frameNumber, compose_ns,
                input_fd, ready->get(), settingsName, *request);
  bool b = Save(dir, "target", output, input, output, request->frameNumber, compose_ns,
                input_fd, ready->get(), settingsName, *request);
  close(dir);
  ALOGI("superman_sf_pair attempt=%u pair_complete=%d", State().attempts, a && b);
}
} // namespace android::superman_sf_pair_trace
