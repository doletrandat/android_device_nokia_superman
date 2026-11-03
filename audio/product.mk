# SPDX-License-Identifier: Apache-2.0
# Addon to the selected PF24 transport product; retain its firmware/base providers.
# Replace prior copies of these destinations rather than producing duplicates.
_superman_audio_destinations := \
    $(TARGET_COPY_OUT_VENDOR)/bin/superman-audio-core.sh \
    $(TARGET_COPY_OUT_VENDOR)/bin/superman-audio-playback.sh \
    $(TARGET_COPY_OUT_VENDOR)/etc/init/superman-audio-core.rc \
    $(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml \
    $(TARGET_COPY_OUT_VENDOR)/etc/superman/audio-core.sha256
PRODUCT_COPY_FILES := $(foreach f,$(PRODUCT_COPY_FILES),$(if $(filter $(_superman_audio_destinations),$(word 2,$(subst :, ,$(f)))),,$(f)))
PRODUCT_PACKAGES += audio.primary.superman
PRODUCT_COPY_FILES += \
    device/nokia/superman/audio/android.hardware.superman.audio.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.superman.audio.xml \
    device/nokia/superman/audio/superman-audio-hal.sh:$(TARGET_COPY_OUT_VENDOR)/bin/superman-audio-hal.sh \
    device/nokia/superman/audio/superman-audio-core.sh:$(TARGET_COPY_OUT_VENDOR)/bin/superman-audio-core.sh \
    device/nokia/superman/audio/superman-audio-playback.sh:$(TARGET_COPY_OUT_VENDOR)/bin/superman-audio-playback.sh \
    device/nokia/superman/audio/superman-audio-core.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/superman-audio-core.rc \
    device/nokia/superman/audio/zz-superman-audio-service.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/zz-superman-audio-service.rc \
    device/nokia/superman/audio/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml \
    device/nokia/superman/audio/audio-core.sha256:$(TARGET_COPY_OUT_VENDOR)/etc/superman/audio-core.sha256 \
    device/nokia/superman/audio/audio-playback.sha256:$(TARGET_COPY_OUT_VENDOR)/etc/superman/audio-playback.sha256 \
    device/nokia/superman/audio/audio-boot.contract:$(TARGET_COPY_OUT_VENDOR)/etc/superman/audio-boot.contract
PRODUCT_COPY_FILES += $(foreach f,$(wildcard device/nokia/superman/audio/pf31/prebuilt/*.ko),$(f):$(TARGET_COPY_OUT_VENDOR)/lib/modules/audio-pf31/$(notdir $(f)))
