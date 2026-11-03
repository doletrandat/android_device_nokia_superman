// SPDX-License-Identifier: GPL-2.0-only
/* Bounded amp-enable test, allowed only with both codec line DACs off. */
#include <linux/module.h>
#include <linux/slimbus.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include "superman-codec.h"
extern const struct bus_type slimbus_bus;
static bool passed,retained;
module_param(passed,bool,0444);module_param(retained,bool,0444);
static int release(const char *value,const struct kernel_param *kp)
{
 bool yes;int ret=kstrtobool(value,&yes);
 if(ret || !yes)return -EINVAL;
 ret=superman_speaker_amp_enable(false);
 if(!ret && retained){retained=false;module_put(THIS_MODULE);}
 return ret;
}
static const struct kernel_param_ops release_ops={.set=release};
module_param_cb(release,&release_ops,NULL,0200);
static int __init start(void)
{
 struct device *dev;struct regmap *map;unsigned int value;int i,cycle,ret,off;
 static const unsigned int regs[]={0x1ca,0x1cf,0x1d3};
 static const unsigned int masks[]={3,0xc0,0xc0};
 dev=bus_find_device_by_name(&slimbus_bus,NULL,"217:e0:1:0");if(!dev)return -ENODEV;
 if(!dev->driver || strcmp(dev->driver->name,"superman-wcd9302")){ret=-ENODEV;goto put;}
 map=dev_get_regmap(dev,NULL);if(!map){ret=-ENODEV;goto put;}
 for(i=0;i<ARRAY_SIZE(regs);i++) {
  ret=regmap_read(map,regs[i],&value);if(ret)goto put;
  pr_info("SUPERMAN_AMP_SMOKE codec reg=%03x value=%02x\n",regs[i],value);
  if(value&masks[i]){ret=-EBUSY;goto put;}
 }
 for(cycle=0;cycle<3;cycle++) {
  ret=superman_speaker_amp_enable(true);
  if(!ret && superman_speaker_amp_enable(true)!=-EBUSY)ret=-EIO;
  if(!ret)msleep(20);
  off=superman_speaker_amp_enable(false);
  if(off){retained=true;__module_get(THIS_MODULE);ret=0;pr_err("SUPERMAN_AMP_SMOKE disable=%d; retained for retry\n",off);goto put;}
  if(ret)goto put;
 }
 passed=true;pr_info("SUPERMAN_AMP_SMOKE PASS three 20ms enable/readback/exact restore cycles, DACs remained off\n");
put:put_device(dev);return ret;
}
static void __exit stop(void) {}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Lumia 730 muted speaker amplifier reversible control check");
