/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lumia 730 (superman) torch-only camera HAL module.
 *
 * There is no working camera pipeline yet (CAMSS for MSM8226 is still being
 * ported), but Android routes the flashlight through the camera service:
 * CameraManager.setTorchMode() -> cameraserver -> camera provider -> HAL.
 * This module reports one back-facing camera with a flash unit and implements
 * only set_torch_mode(), driving the gpio-leds "white:torch" LED (GPIO 32).
 * Opening the camera for capture fails with -ENODEV.
 */

#define LOG_TAG "superman.camera.torch"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include <hardware/camera_common.h>
#include <hardware/hardware.h>
#include <log/log.h>
#include <system/camera_metadata.h>
#include <system/graphics.h>

namespace {

constexpr const char* kTorchBrightness = "/sys/class/leds/white:torch/brightness";
constexpr const char* kCameraId = "0";
constexpr int32_t kWidth = 1280;
constexpr int32_t kHeight = 720;

pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
const camera_module_callbacks_t* gCallbacks = nullptr;
camera_metadata_t* gStatic = nullptr;
bool gTorchOn = false;

int writeTorch(bool on) {
    int fd = open(kTorchBrightness, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        int err = errno;
        ALOGE("open %s: %s", kTorchBrightness, strerror(err));
        return -err;
    }
    const char* value = on ? "1\n" : "0\n";
    ssize_t n = write(fd, value, 2);
    int err = errno;
    close(fd);
    if (n != 2) {
        ALOGE("write %s: %s", kTorchBrightness, strerror(err));
        return -err;
    }
    return 0;
}

bool torchPresent() { return access(kTorchBrightness, F_OK) == 0; }

class MetaBuilder {
  public:
    MetaBuilder() : mMeta(allocate_camera_metadata(64, 2048)) {}

    template <typename T>
    void add(uint32_t tag, const std::vector<T>& v) {
        if (add_camera_metadata_entry(mMeta, tag, v.data(), v.size()) != 0) {
            ALOGE("failed to add tag 0x%x", tag);
            return;
        }
        mKeys.push_back(static_cast<int32_t>(tag));
    }
    void u8(uint32_t tag, std::vector<uint8_t> v) { add(tag, v); }
    void i32(uint32_t tag, std::vector<int32_t> v) { add(tag, v); }
    void i64(uint32_t tag, std::vector<int64_t> v) { add(tag, v); }
    void f(uint32_t tag, std::vector<float> v) { add(tag, v); }

    camera_metadata_t* finish() {
        mKeys.push_back(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
        add_camera_metadata_entry(mMeta, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
                                  mKeys.data(), mKeys.size());
        sort_camera_metadata(mMeta);
        return mMeta;
    }

  private:
    camera_metadata_t* mMeta;
    std::vector<int32_t> mKeys;
};

camera_metadata_t* buildStaticMetadata() {
    MetaBuilder m;
    m.u8(ANDROID_LENS_FACING, {ANDROID_LENS_FACING_BACK});
    m.i32(ANDROID_SENSOR_ORIENTATION, {90});
    m.u8(ANDROID_FLASH_INFO_AVAILABLE, {ANDROID_FLASH_INFO_AVAILABLE_TRUE});
    m.u8(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, {ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY});
    m.u8(ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
         {ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE});
    m.i32(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, {0, 0, kWidth, kHeight});
    m.i32(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, {kWidth, kHeight});
    m.f(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, {1.0f});
    m.u8(ANDROID_SCALER_CROPPING_TYPE, {ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY});

    const int32_t out = ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT;
    m.i32(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
          {HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, kWidth, kHeight, out,
           HAL_PIXEL_FORMAT_YCbCr_420_888, kWidth, kHeight, out,
           HAL_PIXEL_FORMAT_BLOB, kWidth, kHeight, out});
    const int64_t frame = 33333333;
    m.i64(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
          {HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, kWidth, kHeight, frame,
           HAL_PIXEL_FORMAT_YCbCr_420_888, kWidth, kHeight, frame,
           HAL_PIXEL_FORMAT_BLOB, kWidth, kHeight, frame});
    m.i64(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
          {HAL_PIXEL_FORMAT_BLOB, kWidth, kHeight, frame});
    m.i32(ANDROID_JPEG_MAX_SIZE, {kWidth * kHeight * 3 / 2});

    m.i32(ANDROID_REQUEST_PARTIAL_RESULT_COUNT, {1});
    m.u8(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, {4});
    m.i32(ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, {0, 2, 1});
    m.i32(ANDROID_SYNC_MAX_LATENCY, {ANDROID_SYNC_MAX_LATENCY_UNKNOWN});

    m.u8(ANDROID_CONTROL_AVAILABLE_MODES, {ANDROID_CONTROL_MODE_AUTO});
    m.u8(ANDROID_CONTROL_AVAILABLE_SCENE_MODES, {ANDROID_CONTROL_SCENE_MODE_DISABLED});
    m.u8(ANDROID_CONTROL_AVAILABLE_EFFECTS, {ANDROID_CONTROL_EFFECT_MODE_OFF});
    m.u8(ANDROID_CONTROL_AE_AVAILABLE_MODES, {ANDROID_CONTROL_AE_MODE_ON});
    m.u8(ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
         {ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO});
    m.i32(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, {15, 30, 30, 30});
    m.i32(ANDROID_CONTROL_AE_COMPENSATION_RANGE, {0, 0});
    m.i32(ANDROID_CONTROL_AE_COMPENSATION_STEP, {1, 1});
    m.u8(ANDROID_CONTROL_AF_AVAILABLE_MODES, {ANDROID_CONTROL_AF_MODE_OFF});
    m.u8(ANDROID_CONTROL_AWB_AVAILABLE_MODES, {ANDROID_CONTROL_AWB_MODE_AUTO});
    m.i32(ANDROID_CONTROL_MAX_REGIONS, {0, 0, 0});
    m.u8(ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
         {ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF});

    m.i32(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS,
          {ANDROID_CONTROL_MODE, ANDROID_CONTROL_AE_MODE, ANDROID_FLASH_MODE});
    m.i32(ANDROID_REQUEST_AVAILABLE_RESULT_KEYS,
          {ANDROID_CONTROL_MODE, ANDROID_CONTROL_AE_MODE, ANDROID_FLASH_MODE,
           ANDROID_FLASH_STATE, ANDROID_SENSOR_TIMESTAMP});
    return m.finish();
}

void notifyTorch(bool on) {
    if (gCallbacks && gCallbacks->torch_mode_status_change) {
        gCallbacks->torch_mode_status_change(
                gCallbacks, kCameraId,
                on ? TORCH_MODE_STATUS_AVAILABLE_ON : TORCH_MODE_STATUS_AVAILABLE_OFF);
    }
}

int getNumberOfCameras() { return torchPresent() ? 1 : 0; }

int getCameraInfo(int id, struct camera_info* info) {
    if (id != 0 || info == nullptr) return -EINVAL;
    pthread_mutex_lock(&gLock);
    if (gStatic == nullptr) gStatic = buildStaticMetadata();
    pthread_mutex_unlock(&gLock);
    memset(info, 0, sizeof(*info));
    info->facing = CAMERA_FACING_BACK;
    info->orientation = 90;
    info->device_version = CAMERA_DEVICE_API_VERSION_3_2;
    info->static_camera_characteristics = gStatic;
    info->resource_cost = 100;
    return 0;
}

int setCallbacks(const camera_module_callbacks_t* callbacks) {
    pthread_mutex_lock(&gLock);
    gCallbacks = callbacks;
    pthread_mutex_unlock(&gLock);
    return 0;
}

void getVendorTagOps(vendor_tag_ops_t* /*ops*/) {}

int setTorchMode(const char* cameraId, bool enabled) {
    if (cameraId == nullptr || strcmp(cameraId, kCameraId) != 0) return -EINVAL;
    pthread_mutex_lock(&gLock);
    int res = writeTorch(enabled);
    if (res == 0) gTorchOn = enabled;
    pthread_mutex_unlock(&gLock);
    if (res != 0) return -ENOSYS;
    ALOGI("torch %s", enabled ? "on" : "off");
    notifyTorch(enabled);
    return 0;
}

int init() {
    // Start from a known state; the LED keeps its value across HAL restarts.
    if (torchPresent()) writeTorch(false);
    return 0;
}

int openDevice(const hw_module_t* /*module*/, const char* /*id*/, hw_device_t** /*device*/) {
    ALOGW("capture is not supported yet (torch-only HAL)");
    return -ENODEV;
}

hw_module_methods_t gMethods = {
        .open = openDevice,
};

}  // namespace

extern "C" {
__attribute__((visibility("default"))) camera_module_t HAL_MODULE_INFO_SYM = {
        .common =
                {
                        .tag = HARDWARE_MODULE_TAG,
                        .module_api_version = CAMERA_MODULE_API_VERSION_2_4,
                        .hal_api_version = HARDWARE_HAL_API_VERSION,
                        .id = CAMERA_HARDWARE_MODULE_ID,
                        .name = "Lumia 730 torch-only camera HAL",
                        .author = "superman bring-up",
                        .methods = &gMethods,
                        .dso = nullptr,
                        .reserved = {0},
                },
        .get_number_of_cameras = getNumberOfCameras,
        .get_camera_info = getCameraInfo,
        .set_callbacks = setCallbacks,
        .get_vendor_tag_ops = getVendorTagOps,
        .open_legacy = nullptr,
        .set_torch_mode = setTorchMode,
        .init = init,
        .get_physical_camera_info = nullptr,
        .is_stream_combination_supported = nullptr,
        .notify_device_state_change = nullptr,
        .reserved = {nullptr},
};
}
