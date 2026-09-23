# SPDX-License-Identifier: Apache-2.0
# Nokia Lumia 730 (Superman) bring-up product.

PRODUCT_COPY_FILES += \
    device/nokia/superman/prebuilt/zImage-dtb:kernel \
    device/nokia/superman/fstab.superman:$(TARGET_COPY_OUT_RAMDISK)/fstab.superman \
    device/nokia/superman/fstab.superman:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.superman \
    device/nokia/superman/fstab.superman.late:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.superman.late \
    device/nokia/superman/init.superman.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.superman.rc \
    device/nokia/superman/init.recovery.superman.rc:recovery/root/init.recovery.superman.rc \
    device/nokia/superman/prebuilt/power-r35/reboot-mode.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/reboot-mode.ko \
    device/nokia/superman/prebuilt/power-r35/qcom-pon.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/qcom-pon.ko \
    device/nokia/superman/prebuilt/power-r35/pm8941-pwrkey.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/pm8941-pwrkey.ko

# Preserve the exact hardware-tested r37 pair, including its r36 transport.
PRODUCT_COPY_FILES += \
    device/nokia/superman/prebuilt/touch-r33/rmi_core.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/rmi_core.ko \
    device/nokia/superman/prebuilt/touch-r36/rmi_i2c.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/rmi_i2c.ko

PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    persist.sys.usb.config=adb \
    ro.adb.secure=0 \
    ro.sf.lcd_density=320

PRODUCT_PROPERTY_OVERRIDES += \
    ro.surface_flinger.has_wide_color_display=false \
    ro.surface_flinger.has_HDR_display=false \
    ro.superman.start_wifi_framework=true \
    ro.superman.unknown_battery_capacity=true \
    ro.superman.art_diag=1 \
    ro.superman.native_diag=1 \
    ro.hardware.egl=swiftshader \
    debug.superman.cpu_fb_copy=1 \
    ro.superman.power_diag=true \
    ro.superman.long_screen_timeout=true \
    ro.superman.bootanim_low_fps=true \
    ro.superman.zip_diag=true \
    sys.use_memfd=true \
    ro.superman.disable_bpf=true \
    ro.superman.allow_missing_nflog=true

PRODUCT_REQUIRES_INSECURE_EXECMEM_FOR_SWIFTSHADER := true

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
    android.hardware.graphics.composer@2.1-impl \
    android.hardware.graphics.composer@2.1-service \
    android.hardware.graphics.mapper@2.0-impl \
    gralloc.default \
    libEGL_swiftshader \
    libGLESv1_CM_swiftshader \
    libGLESv2_swiftshader \
    libRS \
    librs_jni \
    toybox

# Reference audio plumbing for framework bring-up; no physical codec support.
PRODUCT_COPY_FILES += \
    frameworks/av/services/audiopolicy/config/audio_policy_configuration_generic.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml \
    frameworks/av/services/audiopolicy/config/surround_sound_configuration_5_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/surround_sound_configuration_5_0.xml
