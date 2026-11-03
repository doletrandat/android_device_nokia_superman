// SPDX-License-Identifier: GPL-2.0-only
/* Lumia 730 stock AUDD GPNF role0/resource0 is TLMM72, active low.
 * Live E0 endpoints and chip major 0x0103/minor 0x0001 appear after release.
 * This provides codec reset only; it does not register a PCM or audio route. */
#include <linux/module.h>
#include "superman-codec.h"
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/delay.h>

#define NAME "superman-wcd9302-reset"
struct codec_reset {
 struct gpio_desc *gpio;
 void __iomem *pin;
};
static bool ready;
static struct platform_device *codec_device;
GPIO_LOOKUP_SINGLE(reset_lookup, NAME, "fd510000.pinctrl", 72, "reset", GPIO_ACTIVE_LOW);

static void restore_reset(void *data)
{
 struct codec_reset *r = data;
 int ret;
 gpiod_set_value_cansleep(r->gpio, 1);
 ret = gpiod_direction_input(r->gpio);
 ready = false;
 if (ret || readl(r->pin) != 1 || (readl(r->pin + 4) & 2))
  pr_err(NAME ": pin restoration failed: ret=%d cfg=%08x io=%08x\n",
         ret, readl(r->pin), readl(r->pin + 4));
 else
  pr_info(NAME ": restored TLMM72 input/pull-down/output-latch-low\n");
}

static int codec_probe(struct platform_device *pdev)
{
 struct device *dev = &pdev->dev;
 struct codec_reset *r;
 int ret;
 r = devm_kzalloc(dev, sizeof(*r), GFP_KERNEL);
 if (!r)
  return -ENOMEM;
 r->pin = devm_ioremap(dev, 0xfd511480, 8);
 if (!r->pin)
  return -ENOMEM;
 /* Refuse to take over a pin already configured by another bring-up path. */
 if (readl(r->pin) != 1 || (readl(r->pin + 4) & 2))
  return dev_err_probe(dev, -EBUSY, "unexpected initial TLMM72 state\n");
 r->gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
 if (IS_ERR(r->gpio))
  return dev_err_probe(dev, PTR_ERR(r->gpio), "reset GPIO unavailable\n");
 ret = devm_add_action_or_reset(dev, restore_reset, r);
 if (ret)
  return ret;
 msleep(20);
 gpiod_set_value_cansleep(r->gpio, 0);
 msleep(20);
 if ((readl(r->pin) & 0x21c) != 0x200 || !(readl(r->pin + 4) & 2))
  return dev_err_probe(dev, -EIO, "reset did not deassert\n");
 ready = true;
 dev_info(dev, "TLMM72 codec reset released; no codec gain/route writes\n");
 return 0;
}

static struct platform_driver codec_driver = {
 .probe = codec_probe,
 .driver = {.name = NAME},
};

static int __init codec_init(void)
{
 int ret;
 if (!of_machine_is_compatible("nokia,superman") ||
     !of_machine_is_compatible("qcom,msm8226"))
  return -ENODEV;
 gpiod_add_lookup_table(&reset_lookup);
 ret = platform_driver_register(&codec_driver);
 if (ret)
  goto lookup;
 codec_device = platform_device_register_simple(NAME, -1, NULL, 0);
 if (IS_ERR(codec_device)) {
  ret = PTR_ERR(codec_device);
  goto driver;
 }
 if (ready)
  return 0;
 platform_device_unregister(codec_device);
 ret = -ENODEV;
driver:
 platform_driver_unregister(&codec_driver);
lookup:
 gpiod_remove_lookup_table(&reset_lookup);
 return ret;
}

static void __exit codec_exit(void)
{
 platform_device_unregister(codec_device);
 platform_driver_unregister(&codec_driver);
 gpiod_remove_lookup_table(&reset_lookup);
}
bool superman_wcd9302_reset_ready(void)
{
 return READ_ONCE(ready);
}
EXPORT_SYMBOL_GPL(superman_wcd9302_reset_ready);

module_init(codec_init);
module_exit(codec_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Lumia 730 WCD9302 reset ownership and release");
