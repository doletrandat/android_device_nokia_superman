// SPDX-License-Identifier: GPL-2.0-only
/* CC54: give the WCNSS Wi-Fi and Bluetooth nodes this phone's own addresses (from DPP, read by
 * superman-firmware.sh) before wcn36xx/btqcomsmd load, so the shared DTB carries no addresses.
 * The shared DTB carries placeholders (Wi-Fi 02:00:00:00:00:00, BT the same address in DT byte order) so LK does
 * not insert its own generated addresses (which would make the runtime FDT differ per phone). This module swaps
 * exactly those placeholders for the DPP addresses (of_remove_property + of_add_property; /sys/firmware/fdt, which
 * the boot contracts hash, is the original blob and does not change). A real address is never replaced.
 * Parameters are 12 hex digits in display order (BT is stored little-endian in the tree, as in DT bindings).
 * Permanent: the properties must outlive the module, so it has no exit function.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/etherdevice.h>
#include <linux/hex.h>

static char *wlan, *bt;
static char wlan_result[16] = "unset", bt_result[16] = "unset";
module_param(wlan, charp, 0400);
module_param(bt, charp, 0400);
module_param_string(wlan_result, wlan_result, sizeof(wlan_result), 0444);
module_param_string(bt_result, bt_result, sizeof(bt_result), 0444);

static int add(const char *compat, const char *name, const char *hex, bool reverse, char *result)
{
	struct device_node *np;
	struct property *prop, *old;
	static const u8 wifi_placeholder[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 0 };
	static const u8 bt_placeholder[ETH_ALEN] = { 0, 0, 0, 0, 0, 0x02 };	/* 02:00:00:00:00:00 in DT order */
	const u8 *placeholder = reverse ? bt_placeholder : wifi_placeholder;
	u8 addr[ETH_ALEN], *val;
	int i, ret;

	if (!hex || strlen(hex) != 12 || hex2bin(addr, hex, ETH_ALEN)) {
		strscpy(result, "bad-param", 16);
		return -EINVAL;
	}
	if (!is_valid_ether_addr(addr)) {		/* unicast, not all-zero */
		strscpy(result, "invalid", 16);
		return -EINVAL;
	}
	np = of_find_compatible_node(NULL, NULL, compat);
	if (!np) {
		strscpy(result, "no-node", 16);
		return -ENODEV;
	}
	old = of_find_property(np, name, NULL);
	if (old && !(old->length == ETH_ALEN && !memcmp(old->value, placeholder, ETH_ALEN))) {
		strscpy(result, "present", 16);	/* a real address (old DTB): leave it */
		of_node_put(np);
		return 0;
	}
	prop = kzalloc(sizeof(*prop), GFP_KERNEL);
	val = kmalloc(ETH_ALEN, GFP_KERNEL);
	if (prop)
		prop->name = kstrdup(name, GFP_KERNEL);
	if (!prop || !val || !prop->name) {
		strscpy(result, "nomem", 16);
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < ETH_ALEN; i++)
		val[i] = reverse ? addr[ETH_ALEN - 1 - i] : addr[i];
	prop->value = val;
	prop->length = ETH_ALEN;
	if (old) {
		ret = of_remove_property(np, old);	/* the placeholder moves to deadprops, not freed */
		if (ret) {
			strscpy(result, "remove-failed", 16);
			goto out;
		}
	}
	ret = of_add_property(np, prop);
	strscpy(result, ret ? "add-failed" : (old ? "replaced" : "added"), 16);
	if (!ret) {
		of_node_put(np);
		return 0;
	}
out:
	if (prop)
		kfree(prop->name);
	kfree(prop);
	kfree(val);
	of_node_put(np);
	return ret;
}

static int __init superman_dpp_addr_init(void)
{
	if (!of_machine_is_compatible("nokia,superman"))
		return -ENODEV;
	add("qcom,wcnss-wlan", "local-mac-address", wlan, false, wlan_result);
	add("qcom,wcnss-bt", "local-bd-address", bt, true, bt_result);
	pr_info("SUPERMAN_DPP_ADDR wlan=%s bt=%s\n", wlan_result, bt_result);
	return 0;	/* results are reported through the *_result parameters */
}
module_init(superman_dpp_addr_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Lumia 730: Wi-Fi/BT addresses from DPP into the live device tree");
