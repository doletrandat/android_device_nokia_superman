# SPDX-License-Identifier: Apache-2.0
# Nokia Lumia 730 (Superman) bring-up product.

PRODUCT_COPY_FILES += \
    device/nokia/superman/prebuilt/zImage-dtb:kernel \
    device/nokia/superman/fstab.superman:$(TARGET_COPY_OUT_RAMDISK)/fstab.superman \
    device/nokia/superman/fstab.superman:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.superman \
    device/nokia/superman/init.superman.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.superman.rc \
    device/nokia/superman/init.recovery.superman.rc:recovery/root/init.recovery.superman.rc \
    device/nokia/superman/prebuilt/reboot-mode.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/reboot-mode.ko \
    device/nokia/superman/prebuilt/qcom-pon.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/qcom-pon.ko \
    device/nokia/superman/prebuilt/pm8941-pwrkey.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/pm8941-pwrkey.ko

PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    persist.sys.usb.config=adb \
    ro.adb.secure=0 \
    ro.sf.lcd_density=320

PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator@2.0-impl \
    android.hardware.graphics.allocator@2.0-service \
    android.hardware.graphics.composer@2.1-impl \
    android.hardware.graphics.composer@2.1-service \
    android.hardware.graphics.mapper@2.0-impl \
    gralloc.default \
    hwcomposer.default \
    libGLES_android \
    toybox
