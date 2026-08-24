// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung s6e3fc3 MIPI DSI command-mode panel, as fitted to Pixel 6 (oriole).
 *
 * The bootloader brings this panel up and leaves it scanning out of its own
 * RAM, with the DSI link trained and the stream DSC-compressed. This driver
 * does not repeat that bring-up: gs101_dsim programs neither the D-PHY nor
 * DSC, so the vendor's full init sequence would replace a working
 * configuration with one the host cannot feed.
 *
 * It sends the three commands that were established, by poking the registers
 * on a black screen, to be both necessary and sufficient to bring the panel
 * back: sleep out, tear on, display on. The panel keeps its DSC and link
 * configuration across them.
 *
 * TE is the point of the exercise. DECON drives the panel from a hardware
 * trigger sourced from TE (HW_TRIG_SEL_FROM_DDI0), and if TE stops arriving
 * DECON waits forever mid-frame -- FRAME_START pending, FRAME_DONE never
 * raised, interrupt count frozen.
 *
 * Vendor reference: panel-samsung-s6e3fc3.c in google-modules/display, whose
 * init sequence sends 0x11, waits 120 ms, then 0x35, and ends with 0x29.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>

#include <video/mipi_display.h>

#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct s6e3fc3 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
};

static inline struct s6e3fc3 *to_s6e3fc3(struct drm_panel *panel)
{
	return container_of(panel, struct s6e3fc3, panel);
}

static const struct drm_display_mode s6e3fc3_mode = {
	.name = "1080x2400x60",
	.clock = 168498,
	.hdisplay = 1080,
	.hsync_start = 1080 + 32,
	.hsync_end = 1080 + 32 + 12,
	.htotal = 1080 + 32 + 12 + 26,
	.vdisplay = 2400,
	.vsync_start = 2400 + 12,
	.vsync_end = 2400 + 12 + 4,
	.vtotal = 2400 + 12 + 4 + 26,
	.width_mm = 67,
	.height_mm = 148,
};

static int s6e3fc3_dcs(struct s6e3fc3 *ctx, u8 cmd)
{
	ssize_t ret;

	ret = mipi_dsi_dcs_write(ctx->dsi, cmd, NULL, 0);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "DCS %#04x failed: %zd\n", cmd, ret);
		return ret;
	}

	return 0;
}

static int s6e3fc3_prepare(struct drm_panel *panel)
{
	struct s6e3fc3 *ctx = to_s6e3fc3(panel);
	int ret;

	ret = s6e3fc3_dcs(ctx, MIPI_DCS_EXIT_SLEEP_MODE);
	if (ret)
		return ret;

	msleep(120);

	return s6e3fc3_dcs(ctx, MIPI_DCS_SET_TEAR_ON);
}

static int s6e3fc3_enable(struct drm_panel *panel)
{
	struct s6e3fc3 *ctx = to_s6e3fc3(panel);

	return s6e3fc3_dcs(ctx, MIPI_DCS_SET_DISPLAY_ON);
}

static int s6e3fc3_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &s6e3fc3_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs s6e3fc3_panel_funcs = {
	.prepare = s6e3fc3_prepare,
	.enable = s6e3fc3_enable,
	.get_modes = s6e3fc3_get_modes,
};

static int s6e3fc3_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct s6e3fc3 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct s6e3fc3, panel,
				   &s6e3fc3_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	return 0;
}

static void s6e3fc3_remove(struct mipi_dsi_device *dsi)
{
	struct s6e3fc3 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id s6e3fc3_of_match[] = {
	{ .compatible = "samsung,s6e3fc3" },
	{ }
};
MODULE_DEVICE_TABLE(of, s6e3fc3_of_match);

static struct mipi_dsi_driver s6e3fc3_driver = {
	.probe = s6e3fc3_probe,
	.remove = s6e3fc3_remove,
	.driver = {
		.name = "panel-samsung-s6e3fc3",
		.of_match_table = s6e3fc3_of_match,
	},
};
module_mipi_dsi_driver(s6e3fc3_driver);

MODULE_DESCRIPTION("Samsung s6e3fc3 DSI panel driver");
MODULE_LICENSE("GPL");
