// SPDX-License-Identifier: GPL-2.0-only
/*
 * Truly panel with the FocalTech FT8722 TDDI used by some Samsung Galaxy A22 5G (SM-A226B) units.
 * Timing and power sequencing from Samsung SM-A226B GPL sources:
 * panel-wt-truly-ft8722-cphy-vdo-90hz.c, Copyright (c) 2015 MediaTek Inc. (as
 * YourLocalTechGuy's panel-wt-truly-ft8722.c for postmarketOS also follows it).
 * The FT8722 keeps its panel initialisation itself: the host only resets it and
 * sends sleep-out and display-on. The stock devicetree drives it from the same
 * lines as the JDI HX83112F and Tianma NT36672C panels of this board (pm-enable
 * GPIO 136, bias GPIOs 137 and 138, reset GPIO 86), here the panel-vdd, -vsp and
 * -vsn fixed regulators and reset-gpios of the native display tree.
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

struct ft8722 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset;
	bool powered;
	bool enabled;
};

/*
 * After it enables the bias rails the vendor driver sets both outputs of the bias IC
 * (I2C, AVDD and AVEE) to 0x0f. This driver has no access to that IC: LK leaves it
 * programmed with the rails on. Keeping the rails on while the panel sleeps, as the
 * vendor driver does when wake gestures are on (fts_gestrue_status), keeps that setting.
 */
static bool keep_power_in_standby = true;
module_param(keep_power_in_standby, bool, 0644);
MODULE_PARM_DESC(keep_power_in_standby,
		 "Keep the panel and bias rails on while the panel sleeps (vendor wake-gesture behaviour)");

enum ft8722_supply { FT8722_VDD, FT8722_VSP, FT8722_VSN };

static const struct regulator_bulk_data ft8722_supplies[] = {
	{ .supply = "vdd" },
	{ .supply = "vsp" },
	{ .supply = "vsn" },
};

static inline struct ft8722 *to_ft8722(struct drm_panel *panel)
{
	return container_of(panel, struct ft8722, panel);
}

/* Sent as the vendor driver sends them: from 0xb0 up as generic writes, the rest as DCS. */
#define ft8722_write_seq(ctx, seq...)						\
	do {									\
		static const u8 d[] = { seq };					\
										\
		if (d[0] < 0xb0)						\
			mipi_dsi_dcs_write_buffer_multi(ctx, d, ARRAY_SIZE(d));	\
		else								\
			mipi_dsi_generic_write_multi(ctx, d, ARRAY_SIZE(d));	\
	} while (0)

static int ft8722_power_on(struct ft8722 *ctx)
{
	int ret;

	if (ctx->powered)
		return 0;
	/* Panel enable, 5 ms, the positive bias, 2 ms, the negative bias, as the vendor driver. */
	ret = regulator_enable(ctx->supplies[FT8722_VDD].consumer);
	if (ret)
		return ret;
	usleep_range(5000, 6000);
	ret = regulator_enable(ctx->supplies[FT8722_VSP].consumer);
	if (ret)
		goto disable_vdd;
	usleep_range(2000, 3000);
	ret = regulator_enable(ctx->supplies[FT8722_VSN].consumer);
	if (ret)
		goto disable_vsp;
	usleep_range(2000, 3000);
	ctx->powered = true;
	return 0;

disable_vsp:
	regulator_disable(ctx->supplies[FT8722_VSP].consumer);
	usleep_range(2000, 3000);
disable_vdd:
	regulator_disable(ctx->supplies[FT8722_VDD].consumer);
	return ret;
}

static void ft8722_power_off(struct ft8722 *ctx)
{
	if (!ctx->powered)
		return;
	/* The negative rail falls before the positive one, then the panel enable. */
	regulator_disable(ctx->supplies[FT8722_VSN].consumer);
	usleep_range(2000, 3000);
	regulator_disable(ctx->supplies[FT8722_VSP].consumer);
	usleep_range(2000, 3000);
	regulator_disable(ctx->supplies[FT8722_VDD].consumer);
	ctx->powered = false;
}

static int ft8722_prepare(struct drm_panel *panel)
{
	struct ft8722 *ctx = to_ft8722(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret;

	/* reset-gpios is active low; one asserts reset (the vendor driver drives it low first). */
	gpiod_set_value_cansleep(ctx->reset, 1);
	usleep_range(1000, 2000);

	ret = ft8722_power_on(ctx);
	if (ret)
		return ret;

	/* Vendor truly_panel_init(): high, low, high with 10 ms between. */
	gpiod_set_value_cansleep(ctx->reset, 0);
	usleep_range(10000, 15000);
	gpiod_set_value_cansleep(ctx->reset, 1);
	usleep_range(10000, 15000);
	gpiod_set_value_cansleep(ctx->reset, 0);
	usleep_range(10000, 15000);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);
	if (dsi_ctx.accum_err) {
		gpiod_set_value_cansleep(ctx->reset, 1);
		if (!keep_power_in_standby)
			ft8722_power_off(ctx);
	}
	return dsi_ctx.accum_err;
}

static int ft8722_enable(struct drm_panel *panel)
{
	struct ft8722 *ctx = to_ft8722(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/* The vendor driver sends display-on at the end of its init, then waits 20 ms. */
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	if (!dsi_ctx.accum_err)
		ctx->enabled = true;
	return dsi_ctx.accum_err;
}

static int ft8722_disable(struct drm_panel *panel)
{
	struct ft8722 *ctx = to_ft8722(panel);
	int ret;

	ctx->enabled = false;
	ret = mipi_dsi_dcs_set_display_off(ctx->dsi);
	msleep(50);
	if (ret < 0)
		dev_warn(&ctx->dsi->dev, "Display-off command failed: %d\n", ret);
	return 0;
}

static int ft8722_unprepare(struct drm_panel *panel)
{
	struct ft8722 *ctx = to_ft8722(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 110);
	if (!keep_power_in_standby) {
		/* Vendor deep standby before the rails go: command page 0x8722, then 0xf7 5a a5 95 27. */
		ft8722_write_seq(&dsi_ctx, 0x00, 0x00);
		ft8722_write_seq(&dsi_ctx, 0xff, 0x87, 0x22, 0x01);
		ft8722_write_seq(&dsi_ctx, 0x00, 0x80);
		ft8722_write_seq(&dsi_ctx, 0xff, 0x87, 0x22);
		ft8722_write_seq(&dsi_ctx, 0x00, 0x00);
		ft8722_write_seq(&dsi_ctx, 0xf7, 0x5a, 0xa5, 0x95, 0x27);
		ft8722_write_seq(&dsi_ctx, 0x00, 0x00);
		ft8722_write_seq(&dsi_ctx, 0xff, 0xff, 0xff, 0xff);
		usleep_range(1000, 2000);
		ft8722_power_off(ctx);
	}
	if (dsi_ctx.accum_err)
		dev_warn(&ctx->dsi->dev, "Sleep command failed: %d\n", dsi_ctx.accum_err);
	return 0;
}

/* Vendor timing: HFP 230, HSA 32, HBP 22; VSA 2, VBP 18; VFP 1282 (60 Hz) or 46 (90 Hz). */
#define FT8722_MODE(vfp, fps) { \
	.clock = 1364 * (2408 + (vfp) + 2 + 18) * (fps) / 1000, \
	.hdisplay = 1080, \
	.hsync_start = 1080 + 230, \
	.hsync_end = 1080 + 230 + 32, \
	.htotal = 1080 + 230 + 32 + 22, \
	.vdisplay = 2408, \
	.vsync_start = 2408 + (vfp), \
	.vsync_end = 2408 + (vfp) + 2, \
	.vtotal = 2408 + (vfp) + 2 + 18, \
	.width_mm = 68, \
	.height_mm = 152, \
	.type = DRM_MODE_TYPE_DRIVER, \
}

static const struct drm_display_mode ft8722_modes[] = {
	FT8722_MODE(1282, 60),
	FT8722_MODE(46, 90),
};

static int ft8722_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ft8722_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, &ft8722_modes[i]);
		if (!mode)
			return -ENOMEM;
		if (!i)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}
	connector->display_info.width_mm = 68;
	connector->display_info.height_mm = 152;
	return ARRAY_SIZE(ft8722_modes);
}

static const struct drm_panel_funcs ft8722_funcs = {
	.prepare = ft8722_prepare,
	.enable = ft8722_enable,
	.disable = ft8722_disable,
	.unprepare = ft8722_unprepare,
	.get_modes = ft8722_get_modes,
};

static int ft8722_bl_update(struct backlight_device *bl)
{
	struct ft8722 *ctx = bl_get_data(bl);
	u8 brightness = backlight_get_brightness(bl);
	int ret;

	if (!ctx->enabled)
		return 0;
	/* The vendor driver sends an eight-bit DCS brightness value (truly_setbacklight_cmdq). */
	ret = mipi_dsi_dcs_write(ctx->dsi, 0x51, &brightness, sizeof(brightness));
	return ret < 0 ? ret : 0;
}

static const struct backlight_ops ft8722_bl_ops = {
	.update_status = ft8722_bl_update,
};

static int ft8722_probe(struct mipi_dsi_device *dsi)
{
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 255,
		.brightness = 200,
	};
	struct device *dev = &dsi->dev;
	struct ft8722 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ft8722, panel, &ft8722_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(ft8722_supplies),
					    ft8722_supplies, &ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");
	/*
	 * Released, as LK leaves it: the panel LK started keeps running, and the touch
	 * part of this TDDI is not held in reset before the first prepare().
	 */
	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "Failed to get reset\n");

	dsi->lanes = 3;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS;
	/* Vendor pll_clk 533 MHz, twice that per trio; the modes need up to 1063.0 Msym/s. */
	dsi->hs_rate = 1066000000;

	ctx->panel.backlight = devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
							    &ft8722_bl_ops, &props);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to register backlight\n");

	drm_panel_add(&ctx->panel);
	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret)
		drm_panel_remove(&ctx->panel);
	return ret;
}

static void ft8722_remove(struct mipi_dsi_device *dsi)
{
	struct ft8722 *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ft8722_of_match[] = {
	{ .compatible = "truly,ft8722-a22x" },
	{ }
};
MODULE_DEVICE_TABLE(of, ft8722_of_match);

static struct mipi_dsi_driver ft8722_driver = {
	.probe = ft8722_probe,
	.remove = ft8722_remove,
	.driver = {
		.name = "panel-truly-ft8722-a22x",
		.of_match_table = ft8722_of_match,
	},
};
module_mipi_dsi_driver(ft8722_driver);

MODULE_DESCRIPTION("Truly FT8722 C-PHY panel driver for Galaxy A22 5G");
MODULE_LICENSE("GPL");
