# Calibrated internal IADC current; no charger or SOC policy.
PRODUCT_COPY_FILES += \
    device/nokia/superman/power/current/prebuilt/qcom-spmi-iadc.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/battery-current/qcom-spmi-iadc.ko \
    device/nokia/superman/power/current/prebuilt/superman-iadc-current.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/battery-current/superman-iadc-current.ko \
    device/nokia/superman/power/current/superman-battery-current.sh:$(TARGET_COPY_OUT_VENDOR)/bin/superman-battery-current.sh \
    device/nokia/superman/power/current/superman-battery-current.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/superman-battery-current.rc \
    device/nokia/superman/power/current/battery-current.sha256:$(TARGET_COPY_OUT_VENDOR)/etc/superman/battery-current.sha256
