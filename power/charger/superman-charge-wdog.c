// SPDX-License-Identifier: GPL-2.0-only
/*
 * Service the PM8226 SMBB charger watchdog on Lumia 730.
 *
 * Firmware leaves CHG_WDOG_EN (0x1065) set and the OS cannot clear it, even
 * after SEC_ACCESS. Without a pet the charger state machine never starts a
 * charge cycle, and it stops an active one ~32 s later (WDOG_TIME 0x20).
 * Hardware test: one pet started fast charge at 3.95 V and periodic pets kept
 * it running. This module only pets while USB or DC input is valid. Charge
 * enable, limits and thermal policy stay with qcom_smbb and the hardware
 * comparator; if this worker stops, the watchdog stops charging again.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define CHGR_SUBTYPE		0x1005
#define CHGR_WDOG_PET		0x1064
#define CHGR_WDOG_PET_BIT	BIT(7)
#define CHGR_WDOG_EN		0x1065
#define USB_RT_STS		0x1310
#define DC_RT_STS		0x1410
#define CHG_IN_VALID		BIT(1)

/* Well inside the 32 s hardware timeout. */
#define PERIOD_MS		5000

static struct platform_device *charger;
static struct regmap *map;
static struct delayed_work work;

static unsigned int pets, last_error;
module_param(pets, uint, 0444);
module_param(last_error, uint, 0444);

static int service(void)
{
	unsigned int usb, dc;
	int rc;

	rc = regmap_read(map, USB_RT_STS, &usb);
	if (!rc)
		rc = regmap_read(map, DC_RT_STS, &dc);
	if (rc || !((usb | dc) & CHG_IN_VALID))
		return rc;
	rc = regmap_write(map, CHGR_WDOG_PET, CHGR_WDOG_PET_BIT);
	if (!rc)
		pets++;
	return rc;
}

static void service_work(struct work_struct *w)
{
	int rc = service();

	if (rc) {
		last_error = -rc;
		pr_err_ratelimited("superman-charge-wdog: service error %d\n", rc);
	}
	queue_delayed_work(system_freezable_power_efficient_wq, &work,
			   msecs_to_jiffies(PERIOD_MS));
}

static int __init wdog_init(void)
{
	struct device_node *node;
	unsigned int type, wdog;
	int rc;

	if (!of_machine_is_compatible("nokia,superman") ||
	    !of_machine_is_compatible("qcom,msm8226"))
		return -ENODEV;
	node = of_find_compatible_node(NULL, NULL, "qcom,pm8226-charger");
	if (!node)
		return -ENODEV;
	charger = of_find_device_by_node(node);
	of_node_put(node);
	if (!charger)
		return -ENODEV;
	if (!charger->dev.driver || strcmp(charger->dev.driver->name, "qcom-smbb")) {
		rc = -ENODEV;
		goto fail;
	}
	map = dev_get_regmap(charger->dev.parent, NULL);
	rc = map ? regmap_read(map, CHGR_SUBTYPE, &type) : -ENODEV;
	if (!rc && type != 0x31)
		rc = -ENODEV;
	if (!rc)
		rc = regmap_read(map, CHGR_WDOG_EN, &wdog);
	if (rc)
		goto fail;
	INIT_DELAYED_WORK(&work, service_work);
	queue_delayed_work(system_freezable_power_efficient_wq, &work, 0);
	pr_info("SUPERMAN_CHARGE_WDOG ready wdog_en=%02x period_ms=%d\n",
		wdog, PERIOD_MS);
	return 0;
fail:
	put_device(&charger->dev);
	return rc;
}

static void __exit wdog_exit(void)
{
	cancel_delayed_work_sync(&work);
	put_device(&charger->dev);
}
module_init(wdog_init);
module_exit(wdog_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Lumia730 PM8226 charger watchdog service; limits unchanged");
