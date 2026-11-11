# Native display: mainline msm KMS (cc5c: no-MMU VRAM scanout, Superman 28nm
# DSI PHY/PLL quirk) + Burton panel + drm_hwcomposer with non-blocking flips.
# superman-native-startup.sh selects this stack when the native-boot contract
# phase is "kms"; the boot image only adds the display DT (CC4b cmdline kept).
PRODUCT_PACKAGES += hwcomposer.drm
PRODUCT_COPY_FILES += \
    device/nokia/superman/graphics/native-kms/prebuilt/msm.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/native-kms/msm.ko \
    device/nokia/superman/graphics/native-kms/prebuilt/panel-nokia-burton.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/native-kms/panel-nokia-burton.ko \
    device/nokia/superman/graphics/native-kms/zz-superman-kms.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/zz-superman-kms.rc
