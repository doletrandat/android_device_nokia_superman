# PM8226 charger watchdog service; qcom_smbb limits and thermal gate unchanged.
PRODUCT_COPY_FILES += \
    device/nokia/superman/power/charger/prebuilt/superman-charge-wdog.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/charger/superman-charge-wdog.ko \
    device/nokia/superman/power/charger/superman-charge-wdog.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/superman-charge-wdog.rc
