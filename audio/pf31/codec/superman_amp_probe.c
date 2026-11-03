// SPDX-License-Identifier: GPL-2.0-only
/* Stock AUDD role6 pin0x608 decodes to SID0/peripheral0xc1/IRQ0.
 * Read-only electrical-state check; acquiring ASIS does not enable the amp. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>

#define NAME "superman-amp-probe"
static struct platform_device *pdev;
static bool ready;
GPIO_LOOKUP_SINGLE(amp_lookup, NAME,
 "fc4cf000.spmi:pm8226@0:gpio@c000", 1, "amplifier", GPIO_ACTIVE_HIGH);

static int probe(struct platform_device *dev)
{
 struct device *gpio;
 struct regmap *map;
 struct gpio_desc *line;
 static const unsigned int offsets[]={4,5,8,0x40,0x41,0x42,0x43,0x45,0x46};
 unsigned int value, mode=0;
 int i, ret;
 gpio=bus_find_device_by_name(&platform_bus_type,NULL,
       "fc4cf000.spmi:pm8226@0:gpio@c000");
 if(!gpio)return -ENODEV;
 map=dev_get_regmap(gpio->parent,NULL);
 if(!map){put_device(gpio);return -ENODEV;}
 for(i=0;i<ARRAY_SIZE(offsets);i++) {
  ret=regmap_read(map,0xc100+offsets[i],&value);
  if(ret){put_device(gpio);return ret;}
  if(offsets[i]==4 && value!=0x10){put_device(gpio);return -ENODEV;}
  if(offsets[i]==0x40)mode=value;
  dev_info(&dev->dev,"PM8226 GPIO2 reg=%04x value=%02x\n",0xc100+offsets[i],value);
 }
 put_device(gpio);
 line=devm_gpiod_get(&dev->dev,"amplifier",GPIOD_ASIS);
 if(IS_ERR(line))return PTR_ERR(line);
 /* PMIC GPIO has no get_direction callback; use the MODE_CTL read above. */
 dev_info(&dev->dev,"GPIO2 owned ASIS hardware-mode=%u; no writes performed\n",(mode>>4)&7);
 ready=true;
 return 0;
}
static struct platform_driver driver={.probe=probe,.driver={.name=NAME}};
static int __init start(void)
{
 int ret;
 if(!of_machine_is_compatible("nokia,superman") || !of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 gpiod_add_lookup_table(&amp_lookup);
 ret=platform_driver_register(&driver);if(ret)goto lookup;
 pdev=platform_device_register_simple(NAME,-1,NULL,0);
 if(IS_ERR(pdev)){ret=PTR_ERR(pdev);goto driver;}
 if(ready)return 0;
 platform_device_unregister(pdev);ret=-ENODEV;
driver:platform_driver_unregister(&driver);
lookup:gpiod_remove_lookup_table(&amp_lookup);
 return ret;
}
static void __exit stop(void)
{
 platform_device_unregister(pdev);
 platform_driver_unregister(&driver);
 gpiod_remove_lookup_table(&amp_lookup);
}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Lumia 730 stock speaker amplifier pin read-only verification");
