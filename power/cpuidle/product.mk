# MSM8226 SPC power collapse, enabled after boot; requires the CC4 kernel/DT.
PRODUCT_COPY_FILES += \
    device/nokia/superman/power/cpuidle/superman-cpuidle.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/superman-cpuidle.rc
