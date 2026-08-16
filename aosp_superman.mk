# SPDX-License-Identifier: Apache-2.0
# Nokia Lumia 730 (Superman) Android 11 bring-up target.

$(call inherit-product, $(SRC_TARGET_DIR)/product/core_minimal.mk)
$(call inherit-product, device/nokia/superman/device.mk)

PRODUCT_NAME := aosp_superman
PRODUCT_DEVICE := superman
PRODUCT_BRAND := Nokia
PRODUCT_MODEL := Lumia 730
PRODUCT_MANUFACTURER := Nokia
PRODUCT_CHARACTERISTICS := nosdcard
