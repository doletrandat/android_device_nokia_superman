// SPDX-License-Identifier: GPL-2.0-only
/* Lumia 730 stock AUDD GPNF role0/resource0 is TLMM72, active low.
 * Live E0 endpoints and chip major 0x0103/minor 0x0001 appear after release.
 * This provides codec reset only; it does not register a PCM or audio route.
 * CC26: RM-1040 WCD9302 VDD_DIG (pin 19) is PM8226 L4 1.2V, shared with the
 * MIPI DSI vdda. Only the DSI driver voted for it, so every panel sleep cut
 * the codec digital core: the codec then NACKed all SLIM value reads until
 * reboot. Hold our own L4 vote from before reset release until unload. */
#include <linux/module.h>
#include "superman-codec.h"
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>

#define NAME "superman-wcd9302-reset"
struct codec_reset {
 struct gpio_desc *gpio;
 void __iomem *pin;
};
static bool ready;
static struct platform_device *codec_device;
static unsigned vdig_uv;
module_param(vdig_uv,uint,0444);
#define DSI_NAME "fd922800.dsi"

static void release_vdig(void *data)
{
 struct regulator *vdig = data;
 regulator_disable(vdig);
 regulator_put(vdig);
 vdig_uv = 0;
 pr_info(NAME ": released WCD9302 VDD_DIG (L4) vote\n");
}

/* The exact RPM L4 provider is the DSI node's vdda-supply phandle. */
static int hold_vdig(struct device *dev)
{
 struct device *dsi = bus_find_device_by_name(&platform_bus_type, NULL, DSI_NAME);
 struct regulator *vdig;
 int ret, uv;
 if (!dsi) {
  /* Non-KMS fallback boots have no DSI device; L4 then stays as firmware left it. */
  dev_warn(dev, DSI_NAME " absent: VDD_DIG (L4) not held\n");
  return 0;
 }
 vdig = regulator_get_optional(dsi, "vdda");
 put_device(dsi);
 if (IS_ERR(vdig))
  return dev_err_probe(dev, PTR_ERR(vdig), "VDD_DIG (L4) unavailable\n");
 uv = regulator_get_voltage(vdig);
 if (uv != 1200000) {
  regulator_put(vdig);
  return dev_err_probe(dev, -EINVAL, "VDD_DIG voltage %d, expected 1200000\n", uv);
 }
 ret = regulator_enable(vdig);
 if (ret) {
  regulator_put(vdig);
  return dev_err_probe(dev, ret, "VDD_DIG enable failed\n");
 }
 ret = devm_add_action_or_reset(dev, release_vdig, vdig);
 if (ret)
  return ret;
 vdig_uv = uv;
 /* Supply up (it may have been off) before the reset pulse. */
 usleep_range(5000, 6000);
 return 0;
}
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
 ret = hold_vdig(dev);
 if (ret)
  return ret;
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
 dev_info(dev, "TLMM72 codec reset released, VDD_DIG L4 %uuV held; no codec gain/route writes\n", vdig_uv);
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
