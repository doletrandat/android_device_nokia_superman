/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#define LOG_TAG "hwc-drm-display-compositor"

#include "drmdisplaycompositor.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>
#include <sstream>
#include <vector>

#include <drm/drm_mode.h>
#include <log/log.h>
#include <cutils/properties.h>
#include <inttypes.h>
#include <utils/Timers.h>
#include <sync/sync.h>
#include <utils/Trace.h>

#include "autolock.h"
#include "drmcrtc.h"
#include "drmdevice.h"
#include "drmplane.h"

static const uint32_t kWaitWritebackFence = 100;  // ms

#include "../../hwc-restore.h"
#include "../../../p66/target-trace.h"
#include "../../../p67/current-trace.h"

namespace android {

class CompositorVsyncCallback : public VsyncCallback {
 public:
  CompositorVsyncCallback(DrmDisplayCompositor *compositor)
      : compositor_(compositor) {
  }

  void Callback(int display, int64_t timestamp) {
    compositor_->Vsync(display, timestamp);
  }

 private:
  DrmDisplayCompositor *compositor_;
};

DrmDisplayCompositor::DrmDisplayCompositor()
    : resource_manager_(NULL),
      display_(-1),
      initialized_(false),
      active_(false),
      use_hw_overlays_(true),
      dump_frames_composited_(0),
      dump_last_timestamp_ns_(0),
      flatten_countdown_(FLATTEN_COUNTDOWN_INIT),
      writeback_fence_(-1) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts))
    return;
  dump_last_timestamp_ns_ = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
}

DrmDisplayCompositor::~DrmDisplayCompositor() {
  if (!initialized_)
    return;

  vsync_worker_.Exit();
  int ret = pthread_mutex_lock(&lock_);
  if (ret)
    ALOGE("Failed to acquire compositor lock %d", ret);
  DrmDevice *drm = resource_manager_->GetDrmDevice(display_);
  if (mode_.blob_id)
    drm->DestroyPropertyBlob(mode_.blob_id);
  if (mode_.old_blob_id)
    drm->DestroyPropertyBlob(mode_.old_blob_id);

  if (CompletePendingFrame()) {
    // An unconfirmed latch cannot release either candidate's imports.
    active_composition_.release();
    retiring_composition_.release();
    unconfirmed_composition_.release();
  } else {
    active_composition_.reset();
  }

  ret = pthread_mutex_unlock(&lock_);
  if (ret)
    ALOGE("Failed to acquire compositor lock %d", ret);

  pthread_mutex_destroy(&lock_);
}

int DrmDisplayCompositor::Init(ResourceManager *resource_manager, int display) {
  resource_manager_ = resource_manager;
  display_ = display;
  DrmDevice *drm = resource_manager_->GetDrmDevice(display);
  if (!drm) {
    ALOGE("Could not find drmdevice for display");
    return -EINVAL;
  }
  int ret = pthread_mutex_init(&lock_, NULL);
  if (ret) {
    ALOGE("Failed to initialize drm compositor lock %d\n", ret);
    return ret;
  }
  planner_ = Planner::CreateInstance(drm);

  vsync_worker_.Init(drm, display_);
  auto callback = std::make_shared<CompositorVsyncCallback>(this);
  vsync_worker_.RegisterCallback(callback);

  initialized_ = true;
  return 0;
}

std::unique_ptr<DrmDisplayComposition> DrmDisplayCompositor::CreateComposition()
    const {
  return std::unique_ptr<DrmDisplayComposition>(new DrmDisplayComposition());
}

std::unique_ptr<DrmDisplayComposition>
DrmDisplayCompositor::CreateInitializedComposition() const {
  DrmDevice *drm = resource_manager_->GetDrmDevice(display_);
  DrmCrtc *crtc = drm->GetCrtcForDisplay(display_);
  if (!crtc) {
    ALOGE("Failed to find crtc for display = %d", display_);
    return std::unique_ptr<DrmDisplayComposition>();
  }
  std::unique_ptr<DrmDisplayComposition> comp = CreateComposition();
  std::shared_ptr<Importer> importer = resource_manager_->GetImporter(display_);
  if (!importer) {
    ALOGE("Failed to find resources for display = %d", display_);
    return std::unique_ptr<DrmDisplayComposition>();
  }
  int ret = comp->Init(drm, crtc, importer.get(), planner_.get(), 0);
  if (ret) {
    ALOGE("Failed to init composition for display = %d", display_);
    return std::unique_ptr<DrmDisplayComposition>();
  }
  return comp;
}

std::tuple<uint32_t, uint32_t, int>
DrmDisplayCompositor::GetActiveModeResolution() {
  DrmDevice *drm = resource_manager_->GetDrmDevice(display_);
  DrmConnector *connector = drm->GetConnectorForDisplay(display_);
  if (connector == NULL) {
    ALOGE("Failed to determine display mode: no connector for display %d",
          display_);
    return std::make_tuple(0, 0, -ENODEV);
  }

  const DrmMode &mode = connector->active_mode();
  return std::make_tuple(mode.h_display(), mode.v_display(), 0);
}

int DrmDisplayCompositor::DisablePlanes(DrmDisplayComposition *display_comp) {
  drmModeAtomicReqPtr pset = drmModeAtomicAlloc();
  if (!pset) {
    ALOGE("Failed to allocate property set");
    return -ENOMEM;
  }

  int ret;
  std::vector<DrmCompositionPlane> &comp_planes = display_comp
                                                      ->composition_planes();
  for (DrmCompositionPlane &comp_plane : comp_planes) {
    DrmPlane *plane = comp_plane.plane();
    ret = drmModeAtomicAddProperty(pset, plane->id(),
                                   plane->crtc_property().id(), 0) < 0 ||
          drmModeAtomicAddProperty(pset, plane->id(), plane->fb_property().id(),
                                   0) < 0;
    if (ret) {
      ALOGE("Failed to add plane %d disable to pset", plane->id());
      drmModeAtomicFree(pset);
      return ret;
    }
  }
  DrmDevice *drm = resource_manager_->GetDrmDevice(display_);
  ret = drmModeAtomicCommit(drm->fd(), pset, 0, drm);
  if (ret) {
    ALOGE("Failed to commit pset ret=%d\n", ret);
    drmModeAtomicFree(pset);
    return ret;
  }

  drmModeAtomicFree(pset);
  return 0;
}

int DrmDisplayCompositor::SetupWritebackCommit(drmModeAtomicReqPtr pset,
                                               uint32_t crtc_id,
                                               DrmConnector *writeback_conn,
                                               DrmHwcBuffer *writeback_buffer) {
  int ret = 0;
  if (writeback_conn->writeback_fb_id().id() == 0 ||
      writeback_conn->writeback_out_fence().id() == 0) {
    ALOGE("Writeback properties don't exit");
    return -EINVAL;
  }
  if ((*writeback_buffer)->fb_id == 0) {
    ALOGE("Invalid writeback buffer");
    return -EINVAL;
  }
  ret = drmModeAtomicAddProperty(pset, writeback_conn->id(),
                                 writeback_conn->writeback_fb_id().id(),
                                 (*writeback_buffer)->fb_id);
  if (ret < 0) {
    ALOGE("Failed to add writeback_fb_id");
    return ret;
  }
  ret = drmModeAtomicAddProperty(pset, writeback_conn->id(),
                                 writeback_conn->writeback_out_fence().id(),
                                 (uint64_t)&writeback_fence_);
  if (ret < 0) {
    ALOGE("Failed to add writeback_out_fence");
    return ret;
  }

  ret = drmModeAtomicAddProperty(pset, writeback_conn->id(),
                                 writeback_conn->crtc_id_property().id(),
                                 crtc_id);
  if (ret < 0) {
    ALOGE("Failed to  attach writeback");
    return ret;
  }
  return 0;
}

int DrmDisplayCompositor::CommitFrame(DrmDisplayComposition *display_comp,
                                      bool test_only,
                                      DrmConnector *writeback_conn,
                                      DrmHwcBuffer *writeback_buffer) {
  ATRACE_CALL();

  int ret = 0;
  if (!test_only) commit_submitted_ = false;
  const bool nonblock = !test_only && display_comp->async_present() &&
      !mode_.needs_modeset && !writeback_conn && !writeback_buffer;

  std::vector<DrmHwcLayer> &layers = display_comp->layers();
  std::vector<DrmCompositionPlane> &comp_planes = display_comp
                                                      ->composition_planes();
  DrmDevice *drm = resource_manager_->GetDrmDevice(display_);
  struct OwnedFences {
    std::vector<int32_t> fds;
    explicit OwnedFences(size_t n) : fds(n, -1) {}
    ~OwnedFences() { for (int fd : fds) if (fd >= 0) close(fd); }
  } owned_fences(drm->crtcs().size());
  auto &out_fences = owned_fences.fds;
  uint32_t explicit_inputs = 0;
  const bool timing = !test_only && (display_comp->frame_no() < 8 ||
      display_comp->frame_no() % 120 == 0 || property_get_bool("debug.superman.frame_timing", false));
  nsecs_t ioctl_begin = 0, ioctl_end = 0;

  DrmConnector *connector = drm->GetConnectorForDisplay(display_);
  if (!connector) {
    ALOGE("Could not locate connector for display %d", display_);
    return -ENODEV;
  }
  DrmCrtc *crtc = drm->GetCrtcForDisplay(display_);
  if (!crtc) {
    ALOGE("Could not locate crtc for display %d", display_);
    return -ENODEV;
  }

  drmModeAtomicReqPtr pset = drmModeAtomicAlloc();
  if (!pset) {
    ALOGE("Failed to allocate property set");
    return -ENOMEM;
  }

  if (writeback_buffer != NULL) {
    if (writeback_conn == NULL) {
      ALOGE("Invalid arguments requested writeback without writeback conn");
      return -EINVAL;
    }
    ret = SetupWritebackCommit(pset, crtc->id(), writeback_conn,
                               writeback_buffer);
    if (ret < 0) {
      ALOGE("Failed to Setup Writeback Commit ret = %d", ret);
      return ret;
    }
  }
  if (!crtc->out_fence_ptr_property().id() ||
      crtc->pipe() >= out_fences.size()) {
    drmModeAtomicFree(pset);
    return -ENOTSUP;
  }
  if (!test_only && crtc->out_fence_ptr_property().id() != 0) {
    ret = drmModeAtomicAddProperty(pset, crtc->id(),
                                   crtc->out_fence_ptr_property().id(),
                                   (uint64_t)&out_fences[crtc->pipe()]);
    if (ret < 0) {
      ALOGE("Failed to add OUT_FENCE_PTR property to pset: %d", ret);
      drmModeAtomicFree(pset);
      return ret;
    }
  }

  if (mode_.needs_modeset) {
    ret = drmModeAtomicAddProperty(pset, crtc->id(),
                                   crtc->active_property().id(), 1);
    if (ret < 0) {
      ALOGE("Failed to add crtc active to pset\n");
      drmModeAtomicFree(pset);
      return ret;
    }

    ret = drmModeAtomicAddProperty(pset, crtc->id(), crtc->mode_property().id(),
                                   mode_.blob_id) < 0 ||
          drmModeAtomicAddProperty(pset, connector->id(),
                                   connector->crtc_id_property().id(),
                                   crtc->id()) < 0;
    if (ret) {
      ALOGE("Failed to add blob %d to pset", mode_.blob_id);
      drmModeAtomicFree(pset);
      return ret;
    }
  }

  for (DrmCompositionPlane &comp_plane : comp_planes) {
    DrmPlane *plane = comp_plane.plane();
    DrmCrtc *crtc = comp_plane.crtc();
    std::vector<size_t> &source_layers = comp_plane.source_layers();

    int fb_id = -1;
    int fence_fd = -1;
    hwc_rect_t display_frame;
    hwc_frect_t source_crop;
    uint64_t rotation = 0;
    uint64_t alpha = 0xFFFF;
    uint64_t blend;

    if (comp_plane.type() != DrmCompositionPlane::Type::kDisable) {
      if (source_layers.size() > 1) {
        ALOGE("Can't handle more than one source layer sz=%zu type=%d",
              source_layers.size(), comp_plane.type());
        continue;
      }

      if (source_layers.empty() || source_layers.front() >= layers.size()) {
        ALOGE("Source layer index %zu out of bounds %zu type=%d",
              source_layers.front(), layers.size(), comp_plane.type());
        break;
      }
      DrmHwcLayer &layer = layers[source_layers.front()];
      if (!layer.buffer) {
        ALOGE("Expected a valid framebuffer for pset");
        break;
      }
      fb_id = layer.buffer->fb_id;
      fence_fd = layer.acquire_fence.get();
      display_frame = layer.display_frame;
      source_crop = layer.source_crop;
      alpha = layer.alpha;

      if (plane->blend_property().id()) {
        switch (layer.blending) {
          case DrmHwcBlending::kPreMult:
            std::tie(blend, ret) = plane->blend_property().GetEnumValueWithName(
                "Pre-multiplied");
            break;
          case DrmHwcBlending::kCoverage:
            std::tie(blend, ret) = plane->blend_property().GetEnumValueWithName(
                "Coverage");
            break;
          case DrmHwcBlending::kNone:
          default:
            std::tie(blend, ret) = plane->blend_property().GetEnumValueWithName(
                "None");
            break;
        }
      }

      if (plane->zpos_property().id() &&
          !plane->zpos_property().is_immutable()) {
        uint64_t min_zpos = 0;

        // Ignore ret and use min_zpos as 0 by default
        std::tie(std::ignore, min_zpos) = plane->zpos_property().range_min();

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->zpos_property().id(),
                                       source_layers.front() + min_zpos) < 0;
        if (ret) {
          ALOGE("Failed to add zpos property %d to plane %d",
                plane->zpos_property().id(), plane->id());
          break;
        }
      }

      rotation = 0;
      if (layer.transform & DrmHwcTransform::kFlipH)
        rotation |= DRM_MODE_REFLECT_X;
      if (layer.transform & DrmHwcTransform::kFlipV)
        rotation |= DRM_MODE_REFLECT_Y;
      if (layer.transform & DrmHwcTransform::kRotate90)
        rotation |= DRM_MODE_ROTATE_90;
      else if (layer.transform & DrmHwcTransform::kRotate180)
        rotation |= DRM_MODE_ROTATE_180;
      else if (layer.transform & DrmHwcTransform::kRotate270)
        rotation |= DRM_MODE_ROTATE_270;
      else
        rotation |= DRM_MODE_ROTATE_0;

      if (fence_fd >= 0) {
        int prop_id = plane->in_fence_fd_property().id();
        if (prop_id == 0) {
          ALOGE("Failed to get IN_FENCE_FD property id");
          ret = -ENOTSUP;
          break;
        }
        ret = drmModeAtomicAddProperty(pset, plane->id(), prop_id, fence_fd);
        if (ret < 0) {
          ALOGE("Failed to add IN_FENCE_FD property to pset: %d", ret);
          break;
        }
        if (fence_fd >= 0) ++explicit_inputs;
      }
    }

    // Disable the plane if there's no framebuffer
    if (fb_id < 0) {
      ret = drmModeAtomicAddProperty(pset, plane->id(),
                                     plane->crtc_property().id(), 0) < 0 ||
            drmModeAtomicAddProperty(pset, plane->id(),
                                     plane->fb_property().id(), 0) < 0;
      if (ret) {
        ALOGE("Failed to add plane %d disable to pset", plane->id());
        break;
      }
      continue;
    }

    ret = drmModeAtomicAddProperty(pset, plane->id(),
                                   plane->crtc_property().id(), crtc->id()) < 0;
    ret |= drmModeAtomicAddProperty(pset, plane->id(),
                                    plane->fb_property().id(), fb_id) < 0;
    ret |= drmModeAtomicAddProperty(pset, plane->id(),
                                    plane->crtc_x_property().id(),
                                    display_frame.left) < 0;
    ret |= drmModeAtomicAddProperty(pset, plane->id(),
                                    plane->crtc_y_property().id(),
                                    display_frame.top) < 0;
    ret |= drmModeAtomicAddProperty(pset, plane->id(),
                                    plane->crtc_w_property().id(),
                                    display_frame.right - display_frame.left) <
           0;
    ret |= drmModeAtomicAddProperty(pset, plane->id(),
                                    plane->crtc_h_property().id(),
                                    display_frame.bottom - display_frame.top) <
           0;
    ret |= drmModeAtomicAddProperty(pset, plane->id(),
                                    plane->src_x_property().id(),
                                    (int)(source_crop.left) << 16) < 0;
    ret |= drmModeAtomicAddProperty(pset, plane->id(),
                                    plane->src_y_property().id(),
                                    (int)(source_crop.top) << 16) < 0;
    ret |= drmModeAtomicAddProperty(pset, plane->id(),
                                    plane->src_w_property().id(),
                                    (int)(source_crop.right - source_crop.left)
                                        << 16) < 0;
    ret |= drmModeAtomicAddProperty(pset, plane->id(),
                                    plane->src_h_property().id(),
                                    (int)(source_crop.bottom - source_crop.top)
                                        << 16) < 0;
    if (ret) {
      ALOGE("Failed to add plane %d to set", plane->id());
      break;
    }

    if (plane->rotation_property().id()) {
      ret = drmModeAtomicAddProperty(pset, plane->id(),
                                     plane->rotation_property().id(),
                                     rotation) < 0;
      if (ret) {
        ALOGE("Failed to add rotation property %d to plane %d",
              plane->rotation_property().id(), plane->id());
        break;
      }
    }

    if (plane->alpha_property().id()) {
      ret = drmModeAtomicAddProperty(pset, plane->id(),
                                     plane->alpha_property().id(), alpha) < 0;
      if (ret) {
        ALOGE("Failed to add alpha property %d to plane %d",
              plane->alpha_property().id(), plane->id());
        break;
      }
    }

    if (plane->blend_property().id()) {
      ret = drmModeAtomicAddProperty(pset, plane->id(),
                                     plane->blend_property().id(), blend) < 0;
      if (ret) {
        ALOGE("Failed to add pixel blend mode property %d to plane %d",
              plane->blend_property().id(), plane->id());
        break;
      }
    }
  }

  if (!ret) {
    uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (test_only)
      flags |= DRM_MODE_ATOMIC_TEST_ONLY;
    else if (nonblock)
      flags |= DRM_MODE_ATOMIC_NONBLOCK;

    if (!test_only) {
      ++lab_.live_attempts;
      lab_.explicit_acquire_inputs += explicit_inputs;
      lab_.restore_confirmed = 0;
    }
    if (timing) ioctl_begin = systemTime(SYSTEM_TIME_MONOTONIC);
    ret = drmModeAtomicCommit(drm->fd(), pset, flags, drm);
    if (timing) ioctl_end = systemTime(SYSTEM_TIME_MONOTONIC);
    if (!test_only && !ret) commit_submitted_ = true;
    if (ret) {
      if (!test_only)
        ALOGE("Failed to commit pset ret=%d\n", ret);
      drmModeAtomicFree(pset);
      return ret;
    }
  }
  if (pset)
    drmModeAtomicFree(pset);

  if (!test_only) {
    if (out_fences[crtc->pipe()] < 0) {
      ALOGE("Native commit returned no fence; retain active composition");
      return -EIO;
    }
    int fence = out_fences[crtc->pipe()];
    display_comp->set_out_fence(fence);
    out_fences[crtc->pipe()] = -1;
    // Keep a separate latch reference after TakeOutFence transfers ownership.
    // On descriptor exhaustion, wait synchronously instead of dropping the fence.
    if (nonblock) {
      int fd = fcntl(fence, F_DUPFD_CLOEXEC, 3);
      if (fd >= 0) pending_latch_fence_.Set(fd);
    }
    if (pending_latch_fence_.get() < 0) {
      if (sync_wait(fence, 500)) {
        ALOGE("Native latch fence timed out; retain both compositions");
        return -ETIMEDOUT;
      }
      struct sync_file_info *info = sync_file_info(fence);
      int status = info ? info->status : -EIO;
      sync_file_info_free(info);
      if (status != 1) return status < 0 ? status : -EIO;
      ++lab_.latched_frames;
    }
    if (timing) ALOGI("NATIVE_SF_COMMIT frame=%" PRIu64 " ioctl_ns=%" PRId64 " latch_wait_ns=%" PRId64 " fence=%s",
        display_comp->frame_no(), ioctl_end - ioctl_begin,
        systemTime(SYSTEM_TIME_MONOTONIC) - ioctl_end,
        pending_latch_fence_.get() >= 0 ? "pending" : "signaled");
    if (mode_.needs_modeset) {
      // KMS atomic ACTIVE owns DPMS; no post-commit legacy DPMS operation.
      if (mode_.old_blob_id)
        drm->DestroyPropertyBlob(mode_.old_blob_id);
      connector->set_active_mode(mode_.mode);
      mode_.old_blob_id = mode_.blob_id;
      mode_.blob_id = 0;
      mode_.needs_modeset = false;
    }
  }

  return ret;
}

int DrmDisplayCompositor::ApplyDpms(DrmDisplayComposition *display_comp) {
  DrmDevice *drm = resource_manager_->GetDrmDevice(display_);
  DrmConnector *conn = drm->GetConnectorForDisplay(display_);
  if (!conn) {
    ALOGE("Failed to get DrmConnector for display %d", display_);
    return -ENODEV;
  }

  const DrmProperty &prop = conn->dpms_property();
  int ret = drmModeConnectorSetProperty(drm->fd(), conn->id(), prop.id(),
                                        display_comp->dpms_mode());
  if (ret) {
    ALOGE("Failed to set DPMS property for connector %d", conn->id());
    return ret;
  }
  return 0;
}

std::tuple<int, uint32_t> DrmDisplayCompositor::CreateModeBlob(
    const DrmMode &mode) {
  struct drm_mode_modeinfo drm_mode;
  memset(&drm_mode, 0, sizeof(drm_mode));
  mode.ToDrmModeModeInfo(&drm_mode);

  uint32_t id = 0;
  DrmDevice *drm = resource_manager_->GetDrmDevice(display_);
  int ret = drm->CreatePropertyBlob(&drm_mode, sizeof(struct drm_mode_modeinfo),
                                    &id);
  if (ret) {
    ALOGE("Failed to create mode property blob %d", ret);
    return std::make_tuple(ret, 0);
  }
  ALOGE("Create blob_id %" PRIu32 "\n", id);
  return std::make_tuple(ret, id);
}

int DrmDisplayCompositor::CompletePendingFrame(int timeout_ms) {
  if (present_fault_) return -EIO;
  if (pending_latch_fence_.get() < 0) return 0;
  if (sync_wait(pending_latch_fence_.get(), timeout_ms)) {
    if (!timeout_ms && errno == ETIME) return -EAGAIN;
    present_fault_ = true;
    return -ETIMEDOUT;
  }
  struct sync_file_info *info = sync_file_info(pending_latch_fence_.get());
  int status = info ? info->status : -EIO;
  sync_file_info_free(info);
  if (status != 1) {
    present_fault_ = true;
    return status < 0 ? status : -EIO;
  }
  pending_latch_fence_.Set(-1);
  ++lab_.latched_frames;
  if (retiring_composition_) {
    retiring_composition_.reset();
    ++lab_.retired_compositions;
  }
  return 0;
}

int DrmDisplayCompositor::LaboratoryStatus(superman_hwc_lab_state *state) {
  if (!state || state->size != sizeof(*state)) return -EINVAL;
  AutoLock lock(&lock_, __func__);
  if (lock.Lock()) return -EDEADLK;
  if (pending_latch_fence_.get() >= 0) CompletePendingFrame(0);
  *state = lab_;
  state->size = sizeof(*state);
  // Count the physically latched owner, not an accepted but pending target.
  state->active_composition = pending_latch_fence_.get() >= 0 ?
      (retiring_composition_ ? 1 : 0) : (active_composition_ ? 1 : 0);
  return 0;
}

int DrmDisplayCompositor::LaboratoryRestore(int32_t *fence) {
  if (!fence) return -EINVAL;
  *fence = -1;
  AutoLock lock(&lock_, __func__);
  if (lock.Lock()) return -EDEADLK;
  DrmDevice *drm = resource_manager_->GetDrmDevice(display_);
  DrmCrtc *crtc = drm ? drm->GetCrtcForDisplay(display_) : nullptr;
  DrmConnector *conn = drm ? drm->GetConnectorForDisplay(display_) : nullptr;
  uint32_t plane = 0;
  if (!drm || !crtc || !conn) return -ENODEV;
  for (auto &p : drm->planes()) {
    if (!p->GetCrtcSupported(*crtc)) continue;
    if (plane) return -EINVAL;
    plane = p->id();
  }
  if (!plane) return -ENODEV;
  int ret = CompletePendingFrame();
  if (ret) return ret;
  ret = superman_hwc_restore(drm->fd(), crtc->id(), conn->id(), plane, fence);
  if (ret) return ret; // Preserve all active imports if restore is unconfirmed.
  if (active_composition_) {
    active_composition_.reset();
    ++lab_.retired_compositions;
  }
  // Restoration clears logical ACTIVE/MODE_ID and disconnects the connector.
  // Re-arm the inherited mode for the next frame; physical firmware scanout
  // remains running. Transfer the retained blob instead of destroying it or
  // allocating a new one. Keep any already-pending config blob unchanged.
  if (!mode_.blob_id) {
    mode_.blob_id = mode_.old_blob_id;
    mode_.old_blob_id = 0;
  }
  mode_.needs_modeset = true;
  lab_.restore_confirmed = 1;
  vsync_worker_.VSyncControl(false);
  return 0;
}

void DrmDisplayCompositor::ClearDisplay() {
  // Hot-unplug must not destroy the current imports during a vblank read.
  AutoLock lock(&lock_, __func__);
  if (lock.Lock()) return;
  if (!active_composition_)
    return;
  if (CompletePendingFrame()) return;

  if (DisablePlanes(active_composition_.get()))
    return;

  active_composition_.reset(NULL);
  vsync_worker_.VSyncControl(false);
}

int DrmDisplayCompositor::ApplyFrame(
    std::unique_ptr<DrmDisplayComposition> composition, int status,
    bool writeback) {
  AutoLock lock(&lock_, __func__);
  if (lock.Lock())
    return -EDEADLK;
  int ret = CompletePendingFrame();
  if (ret) return ret;
  ret = status;
  commit_submitted_ = false;

  // If a flip wins the compositor lock before the vblank callback, preserve
  // the still-scanned target before committing its replacement. The current
  // producer slot is not released until normal presentation returns.
  if (!ret && !writeback && display_ == 0 &&
      superman_current_target_trace::State().current_attempts == 0 &&
      superman_current_target_trace::Begin(true)) {
    int fence = active_composition_ ?
        active_composition_->diagnostic_latch_fence() : -1;
    superman_current_target_trace::Snapshot(active_composition_.get(), "current",
                                            lab_.latched_frames, fence);
  }
  bool current_commit = !ret && !writeback && display_ == 0 &&
                        superman_current_target_trace::Begin(false);

  // Diagnostic arm consumes two attempts at most, only on the primary live
  // display. All capture failures leave the existing presentation path intact.
  bool trace = !ret && !writeback && display_ == 0 &&
               superman_target_trace::Begin();
  if (trace)
    superman_target_trace::Snapshot(composition.get(), "before", lab_.latched_frames, 0);

  if (!ret) {
    if (writeback && !CountdownExpired()) {
      ALOGE("Abort playing back scene");
      return -ECANCELED;
    }
    ret = CommitFrame(composition.get(), false);
  }

  if (ret) {
    ALOGE("Composite failed for display %d", display_);
    if (trace) ALOGW("superman_target_trace commit_failed=%d", ret);
    if (current_commit) ALOGW("superman_current_trace commit_failed=%d", ret);
    // An accepted ioctl with an unconfirmed latch must retain both imports.
    if (commit_submitted_) {
      present_fault_ = true;
      unconfirmed_composition_ = std::move(composition);
    }
    return ret;
  }
  ++dump_frames_composited_;

  // One descriptor per active composition, independent of the transferred
  // present fence. Failure only disables reads; it never changes commit status.
  if (!writeback && display_ == 0 && superman_current_target_trace::Enabled()) {
    int fd = fcntl(composition->out_fence(), F_DUPFD_CLOEXEC, 3);
    composition->set_diagnostic_latch_fence(fd);
    if (fd < 0) ALOGW("superman_current_trace latch_dup_failed errno=%d", errno);
  }

  if (current_commit) {
    int fence = composition->diagnostic_latch_fence();
    superman_current_target_trace::Snapshot(composition.get(), "latched",
                                            lab_.latched_frames, fence);
    superman_current_target_trace::Snapshot(active_composition_.get(), "retiring",
                                            lab_.latched_frames, fence);
  }

  if (trace) {
    int latch = superman_target_trace::FenceStatus(composition->out_fence());
    superman_target_trace::Snapshot(composition.get(), "latched", lab_.latched_frames, latch);
    // The old imports are still alive. FramebufferSurface has not released its
    // previous producer slot: onFrameCommitted happens after present returns.
    superman_target_trace::Snapshot(active_composition_.get(), "retiring", lab_.latched_frames, latch);
  }

  if (pending_latch_fence_.get() >= 0) {
    retiring_composition_ = std::move(active_composition_);
    active_composition_ = std::move(composition);
  } else {
    active_composition_.swap(composition);
    if (composition) {
      composition.reset();
      ++lab_.retired_compositions;
    }
  }

  flatten_countdown_ = FLATTEN_COUNTDOWN_INIT;
  vsync_worker_.VSyncControl(!writeback);
  return 0;
}

int DrmDisplayCompositor::ApplyComposition(
    std::unique_ptr<DrmDisplayComposition> composition) {
  int ret = 0;
  switch (composition->type()) {
    case DRM_COMPOSITION_TYPE_FRAME:
      if (composition->geometry_changed()) {
        // Send the composition to the kernel to ensure we can commit it. This
        // is just a test, it won't actually commit the frame.
        ret = TestComposition(composition.get());
        if (ret) {
          ALOGE("Commit test failed for display %d, FIXME", display_);
          return ret;
        }
      }

      ret = ApplyFrame(std::move(composition), ret);
      break;
    case DRM_COMPOSITION_TYPE_DPMS:
      active_ = (composition->dpms_mode() == DRM_MODE_DPMS_ON);
      ret = ApplyDpms(composition.get());
      if (ret)
        ALOGE("Failed to apply dpms for display %d", display_);
      return ret;
    case DRM_COMPOSITION_TYPE_MODESET:
      mode_.mode = composition->display_mode();
      if (mode_.blob_id)
        resource_manager_->GetDrmDevice(display_)->DestroyPropertyBlob(
            mode_.blob_id);
      std::tie(ret, mode_.blob_id) = CreateModeBlob(mode_.mode);
      if (ret) {
        ALOGE("Failed to create mode blob for display %d", display_);
        return ret;
      }
      mode_.needs_modeset = true;
      return 0;
    default:
      ALOGE("Unknown composition type %d", composition->type());
      return -EINVAL;
  }

  return ret;
}

int DrmDisplayCompositor::TestComposition(DrmDisplayComposition *composition) {
  AutoLock lock(&lock_, __func__);
  if (lock.Lock()) return -EDEADLK;
  int ret = CompletePendingFrame();
  return ret ? ret : CommitFrame(composition, true);
}

// Flatten a scene on the display by using a writeback connector
// and returns the composition result as a DrmHwcLayer.
int DrmDisplayCompositor::FlattenOnDisplay(
    std::unique_ptr<DrmDisplayComposition> &src, DrmConnector *writeback_conn,
    DrmMode &src_mode, DrmHwcLayer *writeback_layer) {
  int ret = 0;
  DrmDevice *drm = resource_manager_->GetDrmDevice(display_);
  ret = writeback_conn->UpdateModes();
  if (ret) {
    ALOGE("Failed to update modes %d", ret);
    return ret;
  }
  for (const DrmMode &mode : writeback_conn->modes()) {
    if (mode.h_display() == src_mode.h_display() &&
        mode.v_display() == src_mode.v_display()) {
      mode_.mode = mode;
      if (mode_.blob_id)
        drm->DestroyPropertyBlob(mode_.blob_id);
      std::tie(ret, mode_.blob_id) = CreateModeBlob(mode_.mode);
      if (ret) {
        ALOGE("Failed to create mode blob for display %d", display_);
        return ret;
      }
      mode_.needs_modeset = true;
      break;
    }
  }
  if (mode_.blob_id <= 0) {
    ALOGE("Failed to find similar mode");
    return -EINVAL;
  }

  DrmCrtc *crtc = drm->GetCrtcForDisplay(display_);
  if (!crtc) {
    ALOGE("Failed to find crtc for display %d", display_);
    return -EINVAL;
  }
  // TODO what happens if planes could go to both CRTCs, I don't think it's
  // handled anywhere
  std::vector<DrmPlane *> primary_planes;
  std::vector<DrmPlane *> overlay_planes;
  for (auto &plane : drm->planes()) {
    if (!plane->GetCrtcSupported(*crtc))
      continue;
    if (plane->type() == DRM_PLANE_TYPE_PRIMARY)
      primary_planes.push_back(plane.get());
    else if (plane->type() == DRM_PLANE_TYPE_OVERLAY)
      overlay_planes.push_back(plane.get());
  }

  ret = src->Plan(&primary_planes, &overlay_planes);
  if (ret) {
    ALOGE("Failed to plan the composition ret = %d", ret);
    return ret;
  }

  // Disable the planes we're not using
  for (auto i = primary_planes.begin(); i != primary_planes.end();) {
    src->AddPlaneDisable(*i);
    i = primary_planes.erase(i);
  }
  for (auto i = overlay_planes.begin(); i != overlay_planes.end();) {
    src->AddPlaneDisable(*i);
    i = overlay_planes.erase(i);
  }

  AutoLock lock(&lock_, __func__);
  ret = lock.Lock();
  if (ret)
    return ret;
  DrmFramebuffer *writeback_fb = &framebuffers_[framebuffer_index_];
  framebuffer_index_ = (framebuffer_index_ + 1) % DRM_DISPLAY_BUFFERS;
  if (!writeback_fb->Allocate(mode_.mode.h_display(), mode_.mode.v_display())) {
    ALOGE("Failed to allocate writeback buffer");
    return -ENOMEM;
  }
  DrmHwcBuffer *writeback_buffer = &writeback_layer->buffer;
  writeback_layer->sf_handle = writeback_fb->buffer()->handle;
  ret = writeback_layer->ImportBuffer(
      resource_manager_->GetImporter(display_).get());
  if (ret) {
    ALOGE("Failed to import writeback buffer");
    return ret;
  }

  ret = CommitFrame(src.get(), true, writeback_conn, writeback_buffer);
  if (ret) {
    ALOGE("Atomic check failed");
    return ret;
  }
  ret = CommitFrame(src.get(), false, writeback_conn, writeback_buffer);
  if (ret) {
    ALOGE("Atomic commit failed");
    return ret;
  }

  ret = sync_wait(writeback_fence_, kWaitWritebackFence);
  writeback_layer->acquire_fence.Set(writeback_fence_);
  writeback_fence_ = -1;
  if (ret) {
    ALOGE("Failed to wait on writeback fence");
    return ret;
  }
  return 0;
}

// Flatten a scene by enabling the writeback connector attached
// to the same CRTC as the one driving the display.
int DrmDisplayCompositor::FlattenSerial(DrmConnector *writeback_conn) {
  ALOGV("FlattenSerial by enabling writeback connector to the same crtc");
  // Flattened composition with only one layer that is obtained
  // using the writeback connector
  std::unique_ptr<DrmDisplayComposition>
      writeback_comp = CreateInitializedComposition();
  if (!writeback_comp)
    return -EINVAL;

  AutoLock lock(&lock_, __func__);
  int ret = lock.Lock();
  if (ret)
    return ret;
  if (!CountdownExpired() || active_composition_->layers().size() < 2) {
    ALOGV("Flattening is not needed");
    return -EALREADY;
  }

  DrmFramebuffer *writeback_fb = &framebuffers_[framebuffer_index_];
  framebuffer_index_ = (framebuffer_index_ + 1) % DRM_DISPLAY_BUFFERS;
  lock.Unlock();

  if (!writeback_fb->Allocate(mode_.mode.h_display(), mode_.mode.v_display())) {
    ALOGE("Failed to allocate writeback buffer");
    return -ENOMEM;
  }
  writeback_comp->layers().emplace_back();

  DrmHwcLayer &writeback_layer = writeback_comp->layers().back();
  writeback_layer.sf_handle = writeback_fb->buffer()->handle;
  writeback_layer.source_crop = {0, 0, (float)mode_.mode.h_display(),
                                 (float)mode_.mode.v_display()};
  writeback_layer.display_frame = {0, 0, (int)mode_.mode.h_display(),
                                   (int)mode_.mode.v_display()};
  ret = writeback_layer.ImportBuffer(
      resource_manager_->GetImporter(display_).get());
  if (ret || writeback_comp->layers().size() != 1) {
    ALOGE("Failed to import writeback buffer");
    return ret;
  }

  drmModeAtomicReqPtr pset = drmModeAtomicAlloc();
  if (!pset) {
    ALOGE("Failed to allocate property set");
    return -ENOMEM;
  }
  DrmDevice *drm = resource_manager_->GetDrmDevice(display_);
  DrmCrtc *crtc = drm->GetCrtcForDisplay(display_);
  if (!crtc) {
    ALOGE("Failed to find crtc for display %d", display_);
    return -EINVAL;
  }
  ret = SetupWritebackCommit(pset, crtc->id(), writeback_conn,
                             &writeback_layer.buffer);
  if (ret < 0) {
    ALOGE("Failed to Setup Writeback Commit");
    return ret;
  }
  ret = drmModeAtomicCommit(drm->fd(), pset, 0, drm);
  if (ret) {
    ALOGE("Failed to enable writeback %d", ret);
    return ret;
  }
  ret = sync_wait(writeback_fence_, kWaitWritebackFence);
  writeback_layer.acquire_fence.Set(writeback_fence_);
  writeback_fence_ = -1;
  if (ret) {
    ALOGE("Failed to wait on writeback fence");
    return ret;
  }

  DrmCompositionPlane squashed_comp(DrmCompositionPlane::Type::kLayer, NULL,
                                    crtc);
  for (auto &drmplane : drm->planes()) {
    if (!drmplane->GetCrtcSupported(*crtc))
      continue;
    if (!squashed_comp.plane() && drmplane->type() == DRM_PLANE_TYPE_PRIMARY)
      squashed_comp.set_plane(drmplane.get());
    else
      writeback_comp->AddPlaneDisable(drmplane.get());
  }
  squashed_comp.source_layers().push_back(0);
  ret = writeback_comp->AddPlaneComposition(std::move(squashed_comp));
  if (ret) {
    ALOGE("Failed to add flatten scene");
    return ret;
  }

  ApplyFrame(std::move(writeback_comp), 0, true);
  return 0;
}

// Flatten a scene by using a crtc which works concurrent with
// the one driving the display.
int DrmDisplayCompositor::FlattenConcurrent(DrmConnector *writeback_conn) {
  ALOGV("FlattenConcurrent by using an unused crtc/display");
  int ret = 0;
  DrmDisplayCompositor drmdisplaycompositor;
  ret = drmdisplaycompositor.Init(resource_manager_, writeback_conn->display());
  if (ret) {
    ALOGE("Failed to init  drmdisplaycompositor = %d", ret);
    return ret;
  }
  // Copy of the active_composition, needed because of two things:
  // 1) Not to hold the lock for the whole time we are accessing
  //    active_composition
  // 2) It will be committed on a crtc that might not be on the same
  //     dri node, so buffers need to be imported on the right node.
  std::unique_ptr<DrmDisplayComposition>
      copy_comp = drmdisplaycompositor.CreateInitializedComposition();

  // Writeback composition that will be committed to the display.
  std::unique_ptr<DrmDisplayComposition>
      writeback_comp = CreateInitializedComposition();

  if (!copy_comp || !writeback_comp)
    return -EINVAL;
  AutoLock lock(&lock_, __func__);
  ret = lock.Lock();
  if (ret)
    return ret;
  if (!CountdownExpired() || active_composition_->layers().size() < 2) {
    ALOGV("Flattening is not needed");
    return -EALREADY;
  }
  DrmCrtc *crtc = active_composition_->crtc();

  std::vector<DrmHwcLayer> copy_layers;
  for (DrmHwcLayer &src_layer : active_composition_->layers()) {
    DrmHwcLayer copy;
    ret = copy.InitFromDrmHwcLayer(&src_layer,
                                   resource_manager_
                                       ->GetImporter(writeback_conn->display())
                                       .get());
    if (ret) {
      ALOGE("Failed to import buffer ret = %d", ret);
      return -EINVAL;
    }
    copy_layers.emplace_back(std::move(copy));
  }
  ret = copy_comp->SetLayers(copy_layers.data(), copy_layers.size(), true);
  if (ret) {
    ALOGE("Failed to set copy_comp layers");
    return ret;
  }

  lock.Unlock();
  DrmHwcLayer writeback_layer;
  ret = drmdisplaycompositor.FlattenOnDisplay(copy_comp, writeback_conn,
                                              mode_.mode, &writeback_layer);
  if (ret) {
    ALOGE("Failed to flatten on display ret = %d", ret);
    return ret;
  }

  DrmCompositionPlane squashed_comp(DrmCompositionPlane::Type::kLayer, NULL,
                                    crtc);
  for (auto &drmplane : resource_manager_->GetDrmDevice(display_)->planes()) {
    if (!drmplane->GetCrtcSupported(*crtc))
      continue;
    if (drmplane->type() == DRM_PLANE_TYPE_PRIMARY)
      squashed_comp.set_plane(drmplane.get());
    else
      writeback_comp->AddPlaneDisable(drmplane.get());
  }
  writeback_comp->layers().emplace_back();
  DrmHwcLayer &next_layer = writeback_comp->layers().back();
  next_layer.sf_handle = writeback_layer.get_usable_handle();
  next_layer.blending = DrmHwcBlending::kPreMult;
  next_layer.source_crop = {0, 0, (float)mode_.mode.h_display(),
                            (float)mode_.mode.v_display()};
  next_layer.display_frame = {0, 0, (int)mode_.mode.h_display(),
                              (int)mode_.mode.v_display()};
  ret = next_layer.ImportBuffer(resource_manager_->GetImporter(display_).get());
  if (ret) {
    ALOGE("Failed to import framebuffer for display %d", ret);
    return ret;
  }
  squashed_comp.source_layers().push_back(0);
  ret = writeback_comp->AddPlaneComposition(std::move(squashed_comp));
  if (ret) {
    ALOGE("Failed to add plane composition %d", ret);
    return ret;
  }
  ApplyFrame(std::move(writeback_comp), 0, true);
  return ret;
}

int DrmDisplayCompositor::FlattenActiveComposition() {
  DrmConnector *writeback_conn = resource_manager_->AvailableWritebackConnector(
      display_);
  if (!active_composition_ || !writeback_conn) {
    ALOGV("No writeback connector available");
    return -EINVAL;
  }

  if (writeback_conn->display() != display_) {
    return FlattenConcurrent(writeback_conn);
  } else {
    return FlattenSerial(writeback_conn);
  }

  return 0;
}

bool DrmDisplayCompositor::CountdownExpired() const {
  return flatten_countdown_ <= 0;
}

void DrmDisplayCompositor::Vsync(int display, int64_t timestamp) {
  AutoLock lock(&lock_, __func__);
  if (lock.Lock())
    return;
  if (pending_latch_fence_.get() >= 0 && CompletePendingFrame(0)) return;
  // Callback follows a real drmWaitVBlank. Hold the same lock as ApplyFrame:
  // no commit, swap or retirement can race this read of the active composition.
  // Failed/absent fences still consume an attempt and skip all mapping.
  if (display == 0 && display_ == 0 &&
      superman_current_target_trace::Begin(true)) {
    int fence = active_composition_ ?
        active_composition_->diagnostic_latch_fence() : -1;
    superman_current_target_trace::Snapshot(active_composition_.get(), "current",
                                            lab_.latched_frames, fence,
                                            timestamp > 0 ? uint64_t(timestamp) : 0);
  }
  flatten_countdown_--;
  if (!CountdownExpired())
    return;
  lock.Unlock();
  int ret = FlattenActiveComposition();
  ALOGV("scene flattening triggered for display %d at timestamp %" PRIu64
        " result = %d \n",
        display, timestamp, ret);
}

void DrmDisplayCompositor::Dump(std::ostringstream *out) const {
  int ret = pthread_mutex_lock(&lock_);
  if (ret)
    return;

  uint64_t num_frames = dump_frames_composited_;
  dump_frames_composited_ = 0;

  struct timespec ts;
  ret = clock_gettime(CLOCK_MONOTONIC, &ts);
  if (ret) {
    pthread_mutex_unlock(&lock_);
    return;
  }

  uint64_t cur_ts = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
  uint64_t num_ms = (cur_ts - dump_last_timestamp_ns_) / (1000 * 1000);
  float fps = num_ms ? (num_frames * 1000.0f) / (num_ms) : 0.0f;

  *out << "--DrmDisplayCompositor[" << display_
       << "]: num_frames=" << num_frames << " num_ms=" << num_ms
       << " fps=" << fps << "\n";

  dump_last_timestamp_ns_ = cur_ts;

  pthread_mutex_unlock(&lock_);
}
}  // namespace android
