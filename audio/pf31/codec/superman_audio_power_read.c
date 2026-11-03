// SPDX-License-Identifier: GPL-2.0-only
/* Cached, fixed read-only acquisition of Nokia AUDD clock/pin prerequisites. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/regmap.h>
static char snapshot[1024];
module_param_string(snapshot,snapshot,sizeof(snapshot),0400);
static int __init start(void)
{
 static const unsigned regs[]={0xc004,0xc005,0xc040,0xc041,0xc042,0xc043,0xc045,0xc046,
  0x5b04,0x5b05,0x5b43,0x5b46};
 struct device *gpio;struct regmap *map;unsigned i,v;int ret,pos=0;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 gpio=bus_find_device_by_name(&platform_bus_type,NULL,"fc4cf000.spmi:pm8226@0:gpio@c000");
 if(!gpio)return -ENODEV;
 map=dev_get_regmap(gpio->parent,NULL);if(!map){put_device(gpio);return -ENODEV;}
 ret=regmap_read(map,0xc004,&v);if(ret||v!=0x10){put_device(gpio);return ret?:-ENODEV;}
 for(i=0;i<ARRAY_SIZE(regs);i++){
  ret=regmap_read(map,regs[i],&v);
  if(ret){put_device(gpio);return ret;}
  pos+=scnprintf(snapshot+pos,sizeof(snapshot)-pos,"%04x=%02x ",regs[i],v);
 }
 put_device(gpio);pr_info("SUPERMAN_AUDIO_POWER_READ no hardware writes: %s\n",snapshot);return 0;
}
static void __exit stop(void){}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only cached Nokia AUDD PMIC GPIO1 and DIVCLK1 baseline");
