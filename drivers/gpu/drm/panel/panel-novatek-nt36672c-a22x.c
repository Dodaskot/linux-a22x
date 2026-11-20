// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tianma NT36672C C-PHY panel used by some Samsung Galaxy A22 5G (SM-A226B) units.
 * Initialization, timing and power sequencing from Samsung SM-A226B GPL sources:
 * panel-wt-tianma-nt36672c-cphy-vdo-90hz.c, Copyright (c) 2015 MediaTek Inc.
 * The supplies, reset line and DSI link are the ones of the JDI HX83112F panel
 * (panel-himax-hx83112f.c) on the same board.
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

struct nt36672c {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset;
	bool powered;
	bool enabled;
};

/*
 * After it enables the bias rails the vendor driver sets both outputs of the bias IC
 * (I2C 0x3e, registers 0 and 1) to 0x0f. This driver has no access to that IC: LK
 * leaves it programmed with the rails on. Keeping the rails on while the panel
 * sleeps, as the vendor driver does when wake gestures are on, keeps that setting.
 */
static bool keep_power_in_standby = true;
module_param(keep_power_in_standby, bool, 0644);
MODULE_PARM_DESC(keep_power_in_standby,
		 "Keep the panel and bias rails on while the panel sleeps (vendor wake-gesture behaviour)");

enum nt36672c_supply { NT36672C_VDD, NT36672C_VSP, NT36672C_VSN };

static const struct regulator_bulk_data nt36672c_supplies[] = {
	{ .supply = "vdd" },
	{ .supply = "vsp" },
	{ .supply = "vsn" },
};

static inline struct nt36672c *to_nt36672c(struct drm_panel *panel)
{
	return container_of(panel, struct nt36672c, panel);
}

/*
 * Sent as the vendor driver sends them: commands from 0xb0 up as generic
 * writes, the others as DCS writes.
 */
#define nt36672c_write_seq(ctx, seq...)						\
	do {									\
		static const u8 d[] = { seq };					\
										\
		if (d[0] < 0xb0)						\
			mipi_dsi_dcs_write_buffer_multi(ctx, d, ARRAY_SIZE(d));	\
		else								\
			mipi_dsi_generic_write_multi(ctx, d, ARRAY_SIZE(d));	\
	} while (0)

static int nt36672c_init(struct nt36672c *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	nt36672c_write_seq(&dsi_ctx, 0xff, 0x25);
	nt36672c_write_seq(&dsi_ctx, 0xfb, 0x01);
	nt36672c_write_seq(&dsi_ctx, 0x18, 0x21);

	nt36672c_write_seq(&dsi_ctx, 0xff, 0x10);
	nt36672c_write_seq(&dsi_ctx, 0xfb, 0x01);
	nt36672c_write_seq(&dsi_ctx, 0x3b, 0x03, 0x12, 0x30, 0x04, 0x04);
	nt36672c_write_seq(&dsi_ctx, 0xb0, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0xc0, 0x00);

	nt36672c_write_seq(&dsi_ctx, 0xff, 0x2c);
	nt36672c_write_seq(&dsi_ctx, 0xfb, 0x01);
	nt36672c_write_seq(&dsi_ctx, 0xa9, 0x03);
	nt36672c_write_seq(&dsi_ctx, 0xaa, 0x03);
	nt36672c_write_seq(&dsi_ctx, 0xab, 0x03);
	nt36672c_write_seq(&dsi_ctx, 0xac, 0x0b);
	nt36672c_write_seq(&dsi_ctx, 0xad, 0x0b);
	nt36672c_write_seq(&dsi_ctx, 0xae, 0x0b);
	nt36672c_write_seq(&dsi_ctx, 0xb6, 0x01);
	nt36672c_write_seq(&dsi_ctx, 0xb7, 0x11);
	nt36672c_write_seq(&dsi_ctx, 0xc0, 0x35);
	nt36672c_write_seq(&dsi_ctx, 0xc1, 0x35);
	nt36672c_write_seq(&dsi_ctx, 0xc2, 0x35);
	nt36672c_write_seq(&dsi_ctx, 0xd5, 0xaf);
	nt36672c_write_seq(&dsi_ctx, 0xd6, 0x10);
	nt36672c_write_seq(&dsi_ctx, 0xd7, 0xeb);
	nt36672c_write_seq(&dsi_ctx, 0xd9, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0xda, 0xeb);
	nt36672c_write_seq(&dsi_ctx, 0xdc, 0x10);
	nt36672c_write_seq(&dsi_ctx, 0xde, 0x10);
	nt36672c_write_seq(&dsi_ctx, 0xf2, 0x0b);
	nt36672c_write_seq(&dsi_ctx, 0xf3, 0x03);
	nt36672c_write_seq(&dsi_ctx, 0xf4, 0x06);

	nt36672c_write_seq(&dsi_ctx, 0xff, 0xe0);
	nt36672c_write_seq(&dsi_ctx, 0xfb, 0x01);
	nt36672c_write_seq(&dsi_ctx, 0x35, 0x82);
	nt36672c_write_seq(&dsi_ctx, 0x85, 0x30);

	nt36672c_write_seq(&dsi_ctx, 0xff, 0xf0);
	nt36672c_write_seq(&dsi_ctx, 0xfb, 0x01);
	nt36672c_write_seq(&dsi_ctx, 0x5a, 0x00);

	nt36672c_write_seq(&dsi_ctx, 0xff, 0xc0);
	nt36672c_write_seq(&dsi_ctx, 0xfb, 0x01);
	nt36672c_write_seq(&dsi_ctx, 0x9c, 0x11);
	nt36672c_write_seq(&dsi_ctx, 0x9d, 0x11);

	nt36672c_write_seq(&dsi_ctx, 0xff, 0xd0);
	nt36672c_write_seq(&dsi_ctx, 0xfb, 0x01);
	nt36672c_write_seq(&dsi_ctx, 0x53, 0x22);
	nt36672c_write_seq(&dsi_ctx, 0x54, 0x02);
	nt36672c_write_seq(&dsi_ctx, 0x1c, 0x88);
	nt36672c_write_seq(&dsi_ctx, 0x1d, 0x08);

	nt36672c_write_seq(&dsi_ctx, 0xff, 0x20);
	nt36672c_write_seq(&dsi_ctx, 0xfb, 0x01);
	nt36672c_write_seq(&dsi_ctx, 0x74, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x75, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x76, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x77, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x78, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x79, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x7a, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x7b, 0x20);
	nt36672c_write_seq(&dsi_ctx, 0x7c, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x7d, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x7e, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x7f, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x80, 0x00);

	nt36672c_write_seq(&dsi_ctx, 0xff, 0x2a);
	nt36672c_write_seq(&dsi_ctx, 0xfb, 0x01);
	nt36672c_write_seq(&dsi_ctx, 0x1c, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x1d, 0x30);
	nt36672c_write_seq(&dsi_ctx, 0x56, 0x30);
	nt36672c_write_seq(&dsi_ctx, 0x8b, 0x30);

	nt36672c_write_seq(&dsi_ctx, 0xff, 0x23);
	nt36672c_write_seq(&dsi_ctx, 0xfb, 0x01);
	nt36672c_write_seq(&dsi_ctx, 0x00, 0x00);
	nt36672c_write_seq(&dsi_ctx, 0x07, 0x20);
	nt36672c_write_seq(&dsi_ctx, 0x08, 0x03);
	nt36672c_write_seq(&dsi_ctx, 0x09, 0x2e);

	nt36672c_write_seq(&dsi_ctx, 0xff, 0x10);
	nt36672c_write_seq(&dsi_ctx, 0xfb, 0x01);
	nt36672c_write_seq(&dsi_ctx, 0x53, 0x2c);
	nt36672c_write_seq(&dsi_ctx, 0x68, 0x02, 0x01);
	/* TE on, VBLANK only, as the vendor driver sets it. */
	nt36672c_write_seq(&dsi_ctx, 0x35, 0x00);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 105);

	return dsi_ctx.accum_err;
}

static int nt36672c_power_on(struct nt36672c *ctx)
{
	int ret;

	if (ctx->powered)
		return 0;
	/* Panel enable, then the positive and the negative bias, as the vendor driver does. */
	ret = regulator_enable(ctx->supplies[NT36672C_VDD].consumer);
	if (ret)
		return ret;
	usleep_range(2000, 3000);
	ret = regulator_enable(ctx->supplies[NT36672C_VSP].consumer);
	if (ret)
		goto disable_vdd;
	usleep_range(1000, 2000);
	ret = regulator_enable(ctx->supplies[NT36672C_VSN].consumer);
	if (ret)
		goto disable_vsp;
	ctx->powered = true;
	return 0;

disable_vsp:
	regulator_disable(ctx->supplies[NT36672C_VSP].consumer);
	usleep_range(2000, 3000);
disable_vdd:
	regulator_disable(ctx->supplies[NT36672C_VDD].consumer);
	return ret;
}

static void nt36672c_power_off(struct nt36672c *ctx)
{
	if (!ctx->powered)
		return;
	/* The negative rail falls before the positive one, then the panel enable. */
	regulator_disable(ctx->supplies[NT36672C_VSN].consumer);
	usleep_range(2000, 3000);
	regulator_disable(ctx->supplies[NT36672C_VSP].consumer);
	usleep_range(2000, 3000);
	regulator_disable(ctx->supplies[NT36672C_VDD].consumer);
	ctx->powered = false;
}

static int nt36672c_prepare(struct drm_panel *panel)
{
	struct nt36672c *ctx = to_nt36672c(panel);
	int ret;

	/* reset-gpios is active low; one asserts reset. */
	gpiod_set_value_cansleep(ctx->reset, 1);
	usleep_range(1000, 2000);

	ret = nt36672c_power_on(ctx);
	if (ret)
		return ret;
	msleep(10);

	gpiod_set_value_cansleep(ctx->reset, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset, 0);
	usleep_range(10000, 11000);

	ret = nt36672c_init(ctx);
	if (ret) {
		gpiod_set_value_cansleep(ctx->reset, 1);
		/* As in standby: the rails stay on unless they are allowed to go. */
		if (!keep_power_in_standby)
			nt36672c_power_off(ctx);
	}
	return ret;
}

static int nt36672c_enable(struct drm_panel *panel)
{
	struct nt36672c *ctx = to_nt36672c(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 5);
	if (!dsi_ctx.accum_err)
		ctx->enabled = true;
	return dsi_ctx.accum_err;
}

static int nt36672c_disable(struct drm_panel *panel)
{
	struct nt36672c *ctx = to_nt36672c(panel);
	int ret;

	ctx->enabled = false;
	ret = mipi_dsi_dcs_set_display_off(ctx->dsi);
	msleep(20);
	if (ret < 0)
		dev_warn(&ctx->dsi->dev, "Display-off command failed: %d\n", ret);
	return 0;
}

static int nt36672c_unprepare(struct drm_panel *panel)
{
	struct nt36672c *ctx = to_nt36672c(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	/* The vendor driver waits 200 ms before it removes the bias. */
	msleep(200);
	/* Reset stays released as in the vendor driver; prepare() asserts it again. */
	if (!keep_power_in_standby)
		nt36672c_power_off(ctx);

	if (dsi_ctx.accum_err)
		dev_warn(&ctx->dsi->dev, "Sleep command failed: %d\n", dsi_ctx.accum_err);
	return 0;
}

/* Vendor timing: HFP 250, HSA 20, HBP 40; VSA 2, VBP 16; VFP 1285 (60 Hz) or 48 (90 Hz). */
#define NT36672C_MODE(vfp, fps) { \
	.clock = 1390 * (2408 + (vfp) + 2 + 16) * (fps) / 1000, \
	.hdisplay = 1080, \
	.hsync_start = 1080 + 250, \
	.hsync_end = 1080 + 250 + 20, \
	.htotal = 1080 + 250 + 20 + 40, \
	.vdisplay = 2408, \
	.vsync_start = 2408 + (vfp), \
	.vsync_end = 2408 + (vfp) + 2, \
	.vtotal = 2408 + (vfp) + 2 + 16, \
	.width_mm = 68, \
	.height_mm = 152, \
	.type = DRM_MODE_TYPE_DRIVER, \
}

static const struct drm_display_mode nt36672c_modes[] = {
	NT36672C_MODE(1285, 60),
	NT36672C_MODE(48, 90),
};

static int nt36672c_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(nt36672c_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, &nt36672c_modes[i]);
		if (!mode)
			return -ENOMEM;
		if (!i)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}
	connector->display_info.width_mm = 68;
	connector->display_info.height_mm = 152;
	return ARRAY_SIZE(nt36672c_modes);
}

static const struct drm_panel_funcs nt36672c_funcs = {
	.prepare = nt36672c_prepare,
	.enable = nt36672c_enable,
	.disable = nt36672c_disable,
	.unprepare = nt36672c_unprepare,
	.get_modes = nt36672c_get_modes,
};

static int nt36672c_bl_update(struct backlight_device *bl)
{
	struct nt36672c *ctx = bl_get_data(bl);
	u8 brightness = backlight_get_brightness(bl);
	int ret;

	if (!ctx->enabled)
		return 0;
	/* The vendor driver sends an eight-bit DCS brightness value. */
	ret = mipi_dsi_dcs_write(ctx->dsi, 0x51, &brightness, sizeof(brightness));
	return ret < 0 ? ret : 0;
}

static const struct backlight_ops nt36672c_bl_ops = {
	.update_status = nt36672c_bl_update,
};

static int nt36672c_probe(struct mipi_dsi_device *dsi)
{
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 255,
		.brightness = 200,
	};
	struct device *dev = &dsi->dev;
	struct nt36672c *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct nt36672c, panel,
				   &nt36672c_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(nt36672c_supplies),
					    nt36672c_supplies, &ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");
	/*
	 * Released (logical 0: RESX high), as the vendor driver leaves it: the panel LK
	 * started keeps running, and the touch part of this chip, which RESX resets too,
	 * can be probed before the first prepare() (2026-11-20 review: requested asserted,
	 * the touch probe that reads the chip found it in reset and gave up). prepare()
	 * asserts reset before the power sequence.
	 */
	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "Failed to get reset\n");

	dsi->lanes = 3;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS;
	/*
	 * Vendor pll_clk 544 MHz, twice that per trio (JDI: 530, run at 1060). Its 1076
	 * is the alternative rate for MIPI hopping. The modes need 1083.2 Msym/s.
	 */
	dsi->hs_rate = 1088000000;

	ctx->panel.backlight = devm_backlight_device_register(dev, dev_name(dev), dev,
							    ctx, &nt36672c_bl_ops, &props);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to register backlight\n");

	drm_panel_add(&ctx->panel);
	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret)
		drm_panel_remove(&ctx->panel);
	return ret;
}

static void nt36672c_remove(struct mipi_dsi_device *dsi)
{
	struct nt36672c *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id nt36672c_of_match[] = {
	{ .compatible = "tianma,nt36672c-a22x" },
	{ }
};
MODULE_DEVICE_TABLE(of, nt36672c_of_match);

static struct mipi_dsi_driver nt36672c_driver = {
	.probe = nt36672c_probe,
	.remove = nt36672c_remove,
	.driver = {
		.name = "panel-tianma-nt36672c-a22x",
		.of_match_table = nt36672c_of_match,
	},
};
module_mipi_dsi_driver(nt36672c_driver);

MODULE_DESCRIPTION("Tianma NT36672C C-PHY panel driver for Galaxy A22 5G");
MODULE_LICENSE("GPL");
