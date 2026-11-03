// SPDX-License-Identifier: GPL-2.0-only
/*
 * Inactive laboratory candidate, NOT a cold-start Lumia panel driver.
 * P32 read-only P30 capture: MDP5 1.1.1, DSI6G 1.0.2, RGB888 burst video,
 * lanes 0,1,2,3, 720x1280; inherited PLL gives 64549316Hz pixel clock.
 * All power/reset/backlight/DCS initialization remains owned by firmware.
 * This does NOT make MDP5/DSI startup preserve the firmware display: their
 * takeover, scanout allocation, fences and rollback must be implemented/tested
 * separately before enabling the parent MDSS or selecting a native composer.
 * No suspend/cold-boot/panel-variant support is claimed.
 */
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct superman_panel {
	struct drm_panel panel;
};

static const struct drm_display_mode inherited_mode = {
	/* DRM clocks are integer kHz; retain the measured PLL value in P32. */
	.clock = 64549,
	.hdisplay = 720,
	.hsync_start = 780,
	.hsync_end = 784,
	.htotal = 824,
	.vdisplay = 1280,
	.vsync_start = 1295,
	.vsync_end = 1296,
	.vtotal = 1304,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int superman_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &inherited_mode);
	if (!mode)
		return -ENOMEM;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	connector->display_info.bpc = 8;
	return 1;
}

static const struct drm_panel_funcs superman_panel_funcs = {
	/* No prepare/enable/disable/unprepare: never issue power/reset/DCS. */
	.get_modes = superman_get_modes,
};

static int superman_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct superman_panel *ctx;
	int ret;

	/* This acknowledgement alone is NOT a verified hardware handoff. */
	if (!of_property_read_bool(dev->of_node, "nokia,uefi-initialized-only"))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "cold panel initialization is unsupported\n");
	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;
	drm_panel_init(&ctx->panel, dev, &superman_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	mipi_dsi_set_drvdata(dsi, ctx);
	drm_panel_add(&ctx->panel);
	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return ret;
	}
	dev_info(dev, "P32 firmware-initialized panel candidate; power/DCS unchanged\n");
	return 0;
}

static void superman_panel_remove(struct mipi_dsi_device *dsi)
{
	struct superman_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id superman_panel_of_match[] = {
	{ .compatible = "nokia,superman-uefi-panel" },
	{ }
};
MODULE_DEVICE_TABLE(of, superman_panel_of_match);

static struct mipi_dsi_driver superman_panel_driver = {
	.probe = superman_panel_probe,
	.remove = superman_panel_remove,
	.driver = {
		.name = "panel-superman-uefi",
		.of_match_table = superman_panel_of_match,
	},
};
module_mipi_dsi_driver(superman_panel_driver);

MODULE_DESCRIPTION("Inactive Lumia730 firmware-initialized panel candidate");
MODULE_LICENSE("GPL");
