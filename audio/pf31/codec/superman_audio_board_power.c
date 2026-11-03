// SPDX-License-Identifier: GPL-2.0-only
/* Nokia AUDD PEP: GPIO1 source4/VIN3, existing DIVCLK1 /2, RPM L25 1.775V. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/delay.h>
#include "superman-codec.h"
#define NAME "superman-audio-board-power"
#define CLOCK_NAME NAME "-clock"
#define CTRL "fc4cf000.spmi:pm8226@0:gpio@c000"
struct audio_power {
 struct regmap *gpio_map,*ldo_map;
 struct regulator *l25;
 struct platform_device *clock_dev;
 struct pinctrl *pins;
 struct pinctrl_state *on,*off;
 bool owned,pin_attempt,enabled,voltage_attempt,uncertain,ready;
 int old_uv;
};
static struct audio_power *power;
static DEFINE_MUTEX(power_lock);
static unsigned long on_cfg[4],off_cfg[5];
static const struct pinctrl_map maps[]={
 PIN_MAP_MUX_GROUP(CLOCK_NAME,"clock-on",CTRL,"gpio1","func1"),
 PIN_MAP_CONFIGS_GROUP(CLOCK_NAME,"clock-on",CTRL,"gpio1",on_cfg),
 PIN_MAP_MUX_GROUP(CLOCK_NAME,"clock-off",CTRL,"gpio1","normal"),
 PIN_MAP_CONFIGS_GROUP(CLOCK_NAME,"clock-off",CTRL,"gpio1",off_cfg),
};
static int check_pin(struct audio_power *p,bool on)
{
 static const unsigned regs[]={0x40,0x41,0x42,0x43,0x45,0x46};
 static const u8 off[]={0,0,4,0,1,0x80},active[]={0x14,3,5,0,1,0x80};
 unsigned i,v;int ret;
 for(i=0;i<ARRAY_SIZE(regs);i++){
  ret=regmap_read(p->gpio_map,0xc000+regs[i],&v);if(ret)return ret;
  if(v!=(on?active[i]:off[i]))return -EIO;
 }
 ret=regmap_read(p->gpio_map,0x5b43,&v);if(ret)return ret;if(v!=2)return -EBUSY;
 ret=regmap_read(p->gpio_map,0x5b46,&v);if(ret)return ret;
 return v==0x80?0:-EBUSY;
}
static int check_ldo(struct audio_power *p,bool enabled)
{
 unsigned value;int ret=regmap_read(p->ldo_map,0x5846,&value);
 return ret?ret:!!(value&0x80)==enabled?0:-EBUSY;
}
static int restore(struct audio_power *p)
{
 int ret;
 if(!p->owned)return 0;
 ret=superman_speaker_amp_is_off();if(ret)return ret;
 p->ready=false;
 if(p->pin_attempt){ret=pinctrl_select_state(p->pins,p->off);if(ret)return ret;ret=check_pin(p,false);if(ret)return ret;p->pin_attempt=false;}
 if(p->uncertain)return -EAGAIN;
 if(p->enabled){ret=regulator_disable(p->l25);if(ret)return ret;p->enabled=false;}
 ret=regulator_is_enabled(p->l25);if(ret)return ret<0?ret:-EBUSY;
 ret=check_ldo(p,false);if(ret)return ret;
 if(p->voltage_attempt){
  ret=regulator_get_voltage(p->l25);if(ret<0)return ret;
  if(ret!=p->old_uv){ret=regulator_set_voltage(p->l25,p->old_uv,p->old_uv);if(ret)return ret;}
  if(regulator_get_voltage(p->l25)!=p->old_uv)return -EIO;
  p->voltage_attempt=false;
 }
 p->owned=false;module_put(THIS_MODULE);
 pr_info(NAME ": GPIO1 six registers restored; L25 off/original voltage restored; DIVCLK unchanged\n");return 0;
}
int superman_audio_board_power_enable(bool enable)
{
 struct audio_power *p;int ret,cleanup;
 mutex_lock(&power_lock);p=power;if(!p){ret=-ENODEV;goto out;}
 if(!enable){ret=restore(p);goto out;}
 if(p->owned){ret=-EBUSY;goto out;}
 ret=superman_speaker_amp_is_off();if(ret)goto out;
 ret=check_pin(p,false);if(ret)goto out;
 ret=check_ldo(p,false);if(ret)goto out;
 if(regulator_is_enabled(p->l25)!=0||regulator_get_voltage(p->l25)!=p->old_uv){ret=-EBUSY;goto out;}
 p->owned=true;__module_get(THIS_MODULE);p->voltage_attempt=true;
 ret=regulator_set_voltage(p->l25,1775000,1775000);if(ret)goto fail;
 if(regulator_get_voltage(p->l25)!=1775000){ret=-EIO;goto fail;}
 ret=regulator_enable(p->l25);
 if(ret){if(regulator_is_enabled(p->l25)!=0)p->uncertain=true;goto fail;}
 p->enabled=true;
 if(regulator_is_enabled(p->l25)!=1){ret=-EIO;goto fail;}
 usleep_range(1000,1200);
 ret=check_ldo(p,true);if(ret)goto fail;
 p->pin_attempt=true;ret=pinctrl_select_state(p->pins,p->on);if(ret)goto fail;
 ret=check_pin(p,true);if(ret)goto fail;p->ready=true;
 pr_info(NAME ": Nokia GPIO1 source4/VIN3 active; DIVCLK1 factor2/enabled retained; L25=1775000uV enabled\n");goto out;
fail:
 cleanup=restore(p);if(cleanup)pr_err(NAME ": rollback=%d, ownership retained; disable retry required\n",cleanup);
out:mutex_unlock(&power_lock);return ret;
}
EXPORT_SYMBOL_GPL(superman_audio_board_power_enable);
bool superman_audio_board_power_ready(void)
{
 bool ready;mutex_lock(&power_lock);ready=power&&power->ready;mutex_unlock(&power_lock);return ready;
}
EXPORT_SYMBOL_GPL(superman_audio_board_power_ready);
static int board_probe(struct platform_device *dev)
{
 struct audio_power *p;struct device *gpio,*ldo;struct device_node *supply,*expected;unsigned v;int ret;const char *stage="allocation";
 p=devm_kzalloc(&dev->dev,sizeof(*p),GFP_KERNEL);if(!p)return -ENOMEM;
 gpio=bus_find_device_by_name(&platform_bus_type,NULL,CTRL);if(!gpio)return -ENODEV;
 p->gpio_map=dev_get_regmap(gpio->parent,NULL);put_device(gpio);if(!p->gpio_map)return -ENODEV;
 ret=regmap_read(p->gpio_map,0xc004,&v);if(ret||v!=0x10)return ret?:-ENODEV;
 ret=regmap_read(p->gpio_map,0x5b04,&v);if(ret||v!=6)return ret?:-ENODEV;
 ret=regmap_read(p->gpio_map,0x5b05,&v);if(ret||v!=0x0b)return ret?:-ENODEV;
 ret=check_pin(p,false);if(ret)return ret;
 if(!dev->dev.of_node)return -ENODEV;
 supply=of_parse_phandle(dev->dev.of_node,"vddio-supply",0);if(!supply)return -ENODEV;
 expected=of_find_node_by_path("/remoteproc/smd-edge/rpm-requests/regulators/l25");
 ret=supply==expected?0:-ENODEV;of_node_put(supply);of_node_put(expected);if(ret)return ret;
 stage="RPM exclusive consumer";
 p->l25=regulator_get_exclusive(&dev->dev,"vddio");if(IS_ERR(p->l25))return PTR_ERR(p->l25);
 /* The supply phandle selects RPM L25; direct SPMI is readback only. */
 stage="native readback device";
 ldo=bus_find_device_by_name(&platform_bus_type,NULL,"fc4cf000.spmi:pm8226@1:regulators");
 if(!ldo){ret=-ENODEV;goto put;}
 p->ldo_map=dev_get_regmap(ldo->parent,NULL);put_device(ldo);
 stage="native and RPM regmap distinction";
 if(!p->ldo_map){ret=-ENODEV;goto put;}
 if(!IS_ERR(regulator_get_regmap(p->l25))){ret=-ENODEV;goto put;}
 stage="native LDO type";
 ret=regmap_read(p->ldo_map,0x5804,&v);if(ret)goto put;if(v!=4){ret=-ENODEV;goto put;}
 stage="RPM initial state";
 p->old_uv=regulator_get_voltage(p->l25);
 if(p->old_uv!=1775000||regulator_is_enabled(p->l25)!=0){ret=-EBUSY;goto put;}
 stage="physical LDO initially off";
 ret=check_ldo(p,false);if(ret)goto put;
 /* Static GROUP maps use a child without OF pinctrl properties. */
 stage="clock pinctrl consumer";
 p->clock_dev=platform_device_register_data(&dev->dev,CLOCK_NAME,PLATFORM_DEVID_NONE,NULL,0);
 if(IS_ERR(p->clock_dev)){ret=PTR_ERR(p->clock_dev);goto put;}
 stage="named pinctrl state acquisition";
 p->pins=pinctrl_get(&p->clock_dev->dev);if(IS_ERR(p->pins)){ret=PTR_ERR(p->pins);goto clock;}
 p->on=pinctrl_lookup_state(p->pins,"clock-on");if(IS_ERR(p->on)){ret=PTR_ERR(p->on);goto pins;}
 p->off=pinctrl_lookup_state(p->pins,"clock-off");if(IS_ERR(p->off)){ret=PTR_ERR(p->off);goto pins;}
 platform_set_drvdata(dev,p);mutex_lock(&power_lock);power=p;mutex_unlock(&power_lock);
 dev_info(&dev->dev,"exact RPM L25 provider reserved while physically off; clock pin unchanged until explicitly enabled\n");return 0;
pins:pinctrl_put(p->pins);
clock:platform_device_unregister(p->clock_dev);
put:dev_err(&dev->dev,"probe failed stage=%s ret=%d\n",stage,ret);regulator_put(p->l25);return ret;
}
static int probe(struct platform_device *dev)
{
 int ret=board_probe(dev);
 if(ret)dev_err(&dev->dev,"board probe returned %d; no audio route started\n",ret);
 return ret;
}
static void remove(struct platform_device *dev)
{
 struct audio_power *p=platform_get_drvdata(dev);
 mutex_lock(&power_lock);power=NULL;mutex_unlock(&power_lock);pinctrl_put(p->pins);platform_device_unregister(p->clock_dev);regulator_put(p->l25);
}
static const struct of_device_id of_match[]={{.compatible="nokia,superman-audio-board-power"},{}};
MODULE_DEVICE_TABLE(of,of_match);
static struct platform_driver driver={.probe=probe,.remove=remove,.driver={.name=NAME,.of_match_table=of_match,.suppress_bind_attrs=true}};
static int __init start(void)
{
 int ret;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 on_cfg[0]=pinconf_to_config_packed(PIN_CONFIG_POWER_SOURCE,3);
 on_cfg[1]=pinconf_to_config_packed(PIN_CONFIG_BIAS_DISABLE,1);
 on_cfg[2]=pinconf_to_config_packed(PIN_CONFIG_INPUT_ENABLE,0);
 on_cfg[3]=pinconf_to_config_packed(PIN_CONFIG_OUTPUT,0);
 off_cfg[0]=pinconf_to_config_packed(PIN_CONFIG_OUTPUT,0);
 off_cfg[1]=pinconf_to_config_packed(PIN_CONFIG_OUTPUT_ENABLE,0);
 off_cfg[2]=pinconf_to_config_packed(PIN_CONFIG_INPUT_ENABLE,1);
 off_cfg[3]=pinconf_to_config_packed(PIN_CONFIG_POWER_SOURCE,0);
 off_cfg[4]=pinconf_to_config_packed(PIN_CONFIG_BIAS_PULL_DOWN,1);
 ret=pinctrl_register_mappings(maps,ARRAY_SIZE(maps));if(ret)return ret;
 ret=platform_driver_register(&driver);if(ret)goto maps;
 if(power)return 0;
 ret=-ENODEV;platform_driver_unregister(&driver);
maps:pinctrl_unregister_mappings(maps);return ret;
}
static void __exit stop(void){platform_driver_unregister(&driver);pinctrl_unregister_mappings(maps);}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Reversible Nokia AUDD GPIO1 clock output and exact L25 supply ownership");
