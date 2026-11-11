// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lumia 730 ("superman") camera identification probe (test module, no DT changes).
 *
 * Powers one camera the way the Windows Phone PEP tables describe (minus LVS1/SMPS4, whose
 * feed is unknown: see below), starts the MSM8226 CCI (camera I2C) controller, scans the bus
 * and reads common chip-ID registers, then powers everything off and restores the pins. All
 * results go to the kernel log ("SUPERMAN_CAMPROBE"). Load with insmod; it does its work at
 * load time and stays loaded only so it can be removed with rmmod.
 *
 *   cam=0  rear  (CSI0, MCLK0 on GPIO 26, reset GPIO 36)
 *   cam=1  front (CSI1, MCLK1 on GPIO 27, reset GPIO 35)
 *
 * Power: L19 2.85 V (camera analog, off and unused otherwise) through the RPM regulator. L6
 * 1.8 V is already on. LVS1 (switch; feeds sensor I/O and the CCI pull-ups: with it off SDA/SCL
 * read 0 even with the TLMM pull-ups) is voted on through the RPM (VSA 1, "swen"), as the
 * Windows PEP does. Its input is undocumented; Windows runs the camera with SMPS4 at 2.1 V
 * whenever GPU/Wi-Fi/I2C3 vote it, so S4 at 2.1 V here matches the Windows condition.
 *
 * The RPM l19 node has no phandle (nothing references it), so the probe gives it a temporary
 * one, adds a temporary "superman-camavdd-supply" property to /chosen, gets the regulator via
 * of_regulator_get() and undoes both afterwards.
 */
#define pr_fmt(fmt) "SUPERMAN_CAMPROBE " fmt

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>
#include <linux/of_platform.h>
#include <linux/soc/qcom/smd-rpm.h>

static int cam;
module_param(cam, int, 0444);
MODULE_PARM_DESC(cam, "0 = rear (default), 1 = front");
static unsigned int mclk_hz = 19200000;
module_param(mclk_hz, uint, 0444);
static bool avdd = true;
module_param(avdd, bool, 0444);
MODULE_PARM_DESC(avdd, "enable L19 2.85 V (default yes)");
static bool lvs1 = true;
module_param(lvs1, bool, 0444);
MODULE_PARM_DESC(lvs1, "vote LVS1 (sensor I/O + CCI pull-ups) on via the RPM (default yes)");

#define RPM_KEY_SWEN		0x6e657773	/* "swen" */

/* The RPM regulator driver has no lvs1 node, so vote VSA 1 directly, as Windows does. */
static int lvs1_vote(bool on)
{
	struct { __le32 key, nbytes, value; } req = {
		cpu_to_le32(RPM_KEY_SWEN), cpu_to_le32(4), cpu_to_le32(on) };
	struct device_node *np;
	struct platform_device *rp;
	struct qcom_smd_rpm *rpm;
	int ret;

	np = of_find_node_by_path("/remoteproc/smd-edge/rpm-requests/regulators");
	rp = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	if (!rp)
		return -ENODEV;
	rpm = rp->dev.parent ? dev_get_drvdata(rp->dev.parent) : NULL;
	ret = rpm ? qcom_rpm_smd_write(rpm, QCOM_SMD_RPM_ACTIVE_STATE, QCOM_SMD_RPM_VSA, 1,
				       &req, sizeof(req)) : -ENODEV;
	put_device(&rp->dev);
	return ret;
}

#define TLMM_BASE		0xfd510000
#define TLMM_SIZE		0x2000
#define TLMM_CTL(n)		(0x1000 + 0x10 * (n))
#define TLMM_IO(n)		(0x1004 + 0x10 * (n))
#define CCI_BASE		0xfda0c000
#define CCI_SIZE		0x1000

/* mmcc-msm8974/8226 clock IDs (dt-bindings/clock/qcom,mmcc-msm8974.h) */
#define CAMSS_CCI_CCI_AHB_CLK	44
#define CAMSS_CCI_CCI_CLK	45
#define CAMSS_MCLK0_CLK		77
#define CAMSS_MCLK1_CLK		78
#define CAMSS_TOP_AHB_CLK	85
#define MMSS_MMSSNOC_AHB_CLK	111

/* CCI registers (drivers/i2c/busses/i2c-qcom-cci.c), master 0 only */
#define CCI_HW_VERSION		0x000
#define CCI_RESET_CMD		0x004
#define CCI_RESET_CMD_MASK	0x0f73f3f7
#define CCI_RESET_CMD_M0_MASK	0x000003f1
#define CCI_QUEUE_START		0x008
#define CCI_HALT_REQ		0x034
#define CCI_SCL_CTL		0x100
#define CCI_SDA_CTL_0		0x104
#define CCI_SDA_CTL_1		0x108
#define CCI_SDA_CTL_2		0x10c
#define CCI_MISC_CTL		0x110
#define CCI_READ_DATA		0x118
#define CCI_READ_BUF_LEVEL	0x11c
#define CCI_EXEC_WORD_CNT(q)	(0x300 + 0x100 * (q))
#define CCI_CUR_WORD_CNT(q)	(0x304 + 0x100 * (q))
#define CCI_LOAD_DATA(q)	(0x310 + 0x100 * (q))
#define CCI_IRQ_GLOBAL_CLEAR	0xc00
#define CCI_IRQ_MASK_0		0xc04
#define CCI_IRQ_CLEAR_0		0xc08
#define CCI_IRQ_STATUS_0	0xc0c
#define ST_M0_RD_DONE		BIT(0)
#define ST_M0_Q0_REPORT		BIT(4)
#define ST_M0_Q1_REPORT		BIT(8)
#define ST_RST_DONE		BIT(24)
#define ST_M0_HALT_ACK		BIT(25)
#define ST_M0_Q0_NACK		BIT(27)
#define ST_M0_Q1_NACK		BIT(28)
#define ST_M0_ERROR		0x18000ee6
#define CMD_SET_PARAM		1
#define CMD_REPORT		8
#define CMD_WRITE		9
#define CMD_READ		10
#define CMD_REPORT_IRQ_EN	BIT(8)

struct pinsave { int pin; u32 ctl, io; };

static void __iomem *tlmm, *cci;
static struct platform_device *pdev;

static void pin_set(int pin, int func, int pull, int drv, int oe, int out)
{
	writel((func << 2) | pull | (drv << 6) | (oe << 9), tlmm + TLMM_CTL(pin));
	writel(out << 1, tlmm + TLMM_IO(pin));
}

static void cci_clear(u32 val)
{
	writel(val, cci + CCI_IRQ_CLEAR_0);
	writel(1, cci + CCI_IRQ_GLOBAL_CLEAR);
}

/* Wait for any of |bits| (or an error); returns the status seen, 0 on timeout. */
static u32 cci_wait(u32 bits)
{
	int i;
	u32 st;

	for (i = 0; i < 400; i++) {
		st = readl(cci + CCI_IRQ_STATUS_0);
		if (st & (bits | ST_M0_ERROR)) {
			cci_clear(st);
			return st;
		}
		usleep_range(50, 100);
	}
	return 0;
}

static void cci_params(void)
{
	/* cci_v1_data I2C_MODE_STANDARD (100 kHz) */
	writel(78 << 16 | 114, cci + CCI_SCL_CTL);
	writel(28 << 16 | 28, cci + CCI_SDA_CTL_0);
	writel(10 << 16 | 77, cci + CCI_SDA_CTL_1);
	writel(118, cci + CCI_SDA_CTL_2);
	writel(0 << 8 | 6 << 4 | 1, cci + CCI_MISC_CTL);
}

static int cci_reset_all(void)
{
	writel(CCI_RESET_CMD_MASK, cci + CCI_RESET_CMD);
	if (!(cci_wait(ST_RST_DONE) & ST_RST_DONE))
		return -ETIMEDOUT;
	cci_params();
	return 0;
}

/* After an error the master must be halted and reset (as cci_isr() does). */
static void cci_recover(void)
{
	writel(1, cci + CCI_HALT_REQ);
	cci_wait(ST_M0_HALT_ACK);
	writel(CCI_RESET_CMD_M0_MASK, cci + CCI_RESET_CMD);
	cci_wait(ST_RST_DONE);
	cci_params();
}

static void cci_dump(const char *why)
{
	pr_info("%s: irq_status 0x%08x q0 cur %u cmd 0x%08x rep 0x%08x q1 cur %u cmd 0x%08x rep 0x%08x rdlvl %u\n",
		why, readl(cci + CCI_IRQ_STATUS_0), readl(cci + CCI_CUR_WORD_CNT(0)),
		readl(cci + 0x308), readl(cci + 0x30c), readl(cci + CCI_CUR_WORD_CNT(1)),
		readl(cci + 0x408), readl(cci + 0x40c), readl(cci + CCI_READ_BUF_LEVEL));
}

static void pin_levels(const char *why)
{
	pr_info("%s: gpio29(sda) %u gpio30(scl) %u\n", why, readl(tlmm + TLMM_IO(29)) & 1,
		readl(tlmm + TLMM_IO(30)) & 1);
}

/* Run queue |q| of master 0; returns 0, -ENXIO on NACK, -EIO / -ETIMEDOUT otherwise. */
static int cci_run(int q, u32 done)
{
	u32 st;

	writel(readl(cci + CCI_CUR_WORD_CNT(q)), cci + CCI_EXEC_WORD_CNT(q));
	writel(BIT(q), cci + CCI_QUEUE_START);
	st = cci_wait(done);
	if (!st) {
		static int dumped;

		if (dumped++ < 2)
			cci_dump("timeout");
		cci_reset_all();
		return -ETIMEDOUT;
	}
	if (st & ST_M0_ERROR) {
		cci_recover();
		return (st & (ST_M0_Q0_NACK | ST_M0_Q1_NACK)) ? -ENXIO : -EIO;
	}
	return 0;
}

static int cci_write(u8 addr, const u8 *buf, int len)
{
	u8 load[12] = { 0 };
	int i = 0, j;

	if (len > 10)
		return -EINVAL;
	writel(CMD_SET_PARAM | (addr & 0x7f) << 4, cci + CCI_LOAD_DATA(0));
	load[i++] = CMD_WRITE | len << 4;
	for (j = 0; j < len; j++)
		load[i++] = buf[j];
	for (j = 0; j < i; j += 4)
		writel(load[j] | load[j + 1] << 8 | load[j + 2] << 16 | load[j + 3] << 24,
		       cci + CCI_LOAD_DATA(0));
	writel(CMD_REPORT | CMD_REPORT_IRQ_EN, cci + CCI_LOAD_DATA(0));
	return cci_run(0, ST_M0_Q0_REPORT);
}

static int cci_read(u8 addr, u8 *buf, int len)
{
	u32 val, words;
	int i, idx = 0, ret;
	bool first = true;

	writel(CMD_SET_PARAM | (addr & 0x7f) << 4, cci + CCI_LOAD_DATA(1));
	writel(CMD_READ | len << 4, cci + CCI_LOAD_DATA(1));
	ret = cci_run(1, ST_M0_RD_DONE);
	if (ret)
		return ret;
	words = readl(cci + CCI_READ_BUF_LEVEL);
	if (words != len / 4 + 1)
		return -EIO;
	do {
		val = readl(cci + CCI_READ_DATA);
		for (i = 0; i < 4 && idx < len; i++) {
			if (first) {	/* first byte is the address echo */
				first = false;
				continue;
			}
			buf[idx++] = (val >> (i * 8)) & 0xff;
		}
	} while (--words);
	return 0;
}

static int reg16_read(u8 addr, u16 reg, u8 *buf, int len)
{
	u8 r[2] = { reg >> 8, reg & 0xff };
	int ret = cci_write(addr, r, 2);

	return ret ? ret : cci_read(addr, buf, len);
}

static int reg8_read(u8 addr, u8 reg, u8 *buf, int len)
{
	int ret = cci_write(addr, &reg, 1);

	return ret ? ret : cci_read(addr, buf, len);
}

static struct clk *mm_clk(struct device_node *mmcc, int id)
{
	struct of_phandle_args args = { .np = mmcc, .args_count = 1, .args = { id } };

	return of_clk_get_from_provider(&args);
}

static void scan(void)
{
	int addr, found = 0, i;

	for (addr = 0x08; addr <= 0x77; addr++) {
		u8 z[2] = { 0, 0 };
		int ret = cci_write(addr, z, 2);

		if (ret == -ENXIO)
			continue;
		if (ret) {
			pr_info("addr 0x%02x: bus error %d\n", addr, ret);
			continue;
		}
		found++;
		pr_info("addr 0x%02x (8-bit 0x%02x): ACK\n", addr, addr << 1);
		for (i = 0; i < 0x30; i += 8) {
			u8 d[8] = { };

			if (!reg16_read(addr, i, d, 8))
				pr_info("  r16 %04x: %*ph\n", i, 8, d);
		}
		{
			static const u16 extra[] = { 0x0100, 0x0136, 0x1000, 0x1040, 0x300a, 0x3000 };

			for (i = 0; i < ARRAY_SIZE(extra); i++) {
				u8 d[8] = { };

				if (!reg16_read(addr, extra[i], d, 8))
					pr_info("  r16 %04x: %*ph\n", extra[i], 8, d);
			}
		}
		for (i = 0; i < 0x10; i += 8) {
			u8 d[8] = { };

			if (!reg8_read(addr, i, d, 8))
				pr_info("  r8  %02x:   %*ph\n", i, 8, d);
		}
	}
	pr_info("scan done: %d device(s)\n", found);
}

static int __init camprobe_init(void)
{
	const int mclk_pin = cam ? 27 : 26, rst_pin = cam ? 35 : 36;
	const int clk_ids[] = { MMSS_MMSSNOC_AHB_CLK, CAMSS_TOP_AHB_CLK, CAMSS_CCI_CCI_AHB_CLK,
				CAMSS_CCI_CCI_CLK, cam ? CAMSS_MCLK1_CLK : CAMSS_MCLK0_CLK };
	struct pinsave pins[] = { { mclk_pin }, { 29 }, { 30 }, { rst_pin } };
	struct clk *clks[ARRAY_SIZE(clk_ids)] = { };
	struct device_node *mmcc = NULL, *l19 = NULL, *chosen = NULL;
	struct property *prop = NULL;
	struct regulator *reg = NULL;
	__be32 *phval = NULL;
	bool added = false, lvs1_on = false;
	int i, ret = 0;

	pr_info("start cam=%d (%s) mclk=%u Hz avdd=%d\n", cam, cam ? "front" : "rear", mclk_hz,
		avdd);
	pdev = platform_device_register_simple("superman-camprobe", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	tlmm = ioremap(TLMM_BASE, TLMM_SIZE);
	cci = ioremap(CCI_BASE, CCI_SIZE);
	if (!tlmm || !cci) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		pins[i].ctl = readl(tlmm + TLMM_CTL(pins[i].pin));
		pins[i].io = readl(tlmm + TLMM_IO(pins[i].pin));
	}

	/* Reset asserted (low) while powering up. */
	pin_set(rst_pin, 0, 0, 0, 1, 0);

	if (avdd) {
		l19 = of_find_node_by_path("/remoteproc/smd-edge/rpm-requests/regulators/l19");
		chosen = of_find_node_by_path("/chosen");
		if (!l19 || !chosen || l19->phandle || of_find_node_by_phandle(0x7fffcafe)) {
			pr_err("cannot set up the L19 lookup (node %p phandle %u)\n", l19,
			       l19 ? l19->phandle : 0);
			ret = -ENODEV;
			goto power_off;
		}
		l19->phandle = 0x7fffcafe;
		prop = kzalloc(sizeof(*prop), GFP_KERNEL);
		phval = kzalloc(sizeof(*phval), GFP_KERNEL);
		if (!prop || !phval) {
			ret = -ENOMEM;
			goto power_off;
		}
		*phval = cpu_to_be32(0x7fffcafe);
		prop->name = "superman-camavdd-supply";
		prop->value = phval;
		prop->length = sizeof(*phval);
		ret = of_add_property(chosen, prop);
		if (ret)
			goto power_off;
		added = true;
		reg = of_regulator_get(&pdev->dev, chosen, "superman-camavdd");
		if (IS_ERR(reg)) {
			ret = PTR_ERR(reg);
			reg = NULL;
			pr_err("L19 regulator: %d\n", ret);
			goto power_off;
		}
		ret = regulator_set_voltage(reg, 2850000, 2850000);
		if (!ret)
			ret = regulator_enable(reg);
		if (ret) {
			pr_err("L19 enable: %d\n", ret);
			goto power_off;
		}
		pr_info("L19 on: %d uV\n", regulator_get_voltage(reg));
		usleep_range(1000, 2000);
	}

	if (lvs1) {
		ret = lvs1_vote(true);
		pr_info("LVS1 vote on: %d\n", ret);
		if (ret)
			goto power_off;
		lvs1_on = true;
		usleep_range(1000, 2000);
	}

	mmcc = of_find_compatible_node(NULL, NULL, "qcom,mmcc-msm8226");
	if (!mmcc) {
		ret = -ENODEV;
		goto power_off;
	}
	for (i = 0; i < ARRAY_SIZE(clk_ids); i++) {
		clks[i] = mm_clk(mmcc, clk_ids[i]);
		if (IS_ERR(clks[i])) {
			pr_err("clock %d: %ld\n", clk_ids[i], PTR_ERR(clks[i]));
			ret = PTR_ERR(clks[i]);
			clks[i] = NULL;
			goto power_off;
		}
		if (clk_ids[i] == CAMSS_CCI_CCI_CLK)
			clk_set_rate(clks[i], 19200000);
		if (i == ARRAY_SIZE(clk_ids) - 1)
			clk_set_rate(clks[i], mclk_hz);
		ret = clk_prepare_enable(clks[i]);
		if (ret) {
			pr_err("clock %d enable: %d\n", clk_ids[i], ret);
			clk_put(clks[i]);
			clks[i] = NULL;
			goto power_off;
		}
	}
	pr_info("clocks on: cci %lu Hz, mclk %lu Hz\n", clk_get_rate(clks[3]),
		clk_get_rate(clks[4]));

	/* MCLK out, CCI SDA/SCL with pull-up, then release reset. */
	pin_set(mclk_pin, 1, 0, 1, 0, 0);
	pin_set(29, 0, 0, 0, 0, 0);
	pin_set(30, 0, 0, 0, 0, 0);
	usleep_range(500, 1000);
	pin_levels("gpio input, no pull");
	pin_set(29, 0, 3, 0, 0, 0);
	pin_set(30, 0, 3, 0, 0, 0);
	usleep_range(500, 1000);
	pin_levels("gpio input, pull-up");
	pin_set(29, 1, 3, 0, 0, 0);
	pin_set(30, 1, 3, 0, 0, 0);
	usleep_range(1000, 2000);
	pin_set(rst_pin, 0, 0, 0, 1, 1);
	msleep(20);

	pr_info("CCI hw version 0x%08x\n", readl(cci + CCI_HW_VERSION));
	writel(0x7fffffff & (ST_M0_RD_DONE | ST_M0_Q0_REPORT | ST_M0_Q1_REPORT | ST_RST_DONE |
			     ST_M0_HALT_ACK | ST_M0_ERROR), cci + CCI_IRQ_MASK_0);
	cci_clear(0xffffffff);
	if (cci_reset_all()) {
		pr_err("CCI reset timeout\n");
		ret = -ETIMEDOUT;
	} else {
		pin_levels("after CCI reset");
		scan();
	}
	writel(0, cci + CCI_IRQ_MASK_0);
	cci_clear(0xffffffff);

power_off:
	pin_set(rst_pin, 0, 0, 0, 1, 0);
	usleep_range(1000, 2000);
	for (i = ARRAY_SIZE(clk_ids) - 1; i >= 0; i--) {
		if (clks[i]) {
			clk_disable_unprepare(clks[i]);
			clk_put(clks[i]);
		}
	}
	if (lvs1_on)
		pr_info("LVS1 vote off: %d\n", lvs1_vote(false));
	if (reg) {
		if (regulator_is_enabled(reg))
			regulator_disable(reg);
		regulator_put(reg);
	}
	if (added)
		of_remove_property(chosen, prop);	/* moved to the deadprops list; not freed */
	else {
		kfree(prop);
		kfree(phval);
	}
	if (l19 && l19->phandle == 0x7fffcafe)
		l19->phandle = 0;
	of_node_put(l19);
	of_node_put(chosen);
	of_node_put(mmcc);
	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		writel(pins[i].ctl, tlmm + TLMM_CTL(pins[i].pin));
		writel(pins[i].io, tlmm + TLMM_IO(pins[i].pin));
	}
	pr_info("powered off and restored (ret %d)\n", ret);
out:
	if (cci)
		iounmap(cci);
	if (tlmm)
		iounmap(tlmm);
	cci = tlmm = NULL;
	return 0;	/* stay loaded so rmmod can clean up the platform device */
}

static void __exit camprobe_exit(void)
{
	platform_device_unregister(pdev);
}

module_init(camprobe_init);
module_exit(camprobe_exit);
MODULE_DESCRIPTION("Lumia 730 camera sensor identification probe (test only)");
MODULE_LICENSE("GPL");
