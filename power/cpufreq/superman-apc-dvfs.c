// SPDX-License-Identifier: GPL-2.0-only
/*
 * Turbo CPU frequencies for Lumia 730 (MSM8226 speed bin 0, fuse version 2).
 *
 * Mainline stops at 787.2 MHz because frequencies above it need the turbo
 * voltage corner on the APC rail (PM8226 S2). Downstream maps 998.4, 1094.4
 * and 1190.4 MHz to turbo (ceiling 1.28 V) and everything up to 787.2 MHz to
 * nominal (1.15 V), which is what the bootloader leaves on S2. This part's
 * PVS fuse is bin 23 (open-loop turbo 1.14 V); without CPR we run turbo at a
 * fixed 1.23 V, between the fused value and the ceiling.
 *
 * S2 is an FTSMPS that firmware leaves in range 1 (10 mV steps). Like the
 * downstream spm-regulator, only VSET is written and the range never changes.
 * Neither the L2 nor the CPU SAW drives this rail. The voltage is raised
 * before any change above 787.2 MHz and lowered after any change back.
 * The module cannot be unloaded; a reboot restores the stock behaviour.
 */
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regmap.h>

#define S2_TYPE			0x1704
#define S2_RANGE		0x1740
#define S2_VSET			0x1741
#define FTSMPS_TYPE		0x1c08
#define S2_RANGE_10MV		1

#define VSET_NOMINAL		0x73	/* 1.15 V */
#define VSET_CEILING		0x80	/* 1.28 V */
#define NOMINAL_MAX_KHZ		787200

/* 80 mV at the measured ~9.6 mV/us slew is under 10 us; leave margin. */
#define SETTLE_US		60

static const unsigned long turbo_hz[] = { 998400000, 1094400000, 1190400000 };

static unsigned int turbo_vset = 0x7b;	/* 1.23 V */
module_param(turbo_vset, uint, 0444);
static unsigned int raises, lowers, errors, vset;
module_param(raises, uint, 0444);
module_param(lowers, uint, 0444);
module_param(errors, uint, 0444);
module_param(vset, uint, 0444);

static struct regmap *map;
static DEFINE_MUTEX(lock);

static int set_vset(unsigned int v)
{
	unsigned int now;
	int rc;

	if (vset == v)
		return 0;
	rc = regmap_write(map, S2_VSET, v);
	if (!rc)
		rc = regmap_read(map, S2_VSET, &now);
	if (!rc && now != v)
		rc = -EIO;
	if (rc) {
		errors++;
		pr_err("SUPERMAN_APC_DVFS vset %02x failed %d\n", v, rc);
		return rc;
	}
	if (v > vset) {
		usleep_range(SETTLE_US, 2 * SETTLE_US);
		raises++;
	} else {
		lowers++;
	}
	vset = v;
	return 0;
}

static int transition(struct notifier_block *nb, unsigned long event, void *data)
{
	struct cpufreq_freqs *f = data;

	mutex_lock(&lock);
	if (event == CPUFREQ_PRECHANGE && f->new > NOMINAL_MAX_KHZ)
		set_vset(turbo_vset);
	else if (event == CPUFREQ_POSTCHANGE && f->new <= NOMINAL_MAX_KHZ)
		set_vset(VSET_NOMINAL);
	/* A failed raise is retried once the change lands. */
	else if (event == CPUFREQ_POSTCHANGE && vset != turbo_vset)
		set_vset(turbo_vset);
	mutex_unlock(&lock);
	return NOTIFY_OK;
}

static struct notifier_block nb = { .notifier_call = transition };

static int __init dvfs_init(void)
{
	struct device_node *node;
	struct platform_device *pdev;
	struct device *cpu;
	unsigned int type_hi, type_lo, range;
	int i, rc;

	if (!of_machine_is_compatible("nokia,superman") ||
	    !of_machine_is_compatible("qcom,msm8226"))
		return -ENODEV;
	if (turbo_vset <= VSET_NOMINAL || turbo_vset > VSET_CEILING)
		return -EINVAL;
	node = of_find_compatible_node(NULL, NULL, "qcom,pm8226-regulators");
	if (!node)
		return -ENODEV;
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return -ENODEV;
	map = dev_get_regmap(pdev->dev.parent, NULL);
	put_device(&pdev->dev);
	if (!map)
		return -ENODEV;

	rc = regmap_read(map, S2_TYPE, &type_hi);
	if (!rc)
		rc = regmap_read(map, S2_TYPE + 1, &type_lo);
	if (!rc)
		rc = regmap_read(map, S2_RANGE, &range);
	if (!rc)
		rc = regmap_read(map, S2_VSET, &vset);
	if (rc)
		return rc;
	if ((type_hi << 8 | type_lo) != FTSMPS_TYPE || range != S2_RANGE_10MV ||
	    vset != VSET_NOMINAL) {
		pr_err("SUPERMAN_APC_DVFS unexpected S2 type=%02x%02x range=%u vset=%02x\n",
		       type_hi, type_lo, range, vset);
		return -ENODEV;
	}

	cpu = get_cpu_device(0);
	if (!cpu)
		return -ENODEV;
	rc = cpufreq_register_notifier(&nb, CPUFREQ_TRANSITION_NOTIFIER);
	if (rc)
		return rc;
	for (i = 0; i < ARRAY_SIZE(turbo_hz); i++) {
		rc = dev_pm_opp_add(cpu, turbo_hz[i], 0);
		if (rc && rc != -EEXIST)
			pr_err("SUPERMAN_APC_DVFS opp %lu failed %d\n", turbo_hz[i], rc);
	}
	pr_info("SUPERMAN_APC_DVFS ready nominal=%02x turbo=%02x; rebind cpufreq-dt to use\n",
		VSET_NOMINAL, turbo_vset);
	return 0;
}
module_init(dvfs_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Lumia730 MSM8226 turbo CPU OPPs with PM8226 S2 voltage switching");
