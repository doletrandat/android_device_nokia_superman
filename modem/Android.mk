# SPDX-License-Identifier: Apache-2.0
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := rild.superman
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_SRC_FILES := ../../../../hardware/ril/rild/rild.c
LOCAL_CFLAGS := -DRIL_SHLIB -Wall -Wextra -Werror
LOCAL_C_INCLUDES := hardware/ril/include hardware/ril
LOCAL_SHARED_LIBRARIES := libcutils libdl liblog libril
LOCAL_INIT_RC := superman-ril.rc
LOCAL_VINTF_FRAGMENTS := manifest-superman-cellular.xml
include $(BUILD_EXECUTABLE)
