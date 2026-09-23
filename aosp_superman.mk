# SPDX-License-Identifier: Apache-2.0
# Nokia Lumia 730 (Superman) Android 11 bring-up target.

$(call inherit-product, $(SRC_TARGET_DIR)/product/core_minimal.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/handheld_system_ext.mk)
$(call inherit-product, frameworks/base/data/keyboards/keyboards.mk)
$(call inherit-product-if-exists, frameworks/base/data/fonts/fonts.mk)
$(call inherit-product-if-exists, external/google-fonts/dancing-script/fonts.mk)
$(call inherit-product-if-exists, external/google-fonts/carrois-gothic-sc/fonts.mk)
$(call inherit-product-if-exists, external/google-fonts/coming-soon/fonts.mk)
$(call inherit-product-if-exists, external/google-fonts/cutive-mono/fonts.mk)
$(call inherit-product-if-exists, external/google-fonts/source-sans-pro/fonts.mk)
$(call inherit-product-if-exists, external/noto-fonts/fonts.mk)
$(call inherit-product-if-exists, external/roboto-fonts/fonts.mk)
$(call inherit-product, device/nokia/superman/device.mk)

PRODUCT_NAME := aosp_superman
PRODUCT_DEVICE := superman
PRODUCT_BRAND := Nokia
PRODUCT_MODEL := Lumia 730
PRODUCT_MANUFACTURER := Nokia
PRODUCT_CHARACTERISTICS := nosdcard
