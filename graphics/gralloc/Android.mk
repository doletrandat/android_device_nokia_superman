LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := gralloc.superman_drm
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_PROPRIETARY_MODULE := true
LOCAL_SRC_FILES := gralloc.cpp gralloc_gbm.cpp framebuffer.cpp
LOCAL_SHARED_LIBRARIES := libdrm libgbm_mesa liblog libcutils libhardware
LOCAL_C_INCLUDES := external/libdrm external/mesa3d/src/gbm/main
LOCAL_CFLAGS := -Wall -Wextra -Wno-unused-parameter
include $(BUILD_SHARED_LIBRARY)
