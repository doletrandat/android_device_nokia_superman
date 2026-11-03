// SPDX-License-Identifier: GPL-2.0-only
/* PM8226 laboratory observer. No writes, IRQ registration/ACK, regulator,
 * power_supply setter, private charger struct, ADC or PMIC policy changes.
 * Allowlist: qcom_smbb.c controls/status and PMIC-arb QPNPINT_REG_RT_STS.
 * RT bits are defined by pm8226.dtsi, not the charger's cached STATUS_* bits.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/ktime.h>
#include <linux/mutex.h>

struct observed_register { unsigned int offset; const char *name; };
static const struct observed_register allowlist[] = {
    {0x005,"CHG_SUBTYPE"}, {0x105,"BUCK_SUBTYPE"},
    {0x305,"USB_SUBTYPE"}, {0x605,"MISC_SUBTYPE"},
    {0x010,"CHG_IRQ_RT"}, {0x210,"BAT_IRQ_RT"},
    {0x310,"USB_IRQ_RT"}, {0x410,"DC_IRQ_RT"},
    {0x040,"CHG_VMAX"}, {0x041,"CHG_VSAFE"}, {0x043,"CHG_CFG"},
    {0x044,"CHG_IMAX"}, {0x045,"CHG_ISAFE"}, {0x047,"CHG_VIN_MIN"},
    {0x049,"CHG_CTRL"}, {0x052,"VBAT_WEAK"}, {0x05b,"IBAT_TERM"},
    {0x05d,"VBAT_DET"}, {0x060,"TCHG_MAX_EN"}, {0x062,"WDOG_TIME"},
    {0x065,"WDOG_EN"}, {0x174,"BUCK_REG_MODE"},
    {0x208,"BAT_PRES_STATUS"}, {0x209,"BAT_TEMP_STATUS"},
    {0x249,"BAT_BTC_CTRL"}, {0x344,"USB_IMAX"}, {0x348,"USB_OTG_CTL"},
    {0x34e,"USB_ENUM_TIMER_STOP"}, {0x444,"DC_IMAX"},
    {0x601,"MISC_REV2"}, {0x642,"MISC_BOOT_DONE"},
};
/* Primary SMBB-family qpnp-charger.c reference; enabled only after observed
 * PM8226 SMBBP charger/buck/USB/misc subtypes 31/32/34/37 all agree.
 * Excludes PET, security, latch/pending/ACK and all write-only registers. */
static const struct observed_register smbbp_status[] = {
    {0x009,"CHG_STATUS_RAW"}, {0x00b,"CHG_VBAT_STATUS_RAW"},
    {0x063,"WDOG_DELAY_RAW"}, {0x110,"BUCK_IRQ_RT_RAW"},
    {0x347,"USB_SUSPEND_RAW"},
};
static struct platform_device *charger;
static DEFINE_MUTEX(snapshot_lock);
static unsigned int sequence;
static int snapshot_get(char *buf, const struct kernel_param *param)
{
    struct regmap *map;
    unsigned int i, value, chg_type=0, buck_type=0, usb_type=0, misc_type=0;
    int len=0, ret;
    (void)param;
    mutex_lock(&snapshot_lock);
    if (!charger) { mutex_unlock(&snapshot_lock); return -ENODEV; }
    device_lock(&charger->dev);
    if (!charger->dev.driver || strcmp(charger->dev.driver->name,"qcom-smbb")) {
        ret=-ENODEV; goto out;
    }
    map=dev_get_regmap(charger->dev.parent,NULL);
    if (!map) { ret=-ENODEV; goto out; }
    len+=scnprintf(buf+len,PAGE_SIZE-len,
        "SMBB_SNAPSHOT sequence=%u begin_ns=%llu base=0x1000 read_only=true sequential_not_atomic=true\n",
        ++sequence,ktime_get_boottime_ns());
    for (i=0;i<ARRAY_SIZE(allowlist);++i) {
        ret=regmap_read(map,0x1000+allowlist[i].offset,&value);
        if (ret) len+=scnprintf(buf+len,PAGE_SIZE-len,"%s offset=0x%03x error=%d\n",allowlist[i].name,allowlist[i].offset,ret);
        else {
            len+=scnprintf(buf+len,PAGE_SIZE-len,"%s offset=0x%03x value=0x%02x\n",allowlist[i].name,allowlist[i].offset,value);
            switch(allowlist[i].offset) {
            case 0x005: chg_type=value;break;
            case 0x105: buck_type=value;break;
            case 0x305: usb_type=value;break;
            case 0x605: misc_type=value;break;
            }
        }
    }
    if(chg_type==0x31 && buck_type==0x32 && usb_type==0x34 && misc_type==0x37) {
        len+=scnprintf(buf+len,PAGE_SIZE-len,"SMBBP_FAMILY verified=true extended_status_reads=5 raw_not_numeric_measurements=true\n");
        for(i=0;i<ARRAY_SIZE(smbbp_status);++i) {
            ret=regmap_read(map,0x1000+smbbp_status[i].offset,&value);
            if(ret) len+=scnprintf(buf+len,PAGE_SIZE-len,"%s offset=0x%03x error=%d\n",smbbp_status[i].name,smbbp_status[i].offset,ret);
            else len+=scnprintf(buf+len,PAGE_SIZE-len,"%s offset=0x%03x value=0x%02x\n",smbbp_status[i].name,smbbp_status[i].offset,value);
        }
    } else len+=scnprintf(buf+len,PAGE_SIZE-len,"SMBBP_FAMILY verified=false extended_status_reads=0 reason=subtype_mismatch_or_read_error\n");
    len+=scnprintf(buf+len,PAGE_SIZE-len,"SMBB_SNAPSHOT end_ns=%llu measurements_unavailable=true\n",ktime_get_boottime_ns());
    ret=len;
out:
    device_unlock(&charger->dev);mutex_unlock(&snapshot_lock);return ret;
}
static const struct kernel_param_ops snapshot_ops = { .get=snapshot_get };
module_param_cb(snapshot,&snapshot_ops,NULL,0400);
MODULE_PARM_DESC(snapshot,"Read-only allowlisted PM8226 SMBB state; no voltage/current/capacity measurement");
static int __init charge_observer_init(void)
{
    struct device_node *node;
    unsigned int base;
    if (!of_machine_is_compatible("nokia,superman") ||
        !of_machine_is_compatible("qcom,msm8226")) return -ENODEV;
    node=of_find_compatible_node(NULL,NULL,"qcom,pm8226-charger");
    if (!node) return -ENODEV;
    if (!of_device_is_available(node) || !node->parent ||
        !of_device_is_compatible(node->parent,"qcom,pm8226") ||
        of_property_read_u32(node,"reg",&base) || base!=0x1000) {
        of_node_put(node);return -ENODEV;
    }
    charger=of_find_device_by_node(node);of_node_put(node);
    if (!charger) return -ENODEV;
    device_lock(&charger->dev);
    if (!charger->dev.driver || strcmp(charger->dev.driver->name,"qcom-smbb") ||
        !dev_get_regmap(charger->dev.parent,NULL)) {
        device_unlock(&charger->dev);put_device(&charger->dev);charger=NULL;return -ENODEV;
    }
    device_unlock(&charger->dev);
    pr_info("SUPERMAN_CHARGE_OBSERVER ready read_only=1 allowlist=%zu no_charge_policy=1\n",ARRAY_SIZE(allowlist));
    return 0;
}
static void __exit charge_observer_exit(void)
{
    mutex_lock(&snapshot_lock);if(charger)put_device(&charger->dev);charger=NULL;mutex_unlock(&snapshot_lock);
}
module_init(charge_observer_init);
module_exit(charge_observer_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Superman PM8226 read-only charging-state observer");
