# SPDX-License-Identifier: Apache-2.0
# Nokia Lumia 730 (Superman) bring-up product.

# Provide on-screen Back, Home and Recents on the Lumia display.
DEVICE_PACKAGE_OVERLAYS += device/nokia/superman/overlay

PRODUCT_COPY_FILES += \
    device/nokia/superman/init.superman.cpufreq.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.superman.cpufreq.rc \
    device/nokia/superman/prebuilt/zImage-dtb:kernel \
    device/nokia/superman/fstab.superman:$(TARGET_COPY_OUT_RAMDISK)/fstab.superman \
    device/nokia/superman/fstab.superman:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.superman \
    device/nokia/superman/fstab.superman.late:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.superman.late \
    device/nokia/superman/init.superman.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.superman.rc \
    device/nokia/superman/diagnostics/superman-diag-capture.sh:$(TARGET_COPY_OUT_VENDOR)/bin/superman-diag-capture.sh \
    device/nokia/superman/diagnostics/superman-reboot-capture.sh:$(TARGET_COPY_OUT_VENDOR)/bin/superman-reboot-capture.sh \
    device/nokia/superman/diagnostics/superman-storage-capture-mode.sh:$(TARGET_COPY_OUT_VENDOR)/bin/superman-storage-capture-mode.sh \
    device/nokia/superman/diagnostics/superman-memdiag.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/superman-memdiag.rc \
    device/nokia/superman/diagnostics/superman-bootanim.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/superman-bootanim.rc \
    device/nokia/superman/init.recovery.superman.rc:recovery/root/init.recovery.superman.rc \
    device/nokia/superman/prebuilt/power-p15/reboot-mode.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/reboot-mode.ko \
    device/nokia/superman/prebuilt/power-p15/qcom-pon.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/qcom-pon.ko \
    device/nokia/superman/prebuilt/power-p15/pm8941-pwrkey.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/pm8941-pwrkey.ko \
    device/nokia/superman/diagnostics/superman-power-modules.json:$(TARGET_COPY_OUT_VENDOR)/etc/superman-power-modules.json

# Rebuild the proven r37 touch sources for P15, preserving the r36 transport delay.
PRODUCT_COPY_FILES += \
    device/nokia/superman/prebuilt/touch-p15/rmi_core.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/rmi_core.ko \
    device/nokia/superman/prebuilt/touch-p15/rmi_i2c.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/rmi_i2c.ko \
    device/nokia/superman/diagnostics/superman-touch-modules.json:$(TARGET_COPY_OUT_VENDOR)/etc/superman-touch-modules.json

PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    persist.sys.usb.config=adb \
    ro.adb.secure=0 \
    ro.sf.lcd_density=320

PRODUCT_PROPERTY_OVERRIDES += \
    ro.surface_flinger.has_wide_color_display=false \
    ro.surface_flinger.has_HDR_display=false \
    ro.superman.start_wifi_framework=true \
    ro.superman.unknown_battery_capacity=true \
    ro.superman.power_stats_unavailable=true \
    ro.superman.art_diag=1 \
    ro.superman.native_diag=1 \
    debug.superman.cpu_fb_copy=1 \
    ro.superman.power_diag=true \
    ro.superman.long_screen_timeout=false \
    ro.superman.bootanim_low_fps=true \
    ro.superman.zip_diag=true \
    sys.use_memfd=true \
    ro.superman.disable_bpf=true \
    ro.superman.allow_missing_nflog=true

PRODUCT_REQUIRES_INSECURE_EXECMEM_FOR_SWIFTSHADER := true

PRODUCT_COPY_FILES += \
    device/nokia/superman/diagnostics/mediaextractor.policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/mediaextractor.policy \
    device/nokia/superman/diagnostics/privapp-permissions-superman.xml:$(TARGET_COPY_OUT_SYSTEM)/etc/permissions/privapp-permissions-superman.xml

# LocationManagerService requires a direct-boot-aware fused provider at phase 600.
PRODUCT_PACKAGES += FusedLocation

# Launcher3 enumerates widget providers during workspace loading. Without this
# software feature SystemServer omits AppWidgetService, leaving its manager null.
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.software.app_widgets.xml:$(TARGET_COPY_OUT_SYSTEM)/etc/permissions/android.software.app_widgets.xml

# BatteryService requires a registered IHealth/default. The service module
# installs its init rc and VINTF fragment; it dlopens the separate 2.1 impl.
# Keep the default sysfs-backed implementation (no fabricated battery values).
PRODUCT_PACKAGES += \
    android.hardware.health@2.1-service \
    android.hardware.health@2.1-impl \
    android.hardware.keymaster@4.1-service \
    android.hardware.audio.service \
    android.hardware.audio@6.0-impl \
    android.hardware.audio.effect@6.0-impl \
    audio.primary.default \
    audio.r_submix.default \
    android.hardware.graphics.allocator@2.0-impl \
    android.hardware.graphics.allocator@2.0-service \
    android.hardware.graphics.composer@2.1-service \
    android.hardware.graphics.mapper@2.0-impl \
    gralloc.default \
    libEGL_swiftshader \
    libGLESv1_CM_swiftshader \
    libGLESv2_swiftshader \
    libRS \
    librs_jni \
    superman-memdiag \
    superman-healthdiag \
    superman-kerneldiag \
    toybox

# Reference audio plumbing for framework bring-up; no physical codec support.
PRODUCT_COPY_FILES += \
    device/nokia/superman/audio/bluetooth/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml \
    frameworks/av/services/audiopolicy/config/surround_sound_configuration_5_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/surround_sound_configuration_5_0.xml

# Mainline UDC + FunctionFS support without claiming USB host/accessory hardware.
PRODUCT_PROPERTY_OVERRIDES += ro.superman.mainline_usb=true
PRODUCT_COPY_FILES += \
    device/nokia/superman/init.superman.usb.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.superman.usb.rc \
    device/nokia/superman/diagnostics/superman-usb-config.sh:$(TARGET_COPY_OUT_VENDOR)/bin/superman-usb-config.sh

# OMX store needs a vendor codec catalogue even with the software Codec2 APEX.
PRODUCT_COPY_FILES += \
    device/nokia/superman/media_codecs.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_audio.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_audio.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_video.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_video.xml

# This mainline kernel has no /dev/ion. Codec2 linear blocks come from the gralloc
# BLOB allocator instead (C2PlatformAllocatorStore BLOB=19 | BUFFERQUEUE=18), which
# gralloc.superman_native implements as linear R8 buffers mapped through DMA-BUF.
# Bits 20/21 (V4L2_BUFFERQUEUE/V4L2_BUFFERPOOL, v4l2_codec2 V4L2AllocatorId) let the
# Venus V4L2 Codec2 HAL create its own output pools; without them the framework falls
# back to the basic gralloc pool, which v4l2_codec2's VideoFramePool rejects.
PRODUCT_PROPERTY_OVERRIDES += debug.stagefright.c2-poolmask=3932160

# Reduce cold SwiftShader JIT latency on ARM32; compare steady-state rendering too.
# Set to 0 and restart graphics clients to restore the original codegen policy.
PRODUCT_PROPERTY_OVERRIDES += debug.superman.swiftshader_fast_jit=1

# Matched P26 render-only modules and firmware for P29 early startup validation.
PRODUCT_COPY_FILES += \
    device/nokia/superman/prebuilt/gpu-p26/drm_client_lib.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/gpu-diag/drm_client_lib.ko \
    device/nokia/superman/prebuilt/gpu-p26/drm_exec.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/gpu-diag/drm_exec.ko \
    device/nokia/superman/prebuilt/gpu-p26/gpu-sched.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/gpu-diag/gpu-sched.ko \
    device/nokia/superman/prebuilt/gpu-p26/msm.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/gpu-diag/msm.ko \
    device/nokia/superman/prebuilt/gpu-p14/firmware/LICENSE.qcom:$(TARGET_COPY_OUT_VENDOR)/firmware/LICENSE.qcom \
    device/nokia/superman/prebuilt/gpu-p14/firmware/WHENCE:$(TARGET_COPY_OUT_VENDOR)/firmware/WHENCE \
    device/nokia/superman/prebuilt/gpu-p14/firmware/qcom/a330_pfp.fw:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/a330_pfp.fw \
    device/nokia/superman/prebuilt/gpu-p14/firmware/qcom/a330_pm4.fw:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/a330_pm4.fw \
    device/nokia/superman/diagnostics/superman-integrated-source-provenance.json:$(TARGET_COPY_OUT_VENDOR)/etc/superman-integrated-source-provenance.json

# Retain Mesa and diagnostics; post-fs selects the coherent graphics stack.
PRODUCT_SOONG_NAMESPACES += external/mesa3d
PRODUCT_PACKAGES += libGLES_mesa gallium_dri libgbm_mesa superman-gpu-smoke superman-gpu-buffer-test superman-gpu-window-test superman-gpu-queue-test

# P29 can select this bridge after GPU preflight; default gralloc is retained.
PRODUCT_PACKAGES += gralloc.superman_drm

# BGRA simplefb client-target EGL configs; RGBA configs remain first.
PRODUCT_PROPERTY_OVERRIDES += ro.superman.bgra_window_configs=1

# Explicit standard-HAL/Binder interoperability diagnostic; no automatic GPU selection.
PRODUCT_PACKAGES += libsuperman_graphics_hal superman-gpu-binder-test

# P29 selects the coherent EGL/gralloc/SF target format before graphics starts.
# Driver readiness failure selects the retained software stack on the same boot.
PRODUCT_PROPERTY_OVERRIDES += ro.superman.gpu_candidate=true
PRODUCT_COPY_FILES += \
    device/nokia/superman/diagnostics/superman-gpu-startup.sh:$(TARGET_COPY_OUT_VENDOR)/bin/superman-gpu-startup.sh \
    device/nokia/superman/diagnostics/superman-gpu-startup.sha256:$(TARGET_COPY_OUT_VENDOR)/etc/superman-gpu-startup.sha256
PRODUCT_PACKAGES += superman-gpu-startup-check superman-hwc-bridge-test

# The selected gralloc adapter implements mapper2; skip absent mapper3/4 probes.
PRODUCT_SYSTEM_DEFAULT_PROPERTIES += ro.superman.mapper2_only=true

# P74 diagnostic image: retain bounded A305B CP progress in existing fault dumps.
PRODUCT_SYSTEM_DEFAULT_PROPERTIES += ro.superman.gpu_progress=1

# Publish calibrated battery current through the existing Health HAL.
$(call inherit-product, device/nokia/superman/power/current/product.mk)
$(call inherit-product, device/nokia/superman/power/charger/product.mk)
$(call inherit-product, device/nokia/superman/power/cpuidle/product.mk)
$(call inherit-product, device/nokia/superman/power/cpufreq/product.mk)
$(call inherit-product, device/nokia/superman/memory/zram/product.mk)
$(call inherit-product, device/nokia/superman/graphics/native-kms/product.mk)
$(call inherit-product, device/nokia/superman/audio/bluetooth/product.mk)

# Venus hardware video codec via the V4L2 Codec2 HAL (IComponentStore/default).
# The Venus kernel modules are loaded by media/venus/init (superman-venus.rc).
PRODUCT_PACKAGES += \
    android.hardware.media.c2@1.0-service-v4l2 \
    libv4l2_codec2_components \
    libstagefright_bufferqueue_helper_novndk \
    android.hardware.media.bufferpool@1.0
PRODUCT_COPY_FILES += \
    device/nokia/superman/media/venus/c2/media_codecs_c2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2.xml \
    device/nokia/superman/media/venus/c2/codec2.vendor.ext.policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/codec2.vendor.ext.policy \
    device/nokia/superman/media/venus/c2/android.hardware.media.c2-v4l2-seccomp_policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/android.hardware.media.c2-v4l2-seccomp_policy

# Flashlight: torch-only camera HAL (camera.superman) behind the legacy camera
# provider. The flash feature makes SystemUI show the Flashlight tile.
PRODUCT_PACKAGES += \
    camera.superman \
    android.hardware.camera.provider@2.4-impl \
    android.hardware.camera.provider@2.4-service
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.camera.flash-autofocus.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.flash-autofocus.xml

# GPS: legacy gps.superman module (QMI LOC over QRTR, gnss/) behind the AOSP
# GNSS 1.0 passthrough service. Standalone only; no SUPL/XTRA.
PRODUCT_PACKAGES += \
    gps.superman \
    android.hardware.gnss@1.0-impl \
    android.hardware.gnss@1.0-service \
    superman-gnss-test
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.location.gps.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.location.gps.xml
