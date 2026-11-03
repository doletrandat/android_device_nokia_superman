// SPDX-License-Identifier: GPL-2.0-only
/* Inactive Nokia 730/MSM8226 laboratory diagnostic. One SPMI read plan,
 * no hardware writes, IRQ registration/ACK, reset or watchdog operation.
 *
 * Register provenance: Qualcomm qpnp-power-on.c, gen1 subtype selector,
 * store_and_clear_warm_reset (clearing is a separate explicit write),
 * and qpnp_pmic_wd_bark_irq's read-only reason dump:
 * https://github.com/commaai/android_kernel_comma_msm8996/blob/master/drivers/platform/msm/qpnp-power-on.c
 * The reference supplies register layout, not this phone's reset cause.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/ktime.h>
#include <linux/mutex.h>

#include "pon-read-plan.h"

static struct superman_pon_snapshot captured;
static unsigned long long begin_ns, end_ns;
static int capture_result;

static int read_pon_register(void *context, unsigned int reg, unsigned int *value)
{
	return regmap_read(context, reg, value);
}

static int snapshot_get(char *buffer, const struct kernel_param *parameter)
{
	unsigned int i;
	int length = 0;

	(void)parameter;
	length += scnprintf(buffer + length, PAGE_SIZE - length,
		"PON_SNAPSHOT base=0x800 reads=%u gen1_verified=%u complete=%u result=%d begin_ns=%llu end_ns=%llu cached_only=1 sequential_not_atomic=1 firmware_may_have_changed_reasons=1\n",
		captured.reads, captured.gen1_verified, captured.complete,
		capture_result, begin_ns, end_ns);
	for (i = 0; i < captured.reads; i++) {
		if (captured.error[i])
			length += scnprintf(buffer + length, PAGE_SIZE - length,
				"%s offset=0x%02x error=%d\n",
				superman_pon_allowlist[i].name,
				superman_pon_allowlist[i].offset, captured.error[i]);
		else
			length += scnprintf(buffer + length, PAGE_SIZE - length,
				"%s offset=0x%02x value=0x%02x raw_not_cause_attribution=1\n",
				superman_pon_allowlist[i].name,
				superman_pon_allowlist[i].offset, captured.value[i]);
	}
	return length;
}

static const struct kernel_param_ops snapshot_ops = { .get = snapshot_get };
module_param_cb(snapshot, &snapshot_ops, NULL, 0400);
MODULE_PARM_DESC(snapshot, "Cached one-shot gen1 PON reasons; never rereads hardware");

static int __init reset_observer_init(void)
{
	struct device_node *node;
	struct platform_device *pon;
	struct regmap *map;
	unsigned int base, i;
	int ret = -ENODEV;

	if (!of_machine_is_compatible("nokia,superman") ||
	    !of_machine_is_compatible("qcom,msm8226"))
		return -ENODEV;
	node = of_find_compatible_node(NULL, NULL, "qcom,pm8916-pon");
	if (!node)
		return -ENODEV;
	if (!of_device_is_available(node) || !node->parent ||
	    !of_device_is_compatible(node->parent, "qcom,pm8226") ||
	    of_property_read_u32(node, "reg", &base) || base != SUPERMAN_PON_BASE) {
		of_node_put(node);
		return -ENODEV;
	}
	pon = of_find_device_by_node(node);
	of_node_put(node);
	if (!pon)
		return -ENODEV;
	if (!pon->dev.parent)
		goto put_pon;

	/* Parent then child prevents driver removal during the one-shot read.
 * No private driver structure or SPMI ownership is assumed.
 */
	device_lock(pon->dev.parent);
	mutex_lock_nested(&pon->dev.mutex, SINGLE_DEPTH_NESTING);
	if (!pon->dev.driver || strcmp(pon->dev.driver->name, "pm8916-pon") ||
	    !pon->dev.parent->driver ||
	    strcmp(pon->dev.parent->driver->name, "pmic-spmi"))
		goto unlock;
	map = dev_get_regmap(pon->dev.parent, NULL);
	if (!map)
		goto unlock;

	begin_ns = ktime_get_boottime_ns();
	capture_result = superman_pon_read_once(read_pon_register, map, &captured);
	end_ns = ktime_get_boottime_ns();
	pr_info("SUPERMAN_RESET_OBSERVER begin_ns=%llu end_ns=%llu reads=%u gen1_verified=%u complete=%u result=%d cached_only=1 no_hardware_writes=1 firmware_may_have_changed_reasons=1\n",
		begin_ns, end_ns, captured.reads, captured.gen1_verified,
		captured.complete, capture_result);
	for (i = 0; i < captured.reads; i++)
		pr_info("SUPERMAN_RESET_OBSERVER %s offset=0x%02x value=0x%02x error=%d raw_not_cause_attribution=1\n",
			superman_pon_allowlist[i].name,
			superman_pon_allowlist[i].offset, captured.value[i], captured.error[i]);
	/* Identity rejection prevents loading. On a verified family, retain
 * partial reason-read errors rather than silently losing that evidence.
 */
	ret = captured.gen1_verified ? 0 : capture_result;
unlock:
	device_unlock(&pon->dev);
	device_unlock(pon->dev.parent);
put_pon:
	put_device(&pon->dev);
	return ret;
}

static void __exit reset_observer_exit(void)
{
	/* No retained device reference, timer, IRQ or hardware state. */
}

module_init(reset_observer_init);
module_exit(reset_observer_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Superman PM8226 cached read-only reset-reason observer");
