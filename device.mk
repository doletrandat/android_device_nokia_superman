# SPDX-License-Identifier: Apache-2.0
# Nokia Lumia 730 (Superman) bring-up product.

PRODUCT_COPY_FILES += \
    device/nokia/superman/prebuilt/zImage-dtb:kernel \
    device/nokia/superman/init.superman.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.superman.rc \
    device/nokia/superman/init.recovery.superman.rc:recovery/root/init.recovery.superman.rc

PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    persist.sys.usb.config=adb \
    ro.adb.secure=0

PRODUCT_PACKAGES += toybox
