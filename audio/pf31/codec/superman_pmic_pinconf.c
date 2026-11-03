// SPDX-License-Identifier: GPL-2.0-only
/* One-off recovery of the first amp test through native GROUP pinconf. */
#include <linux/module.h>
#include <linux/gpio/driver.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinconf-generic.h>
#define NAME "superman-amp-pinconf-recovery"
static struct gpio_chip *chip;
static struct gpio_device *gdev;
static struct platform_device *pdev;
static struct pinctrl *pinctrl;
static struct pinctrl_state *off;
static unsigned long configs[3];
static const struct pinctrl_map maps[]={
 PIN_MAP_CONFIGS_GROUP(NAME,"amp-off","fc4cf000.spmi:pm8226@0:gpio@c000","gpio2",configs),
};
static int set_config(struct gpio_chip *gc,unsigned int offset,unsigned long config)
{
 if(gc!=chip || offset!=1 || pinconf_to_config_param(config)!=PIN_CONFIG_OUTPUT_ENABLE || pinconf_to_config_argument(config))return -EOPNOTSUPP;
 return pinctrl_select_state(pinctrl,off);
}
static int __init start(void)
{
 int ret;
 if(!of_machine_is_compatible("nokia,superman") || !of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 gdev=gpio_device_find_by_label("fc4cf000.spmi:pm8226@0:gpio@c000");if(!gdev)return -ENODEV;
 chip=gpio_device_get_chip(gdev);
 if(!chip || chip->ngpio!=8 || chip->can_sleep || chip->set_config){ret=-EBUSY;goto gpio;}
 configs[0]=pinconf_to_config_packed(PIN_CONFIG_OUTPUT,0);
 configs[1]=pinconf_to_config_packed(PIN_CONFIG_OUTPUT_ENABLE,0);
 configs[2]=pinconf_to_config_packed(PIN_CONFIG_INPUT_ENABLE,1);
 ret=pinctrl_register_mappings(maps,ARRAY_SIZE(maps));if(ret)goto gpio;
 pdev=platform_device_register_simple(NAME,-1,NULL,0);if(IS_ERR(pdev)){ret=PTR_ERR(pdev);goto maps;}
 pinctrl=pinctrl_get(&pdev->dev);if(IS_ERR(pinctrl)){ret=PTR_ERR(pinctrl);goto device;}
 off=pinctrl_lookup_state(pinctrl,"amp-off");if(IS_ERR(off)){ret=PTR_ERR(off);goto pinctrl;}
 WRITE_ONCE(chip->set_config,set_config);
 pr_info("SUPERMAN_PMIC_PINCONF group recovery available for GPIO2 output-disable only\n");return 0;
pinctrl:pinctrl_put(pinctrl);
device:platform_device_unregister(pdev);
maps:pinctrl_unregister_mappings(maps);
gpio:gpio_device_put(gdev);return ret;
}
static void __exit stop(void)
{
 if(READ_ONCE(chip->set_config)==set_config)WRITE_ONCE(chip->set_config,NULL);
 pinctrl_put(pinctrl);platform_device_unregister(pdev);pinctrl_unregister_mappings(maps);gpio_device_put(gdev);
 pr_info("SUPERMAN_PMIC_PINCONF original callback restored\n");
}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Superman GPIO2 native group pinconf recovery for retained first test");
