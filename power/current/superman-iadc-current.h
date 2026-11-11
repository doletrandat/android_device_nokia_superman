/* SPDX-License-Identifier: GPL-2.0-only */
struct device;
/* Signed calibrated internal-sense microamps; negative on verified discharge. */
int superman_iadc_current_ua(struct device *dev,int *ua);

struct superman_iadc_calibration { unsigned int gain, offset, rsense_uohm; };
int superman_iadc_get_calibration(struct device *dev,struct superman_iadc_calibration *cal);
