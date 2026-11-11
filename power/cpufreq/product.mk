# MSM8226 turbo CPU OPPs; S2 raised to 1.23 V above 787.2 MHz, 1.15 V otherwise.
PRODUCT_COPY_FILES += \
    device/nokia/superman/power/cpufreq/prebuilt/superman-apc-dvfs.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/cpufreq/superman-apc-dvfs.ko \
    device/nokia/superman/power/cpufreq/superman-apc-dvfs.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/superman-apc-dvfs.rc
