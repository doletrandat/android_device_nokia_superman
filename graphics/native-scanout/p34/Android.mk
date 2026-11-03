LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := gralloc.superman_native
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_PROPRIETARY_MODULE := true
LOCAL_SRC_FILES := gralloc/gralloc.cpp gralloc/gralloc_gbm.cpp gralloc/framebuffer.cpp
LOCAL_SHARED_LIBRARIES := libdrm libgbm_mesa liblog libcutils libhardware
LOCAL_C_INCLUDES := external/libdrm external/mesa3d/src/gbm/main
LOCAL_CFLAGS := -Wall -Wextra -Wno-unused-parameter
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := superman-native-kms-check
LOCAL_PROPRIETARY_MODULE := true
LOCAL_SRC_FILES := native-kms-check.cpp
LOCAL_SHARED_LIBRARIES := libdrm libgbm_mesa libsync libcutils libdl libhardware
LOCAL_C_INCLUDES := external/libdrm external/mesa3d/src/gbm/main frameworks/native/opengl/include
LOCAL_CFLAGS := -Wall -Wextra -Werror
include $(BUILD_EXECUTABLE)
