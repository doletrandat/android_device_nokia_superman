# SPDX-License-Identifier: Apache-2.0
# Selected only with the matching qualified firmware/modem/SD profile.
PRODUCT_PACKAGES += libsuperman-ril rild.superman superman-modem-start superman-rmtfs superman-qmux-probe superman-qrtr-probe
PRODUCT_COPY_FILES += frameworks/native/data/etc/android.hardware.telephony.gsm.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.gsm.xml

PRODUCT_PROPERTY_OVERRIDES += ro.telephony.default_network=3

PRODUCT_COPY_FILES += device/nokia/superman/prebuilt/modem-pf30/qcom_bam_dmux.ko:$(TARGET_COPY_OUT_VENDOR)/lib/modules/modem/qcom_bam_dmux.ko

include device/nokia/superman/wifi/superman-wifi.mk
