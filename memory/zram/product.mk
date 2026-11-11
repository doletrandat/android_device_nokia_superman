# zram swap from out-of-tree zsmalloc/zram modules (lzo, lzo-rle, zstd backends).
PRODUCT_COPY_FILES += \
    device/nokia/superman/memory/zram/prebuilt/zsmalloc.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/zram/zsmalloc.ko \
    device/nokia/superman/memory/zram/prebuilt/zram.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/zram/zram.ko \
    device/nokia/superman/memory/zram/superman-zram.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/superman-zram.rc
