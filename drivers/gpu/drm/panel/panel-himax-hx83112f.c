// SPDX-License-Identifier: GPL-2.0-only
/*
 * JDI HX83112F C-PHY panel used by Samsung Galaxy A22 5G.
 * Initialization and power sequencing from Samsung SM-A226B GPL sources:
 * panel-wt-jdi-hx83112f-cphy-vdo-90hz.c, Copyright (c) 2015 MediaTek Inc.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct hx83112f {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset;
	bool enabled;
};

enum hx83112f_supply { HX83112F_VDD, HX83112F_VSP, HX83112F_VSN };

static const struct regulator_bulk_data hx83112f_supplies[] = {
	{ .supply = "vdd" },
	{ .supply = "vsp" },
	{ .supply = "vsn" },
};

static inline struct hx83112f *to_hx83112f(struct drm_panel *panel)
{
	return container_of(panel, struct hx83112f, panel);
}

static int hx83112f_init(struct hx83112f *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb9, 0x83, 0x11, 0x2f);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xce, 0x00, 0x50, 0x00, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb1,
				0x40, 0x27, 0x27, 0x80, 0x80, 0xaa, 0xa6, 0x2a,
				0x26, 0x06, 0x06);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb1, 0x9b, 0x00, 0x81);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x02);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb1, 0x2d, 0x2d);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb2,
				0x00, 0x02, 0x08, 0x90, 0x68, 0x00, 0x1c, 0x4c,
				0x10, 0x08, 0x00, 0x6e, 0x11, 0x01, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb4,
				0x00, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x72,
				0x00, 0x78, 0x00, 0x00, 0x00, 0x72, 0x00, 0x00,
				0x01, 0x02, 0x00, 0x1a, 0x00, 0x0c, 0x00, 0x0d);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe0,
				0x00, 0x10, 0x2a, 0x2c, 0x34, 0x6c, 0x7c, 0x81,
				0x84, 0x84, 0x84, 0x81, 0x7e, 0x7d, 0x7c, 0x7c,
				0x7d, 0x7f, 0x82, 0x9b, 0xae, 0x48, 0x67, 0x00,
				0x10, 0x2a, 0x2c, 0x34, 0x6c, 0x7c, 0x81, 0x84,
				0x84, 0x84, 0x81, 0x7e, 0x7d, 0x7c, 0x7c, 0x7d,
				0x7f, 0x82, 0x9b, 0xae, 0x48, 0x67);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe9, 0xc5);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xba, 0x11);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbf, 0x0b);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc0,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x33, 0x33, 0x33, 0x00, 0x33, 0x33, 0x33);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc7, 0xf0, 0x20);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc7, 0x44);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xcb, 0x5f, 0x19, 0x06, 0xe6);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xcc, 0x0a);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xd3,
				0x81, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x03, 0x37, 0x1c, 0x1c, 0x1e, 0x1a, 0x00, 0x00,
				0x32, 0x20, 0x32, 0x10, 0x1c, 0x00, 0x1c, 0x32,
				0x10, 0x00, 0x00, 0x00, 0x32, 0x10, 0x08, 0x00,
				0x00, 0x00, 0x00, 0x1c, 0x09, 0x86);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xd5,
				0x18, 0x18, 0x19, 0x18, 0x18, 0x18, 0x18, 0x20,
				0x18, 0x18, 0x10, 0x10, 0x18, 0x18, 0x18, 0x18,
				0x01, 0x01, 0x00, 0x00, 0x03, 0x03, 0x02, 0x02,
				0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
				0x18, 0x18, 0x31, 0x31, 0x2f, 0x2f, 0x30, 0x30,
				0x18, 0x18, 0x35, 0x35, 0x36, 0x36, 0x37, 0x37);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xd6,
				0x18, 0x18, 0x19, 0x18, 0x18, 0x18, 0x19, 0x20,
				0x18, 0x18, 0x10, 0x10, 0x18, 0x18, 0x18, 0x18,
				0x02, 0x02, 0x03, 0x03, 0x00, 0x00, 0x01, 0x01,
				0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
				0x18, 0x18, 0x31, 0x31, 0x2f, 0x2f, 0x30, 0x30,
				0x18, 0x18, 0x35, 0x35, 0x36, 0x36, 0x37, 0x37);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xd8,
				0xaa, 0xaa, 0xff, 0xaa, 0xbf, 0xaa, 0xaa, 0xaa,
				0xff, 0xaa, 0xbf, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
				0xbf, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xbf, 0xaa);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xd8,
				0xaa, 0xaa, 0xff, 0xaa, 0xbf, 0xaa, 0xaa, 0xaa,
				0xff, 0xaa, 0xbf, 0xaa, 0xaa, 0xaa, 0xff, 0xaa,
				0xbf, 0xaa, 0xaa, 0xaa, 0xff, 0xaa, 0xbf, 0xaa);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x02);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xd8,
				0xaa, 0xaa, 0xff, 0xaa, 0xbf, 0xaa, 0xaa, 0xaa,
				0xff, 0xaa, 0xbf, 0xaa);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x03);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xd8,
				0xba, 0xaa, 0xaa, 0xaa, 0xbf, 0xaa, 0xaa, 0xaa,
				0xaa, 0xaa, 0xbf, 0xaa, 0xaa, 0xaa, 0xff, 0xaa,
				0xbf, 0xaa, 0xaa, 0xaa, 0xff, 0xaa, 0xbf, 0xaa);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe7,
				0x50, 0x32, 0x10, 0x10, 0x28, 0x81, 0x37, 0x81,
				0x3c, 0x81, 0x04, 0x02, 0x00, 0x00, 0x02, 0x02,
				0x12, 0x05, 0xff, 0xff, 0x37, 0x81, 0x3c, 0x81,
				0x08, 0x00, 0x00, 0x48, 0x17, 0x03, 0x69, 0x00,
				0x00, 0x00, 0x4c, 0x4c, 0x1e, 0x00, 0x40, 0x01,
				0x00, 0x0c, 0x0a, 0x04);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe7,
				0x02, 0x50, 0xd0, 0x01, 0x3c, 0x07, 0x38, 0x10,
				0x00, 0x00, 0x00, 0x02);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x02);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe7,
				0x02, 0x02, 0x02, 0x00, 0x72, 0x01, 0x03, 0x01,
				0x03, 0x01, 0x03, 0x22, 0x20, 0x28, 0x40, 0x01,
				0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe1,
				0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x09, 0x68,
				0x04, 0x38, 0x00, 0x08, 0x02, 0x1c, 0x02, 0x1c,
				0x02, 0x00, 0x02, 0x0e, 0x00, 0x20, 0x00, 0xbb,
				0x00, 0x07, 0x00, 0x0c, 0x0d, 0xb7, 0x0c, 0xb7,
				0x18, 0x00, 0x10, 0xf0, 0x03, 0x0c, 0x20, 0x00,
				0x06, 0x0b, 0x0b, 0x33, 0x0e, 0x1c, 0x2a, 0x38,
				0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b,
				0x7d, 0x7e, 0x01, 0x02, 0x01, 0x00, 0x09);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe1,
				0x40, 0x09, 0xbe, 0x19, 0xfc, 0x19, 0xfa, 0x19,
				0xf8, 0x1a, 0x38, 0x1a, 0x78, 0x1a, 0xb6, 0x2a,
				0xf6, 0x2b, 0x34, 0x2b, 0x74, 0x3b, 0x74, 0x63,
				0xf4);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x03);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe7, 0x01, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe1, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xba, 0x00, 0x00, 0x20, 0xc0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb9, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x53, 0x2c);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 86);

	return dsi_ctx.accum_err;
}

static void hx83112f_power_off(struct hx83112f *ctx)
{
	/* The negative rail must fall before the positive rail. */
	regulator_disable(ctx->supplies[HX83112F_VSN].consumer);
	usleep_range(2000, 3000);
	regulator_disable(ctx->supplies[HX83112F_VSP].consumer);
	usleep_range(2000, 3000);
	regulator_disable(ctx->supplies[HX83112F_VDD].consumer);
}

static int hx83112f_prepare(struct drm_panel *panel)
{
	struct hx83112f *ctx = to_hx83112f(panel);
	int ret;

	/* reset-gpios is active low; one asserts reset. */
	gpiod_set_value_cansleep(ctx->reset, 1);
	usleep_range(1000, 2000);

	ret = regulator_enable(ctx->supplies[HX83112F_VDD].consumer);
	if (ret)
		return ret;
	usleep_range(5000, 6000);
	ret = regulator_enable(ctx->supplies[HX83112F_VSP].consumer);
	if (ret)
		goto disable_vdd;
	usleep_range(2000, 3000);
	ret = regulator_enable(ctx->supplies[HX83112F_VSN].consumer);
	if (ret)
		goto disable_vsp;
	usleep_range(2000, 3000);

	gpiod_set_value_cansleep(ctx->reset, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset, 0);
	usleep_range(10000, 11000);

	ret = hx83112f_init(ctx);
	if (ret) {
		gpiod_set_value_cansleep(ctx->reset, 1);
		hx83112f_power_off(ctx);
	}
	return ret;

disable_vsp:
	regulator_disable(ctx->supplies[HX83112F_VSP].consumer);
	usleep_range(2000, 3000);
disable_vdd:
	regulator_disable(ctx->supplies[HX83112F_VDD].consumer);
	return ret;
}

static int hx83112f_enable(struct drm_panel *panel)
{
	struct hx83112f *ctx = to_hx83112f(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 5);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (!dsi_ctx.accum_err)
		ctx->enabled = true;
	return dsi_ctx.accum_err;
}

static int hx83112f_disable(struct drm_panel *panel)
{
	struct hx83112f *ctx = to_hx83112f(panel);
	int ret;

	ctx->enabled = false;
	ret = mipi_dsi_dcs_set_display_off(ctx->dsi);
	msleep(50);
	if (ret < 0)
		dev_warn(&ctx->dsi->dev, "Display-off command failed: %d\n", ret);
	return 0;
}

static bool keep_reset_in_standby;
module_param(keep_reset_in_standby, bool, 0644);
MODULE_PARM_DESC(keep_reset_in_standby,
		 "Leave reset deasserted after deep standby, as the vendor driver does");

static int hx83112f_unprepare(struct drm_panel *panel)
{
	struct hx83112f *ctx = to_hx83112f(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	/* Still wait if the command failed before removing panel power. */
	msleep(110);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb9, 0x83, 0x11, 0x2f);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb1, 0x01);
	/*
	 * A low RESX releases deep standby while IOVCC stays powered. The
	 * vendor driver leaves reset high here; prepare() asserts it again
	 * before the rails come back.
	 */
	if (!keep_reset_in_standby)
		gpiod_set_value_cansleep(ctx->reset, 1);
	hx83112f_power_off(ctx);

	if (dsi_ctx.accum_err)
		dev_warn(&ctx->dsi->dev, "Sleep command failed: %d\n", dsi_ctx.accum_err);
	return 0;
}

#define HX83112F_MODE(vfp, fps) { \
	.clock = 1296 * (2408 + (vfp) + 15 + 15) * (fps) / 1000, \
	.hdisplay = 1080, \
	.hsync_start = 1080 + 174, \
	.hsync_end = 1080 + 174 + 20, \
	.htotal = 1296, \
	.vdisplay = 2408, \
	.vsync_start = 2408 + (vfp), \
	.vsync_end = 2408 + (vfp) + 15, \
	.vtotal = 2408 + (vfp) + 15 + 15, \
	.width_mm = 68, \
	.height_mm = 152, \
	.type = DRM_MODE_TYPE_DRIVER, \
}

/* DRM derives refresh from clock/totals; vendor .vrefresh is not a DRM field. */
static const struct drm_display_mode hx83112f_modes[] = {
	HX83112F_MODE(1350, 60),
	HX83112F_MODE(78, 90),
};

static int hx83112f_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hx83112f_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, &hx83112f_modes[i]);
		if (!mode)
			return -ENOMEM;
		if (!i)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}
	connector->display_info.width_mm = 68;
	connector->display_info.height_mm = 152;
	return ARRAY_SIZE(hx83112f_modes);
}

static const struct drm_panel_funcs hx83112f_funcs = {
	.prepare = hx83112f_prepare,
	.enable = hx83112f_enable,
	.disable = hx83112f_disable,
	.unprepare = hx83112f_unprepare,
	.get_modes = hx83112f_get_modes,
};

static int hx83112f_bl_update(struct backlight_device *bl)
{
	struct hx83112f *ctx = bl_get_data(bl);
	u8 brightness = backlight_get_brightness(bl);
	int ret;

	if (!ctx->enabled)
		return 0;
	/* This panel uses an eight-bit DCS brightness value. */
	ret = mipi_dsi_dcs_write(ctx->dsi, 0x51, &brightness, sizeof(brightness));
	return ret < 0 ? ret : 0;
}

static const struct backlight_ops hx83112f_bl_ops = {
	.update_status = hx83112f_bl_update,
};

static int hx83112f_probe(struct mipi_dsi_device *dsi)
{
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 255,
		.brightness = 200,
	};
	struct device *dev = &dsi->dev;
	struct hx83112f *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct hx83112f, panel,
				   &hx83112f_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(hx83112f_supplies),
					    hx83112f_supplies, &ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");
	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "Failed to get reset\n");

	dsi->lanes = 3;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS;
	dsi->hs_rate = 1060000000;

	ctx->panel.backlight = devm_backlight_device_register(dev, dev_name(dev), dev,
							    ctx, &hx83112f_bl_ops, &props);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to register backlight\n");

	drm_panel_add(&ctx->panel);
	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret)
		drm_panel_remove(&ctx->panel);
	return ret;
}

static void hx83112f_remove(struct mipi_dsi_device *dsi)
{
	struct hx83112f *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id hx83112f_of_match[] = {
	{ .compatible = "jdi,hx83112f-a22x" },
	{ }
};
MODULE_DEVICE_TABLE(of, hx83112f_of_match);

static struct mipi_dsi_driver hx83112f_driver = {
	.probe = hx83112f_probe,
	.remove = hx83112f_remove,
	.driver = {
		.name = "panel-jdi-hx83112f",
		.of_match_table = hx83112f_of_match,
	},
};
module_mipi_dsi_driver(hx83112f_driver);

MODULE_DESCRIPTION("JDI HX83112F C-PHY panel driver for Galaxy A22 5G");
MODULE_LICENSE("GPL");
