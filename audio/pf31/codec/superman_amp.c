// SPDX-License-Identifier: GPL-2.0-only
/* RM-1040 4jc_04g schematic pp2/4: IHF_PA_EN is TLMM13, active high.
 * AUDD resource2/GPNF role7 corroborates the pin. Role6 PMIC GPIO2 is not
 * connected to the speaker enable and must not be driven for playback. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinconf-generic.h>
#include "superman-codec.h"
#define NAME "superman-speaker-amp"
#define TLMM13 0xfd5110d0
struct amp {struct pinctrl *pinctrl;struct pinctrl_state *on,*off;struct gpio_desc *gpio;void __iomem *pin;bool owned;};
static struct amp *amp;
static struct platform_device *pdev;
static DEFINE_MUTEX(amp_lock);
GPIO_LOOKUP_SINGLE(amp_lookup,NAME,"fd510000.pinctrl",13,"enable",GPIO_ACTIVE_HIGH);
static unsigned long on_configs[1],off_configs[1];
static const struct pinctrl_map amp_maps[]={
 PIN_MAP_CONFIGS_GROUP(NAME,"amp-on","fd510000.pinctrl","gpio13",on_configs),
 PIN_MAP_CONFIGS_GROUP(NAME,"amp-off","fd510000.pinctrl","gpio13",off_configs),
};
/* Verified inherited state: GPIO input, no pull, 2mA; output latch high.
 * Ignore the read-only pad input bit, but verify all writable pin state. */
static int check(struct amp *a,bool active)
{
 return readl(a->pin)==(active?0x200:0) &&
        (readl(a->pin+4)&~1U)==2 ? 0:-EIO;
}
static int restore(struct amp *a)
{
 int ret;
 if(!a->owned)return 0;
 /* Drive low before disabling the hardware output. Keep the descriptor
  * logically output until its original latch is restored: gpiolib rejects
  * set_value on an input. No high level is driven during restoration. */
 ret=gpiod_direction_output(a->gpio,0);if(ret)return ret;
 /* Always transition through a distinct state: select_state caches identity.
  * The latch is already low, so selecting output-enable cannot pulse high. */
 ret=pinctrl_select_state(a->pinctrl,a->on);if(ret)return ret;
 ret=pinctrl_select_state(a->pinctrl,a->off);if(ret)return ret;
 if(readl(a->pin)!=0)return -EIO;
 ret=gpiod_set_value_cansleep(a->gpio,1);if(ret)return ret;
 ret=gpiod_direction_input(a->gpio);if(ret)return ret;
 ret=check(a,false);if(ret)return ret;
 a->owned=false;module_put(THIS_MODULE);
 pr_info(NAME ": TLMM13 off; original input/configuration/output latch restored\n");
 return 0;
}
int superman_speaker_amp_enable(bool enable)
{
 struct amp *a;int ret,rollback;
 mutex_lock(&amp_lock);a=amp;
 if(!a){ret=-ENODEV;goto out;}
 if(!enable){ret=restore(a);goto out;}
 if(a->owned){ret=-EBUSY;goto out;}
 ret=check(a,false);if(ret)goto out;
 a->owned=true;__module_get(THIS_MODULE);
 ret=gpiod_direction_output(a->gpio,1);if(ret)goto fail;
 ret=check(a,true);if(ret)goto fail;
 pr_info(NAME ": TLMM13 IHF_PA_EN enabled; PMIC GPIO2 untouched\n");
 goto out;
fail:
 rollback=restore(a);if(rollback)pr_err(NAME ": rollback=%d; ownership/module retained for disable retry\n",rollback);
out:mutex_unlock(&amp_lock);return ret;
}
EXPORT_SYMBOL_GPL(superman_speaker_amp_enable);
int superman_speaker_amp_is_off(void)
{
 int ret;mutex_lock(&amp_lock);
 ret=!amp?-ENODEV:amp->owned?-EBUSY:check(amp,false);
 mutex_unlock(&amp_lock);return ret;
}
EXPORT_SYMBOL_GPL(superman_speaker_amp_is_off);
static int probe(struct platform_device *dev)
{
 struct amp *a;int ret;
 a=devm_kzalloc(&dev->dev,sizeof(*a),GFP_KERNEL);if(!a)return -ENOMEM;
 a->pin=devm_ioremap(&dev->dev,TLMM13,8);if(!a->pin)return -ENOMEM;
 ret=check(a,false);if(ret)return dev_err_probe(&dev->dev,ret,"unexpected initial TLMM13 state\n");
 a->gpio=devm_gpiod_get(&dev->dev,"enable",GPIOD_ASIS);if(IS_ERR(a->gpio))return PTR_ERR(a->gpio);
 ret=check(a,false);if(ret)return dev_err_probe(&dev->dev,ret,"GPIO request changed TLMM13 state\n");
 a->pinctrl=devm_pinctrl_get(&dev->dev);if(IS_ERR(a->pinctrl))return PTR_ERR(a->pinctrl);
 a->on=pinctrl_lookup_state(a->pinctrl,"amp-on");if(IS_ERR(a->on))return PTR_ERR(a->on);
 a->off=pinctrl_lookup_state(a->pinctrl,"amp-off");if(IS_ERR(a->off))return PTR_ERR(a->off);
 mutex_lock(&amp_lock);amp=a;mutex_unlock(&amp_lock);
 dev_info(&dev->dev,"RM-1040 TLMM13 speaker enable owned ASIS; amplifier remains off\n");return 0;
}
static struct platform_driver driver={.probe=probe,.driver={.name=NAME,.suppress_bind_attrs=true}};
static int __init start(void)
{
 int ret;
 if(!of_machine_is_compatible("nokia,superman") || !of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 on_configs[0]=pinconf_to_config_packed(PIN_CONFIG_OUTPUT_ENABLE,1);
 off_configs[0]=pinconf_to_config_packed(PIN_CONFIG_OUTPUT_ENABLE,0);
 ret=pinctrl_register_mappings(amp_maps,ARRAY_SIZE(amp_maps));if(ret)return ret;
 gpiod_add_lookup_table(&amp_lookup);ret=platform_driver_register(&driver);if(ret)goto lookup;
 pdev=platform_device_register_simple(NAME,-1,NULL,0);
 if(IS_ERR(pdev)){ret=PTR_ERR(pdev);goto driver;}
 if(amp)return 0;
 platform_device_unregister(pdev);ret=-ENODEV;
driver:platform_driver_unregister(&driver);
lookup:gpiod_remove_lookup_table(&amp_lookup);pinctrl_unregister_mappings(amp_maps);return ret;
}
static void __exit stop(void)
{
 mutex_lock(&amp_lock);amp=NULL;mutex_unlock(&amp_lock);
 platform_device_unregister(pdev);platform_driver_unregister(&driver);gpiod_remove_lookup_table(&amp_lookup);pinctrl_unregister_mappings(amp_maps);
}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Lumia 730 schematic-correct TLMM13 speaker amplifier enable with exact restoration");
