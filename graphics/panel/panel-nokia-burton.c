// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nokia Lumia 730 "Burton" Samsung AMOLED DSI video-mode panel.
 *
 * Init/term sequences come from the stock UEFI DisplayDxe "Burton" XML; the
 * power sequence (S4, L6, L15, GPIO 110 then 111) from the stock ACPI
 * \_SB.DISP PEP D0/D3 packages. Brightness is DCS 0x51 (UEFI default 0x80).
 * Evidence: handoff/evidence/cc2-panel-burton-20261103.
 */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct burton {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data supplies[3];
	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	bool id_logged;
};

static inline struct burton *to_burton(struct drm_panel *panel)
{
	return container_of(panel, struct burton, panel);
}

static void burton_power_on(struct burton *ctx)
{
	msleep(10);
	gpiod_set_value_cansleep(ctx->enable_gpio, 1);
	msleep(10);
	/* Short reset pulse so a warm panel also starts from a known state. */
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(40);
}

static void burton_power_off(struct burton *ctx)
{
	gpiod_set_value_cansleep(ctx->enable_gpio, 0);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
}

static void burton_log_id(struct burton *ctx, struct mipi_dsi_multi_context *dsi_ctx)
{
	u8 id[3] = { 0 };
	int i, ret;

	if (ctx->id_logged || dsi_ctx->accum_err)
		return;
	for (i = 0; i < 3; i++) {
		ret = mipi_dsi_dcs_read(ctx->dsi, 0xda + i, &id[i], 1);
		if (ret < 0) {
			dev_warn(&ctx->dsi->dev, "panel ID%d read failed: %d\n",
				 i + 1, ret);
			return;
		}
	}
	ctx->id_logged = true;
	dev_info(&ctx->dsi->dev, "panel ID %02x %02x %02x\n", id[0], id[1], id[2]);
}

static int burton_on(struct burton *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf1, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xba, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcb, 0x00, 0x00, 0x00, 0x00,
				     0x4d, 0x7d, 0x40, 0x85);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x1d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcb, 0x59, 0x74);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x2b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcb, 0x08, 0x05, 0x23, 0x48);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf1, 0xa5, 0xa5);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	/* UEFI waits 120 ms then a further 101 ms after sleep out. */
	mipi_dsi_msleep(&dsi_ctx, 221);
	burton_log_id(ctx, &dsi_ctx);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x2c);
	/*
	 * Stay dark until enable: UEFI's fixed 0x80 flashed the old level on
	 * every wake, and display-on before video showed stale panel RAM. The
	 * panel core restores the real level through the backlight after enable.
	 */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00);

	return dsi_ctx.accum_err;
}

/* Video is running by now (prepare_prev_first), so the first lit frame is real. */
static int burton_enable(struct drm_panel *panel)
{
	struct burton *ctx = to_burton(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	return dsi_ctx.accum_err;
}

static int burton_disable(struct drm_panel *panel)
{
	struct burton *ctx = to_burton(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/*
	 * Send the whole off sequence while video still runs: once msm stops
	 * the timing engine its DSI commands wait for a video-done that never
	 * comes and fail with -ETIMEDOUT, leaving the panel out of sleep.
	 */
	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x00);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	/*
	 * The AMOLED's power-off sequence (ELVDD/ELVSS) runs on the link clock
	 * for several frames after sleep-in; UEFI waits 120 ms here. Stopping
	 * video first leaves the high voltage on: a brightening green line
	 * while "off" and a green burst on the next power-up.
	 */
	mipi_dsi_msleep(&dsi_ctx, 120);
	if (dsi_ctx.accum_err)
		dev_err(panel->dev, "failed to enter sleep: %d\n", dsi_ctx.accum_err);
	return dsi_ctx.accum_err;
}

static int burton_prepare(struct drm_panel *panel)
{
	struct burton *ctx = to_burton(panel);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(panel->dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	burton_power_on(ctx);

	ret = burton_on(ctx);
	if (ret < 0) {
		dev_err(panel->dev, "failed to initialize panel: %d\n", ret);
		burton_power_off(ctx);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ret;
	}

	return 0;
}

static int burton_unprepare(struct drm_panel *panel)
{
	struct burton *ctx = to_burton(panel);

	/* Sleep-in and its 120 ms settle already happened in disable. */
	burton_power_off(ctx);
	/* L6/L15 are shared with the touchscreen; the framework keeps them on. */
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

/* UEFI DSIBitClockFrequency 387296000 Hz = 64549333 Hz x 24 bpp / 4 lanes. */
static const struct drm_display_mode burton_mode = {
	.clock = 64549,
	.hdisplay = 720,
	.hsync_start = 720 + 60,
	.hsync_end = 720 + 60 + 4,
	.htotal = 720 + 60 + 4 + 40,
	.vdisplay = 1280,
	.vsync_start = 1280 + 15,
	.vsync_end = 1280 + 15 + 1,
	.vtotal = 1280 + 15 + 1 + 8,
	.width_mm = 59,
	.height_mm = 104,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int burton_get_modes(struct drm_panel *panel,
			    struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &burton_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs burton_panel_funcs = {
	.prepare = burton_prepare,
	.enable = burton_enable,
	.disable = burton_disable,
	.unprepare = burton_unprepare,
	.get_modes = burton_get_modes,
};

static int burton_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				 &(u8){ brightness }, 1);
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return ret < 0 ? ret : 0;
}

static const struct backlight_ops burton_bl_ops = {
	.update_status = burton_bl_update_status,
};

static struct backlight_device *
burton_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 0x80,
		.max_brightness = 0xff,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &burton_bl_ops, &props);
}

static int burton_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct burton *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct burton, panel, &burton_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->supplies[0].supply = "vdd";
	ctx->supplies[1].supply = "vddio";
	ctx->supplies[2].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	/* ASIS: the panel is already powered by UEFI at probe time. */
	ctx->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(ctx->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->enable_gpio),
				     "failed to get enable-gpios\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = burton_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	return 0;
}

static void burton_remove(struct mipi_dsi_device *dsi)
{
	struct burton *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id burton_of_match[] = {
	{ .compatible = "nokia,superman-burton" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, burton_of_match);

static struct mipi_dsi_driver burton_driver = {
	.probe = burton_probe,
	.remove = burton_remove,
	.driver = {
		.name = "panel-nokia-burton",
		.of_match_table = burton_of_match,
	},
};
module_mipi_dsi_driver(burton_driver);

MODULE_DESCRIPTION("Nokia Lumia 730 Burton AMOLED DSI panel");
MODULE_LICENSE("GPL");
