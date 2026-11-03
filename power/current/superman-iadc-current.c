// SPDX-License-Identifier: GPL-2.0-only
/* Publish measured current on the existing battery; no charger or SOC policy. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/power_supply.h>
#include "superman-iadc-current.h"
static struct platform_device *iadc_device;
static struct power_supply *battery;
static bool attribute_created;
static ssize_t current_now_show(struct device *dev,struct device_attribute *attr,char *buf)
{
 int ua,ret;(void)attr;
 if(!battery||dev!=&battery->dev||!iadc_device)return -ENODEV;
 ret=superman_iadc_current_ua(&iadc_device->dev,&ua);
 if(ret)return ret;
 return sysfs_emit(buf,"%d\n",ua);
}
static DEVICE_ATTR_RO(current_now);
static void current_cleanup(void)
{
 /* kernfs removal drains active readers before either reference is released. */
 if(attribute_created){device_remove_file(&battery->dev,&dev_attr_current_now);attribute_created=false;}
 if(iadc_device){put_device(&iadc_device->dev);iadc_device=NULL;}
 if(battery){power_supply_put(battery);battery=NULL;}
}
static int __init current_init(void)
{
 struct device_node *node;unsigned base;int ret,ua;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 node=of_find_compatible_node(NULL,NULL,"qcom,pm8226-iadc");
 if(!node)return -ENODEV;
 if(!of_device_is_available(node)||!node->parent||!of_device_is_compatible(node->parent,"qcom,pm8226")||of_property_read_u32(node,"reg",&base)||base!=0x3600){of_node_put(node);return -ENODEV;}
 iadc_device=of_find_device_by_node(node);of_node_put(node);
 if(!iadc_device)return -ENODEV;
 battery=power_supply_get_by_name("smbb-bif");
 if(!battery){ret=-ENODEV;goto fail;}
 if(!battery->desc||battery->desc->type!=POWER_SUPPLY_TYPE_BATTERY||!battery->dev.parent||!battery->dev.parent->of_node||!of_device_is_compatible(battery->dev.parent->of_node,"qcom,pm8226-charger")){ret=-ENODEV;goto fail;}
 ret=superman_iadc_current_ua(&iadc_device->dev,&ua);if(ret)goto fail;
 ret=device_create_file(&battery->dev,&dev_attr_current_now);if(ret)goto fail;
 attribute_created=true;
 pr_info("SUPERMAN_BATTERY_CURRENT ready signed_microamps=1 no_charge_policy=1 no_SOC=1\n");
 return 0;
 fail:current_cleanup();return ret;
}
module_init(current_init);
module_exit(current_cleanup);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Lumia730 calibrated internal IADC current sysfs bridge; no charger policy or SOC");
