# AOSP software Bluetooth audio (A2DP, hearing aid) through the stock audio HAL service.
PRODUCT_PACKAGES += \
    android.hardware.bluetooth.audio@2.0-impl \
    audio.bluetooth.default

PRODUCT_COPY_FILES += \
    frameworks/av/services/audiopolicy/config/bluetooth_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/bluetooth_audio_policy_configuration.xml

# The IBluetoothAudioProvidersFactory VINTF entry is merged through DEVICE_MANIFEST_FILE
# (BoardConfig.mk); the build rejects VINTF files in PRODUCT_COPY_FILES.
