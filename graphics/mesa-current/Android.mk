# Mesa 26.2.5 / libdrm 2.4.134, rebuilt by rebuild.py and qualified on A305B.
LOCAL_PATH := $(call my-dir)
ifeq ($(SUPERMAN_MESA26_QUALIFIED),true)
include $(CLEAR_VARS)
LOCAL_MODULE := libGLES_mesa
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_STEM := libGLES_mesa
LOCAL_SRC_FILES := prebuilt/libGLES_mesa.so
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MULTILIB := 32
LOCAL_STRIP_MODULE := false
# Meson linked against Android 11 platform ABI; renamed module IDs differ from ELF SONAMEs.
LOCAL_CHECK_ELF_FILES := false
LOCAL_MODULE_RELATIVE_PATH := egl
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := libgbm_mesa
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_STEM := libgbm_mesa
LOCAL_SRC_FILES := prebuilt/libgbm_mesa.so
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MULTILIB := 32
LOCAL_STRIP_MODULE := false
# Meson linked against Android 11 platform ABI; renamed module IDs differ from ELF SONAMEs.
LOCAL_CHECK_ELF_FILES := false
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := gallium_dri
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_STEM := gallium_dri
LOCAL_SRC_FILES := prebuilt/libgallium_dri.so
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MULTILIB := 32
LOCAL_STRIP_MODULE := false
# Meson linked against Android 11 platform ABI; renamed module IDs differ from ELF SONAMEs.
LOCAL_CHECK_ELF_FILES := false
LOCAL_MODULE_RELATIVE_PATH := dri
LOCAL_MODULE_SYMLINKS := msm_dri.so
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := superman_mesa_gallium
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_STEM := libgallium_dri
LOCAL_SRC_FILES := prebuilt/libgallium_dri.so
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MULTILIB := 32
LOCAL_STRIP_MODULE := false
# Meson linked against Android 11 platform ABI; renamed module IDs differ from ELF SONAMEs.
LOCAL_CHECK_ELF_FILES := false
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := superman_mesa_egl
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_STEM := libEGL_mesa
LOCAL_SRC_FILES := prebuilt/libEGL_mesa.so
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MULTILIB := 32
LOCAL_STRIP_MODULE := false
# Meson linked against Android 11 platform ABI; renamed module IDs differ from ELF SONAMEs.
LOCAL_CHECK_ELF_FILES := false
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := superman_mesa_gles1
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_STEM := libGLESv1_CM_mesa
LOCAL_SRC_FILES := prebuilt/libGLESv1_CM_mesa.so
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MULTILIB := 32
LOCAL_STRIP_MODULE := false
# Meson linked against Android 11 platform ABI; renamed module IDs differ from ELF SONAMEs.
LOCAL_CHECK_ELF_FILES := false
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := superman_mesa_gles2
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_STEM := libGLESv2_mesa
LOCAL_SRC_FILES := prebuilt/libGLESv2_mesa.so
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MULTILIB := 32
LOCAL_STRIP_MODULE := false
# Meson linked against Android 11 platform ABI; renamed module IDs differ from ELF SONAMEs.
LOCAL_CHECK_ELF_FILES := false
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := superman_mesa_drm
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_STEM := libdrm
LOCAL_SRC_FILES := prebuilt/libdrm.so
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MULTILIB := 32
LOCAL_STRIP_MODULE := false
# Meson linked against Android 11 platform ABI; renamed module IDs differ from ELF SONAMEs.
LOCAL_CHECK_ELF_FILES := false
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := superman_mesa_gbm_backend
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_STEM := dri_gbm
LOCAL_SRC_FILES := prebuilt/gbm/dri_gbm.so
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MULTILIB := 32
LOCAL_STRIP_MODULE := false
# Meson linked against Android 11 platform ABI; renamed module IDs differ from ELF SONAMEs.
LOCAL_CHECK_ELF_FILES := false
LOCAL_MODULE_RELATIVE_PATH := gbm
include $(BUILD_PREBUILT)

endif
