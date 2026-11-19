// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Galaxy A22 5G (SM-A226B): choose the display path by the panel
 * the bootloader found.
 *
 * The native-display devicetree describes the MediaTek DSI pipeline with the
 * JDI HX83112F panel, the only panel this kernel has a driver for. LK names
 * the fitted panel as a bare command-line token (the stock kernel's
 * tp_match_lcm_name() reads the same token). When LK names another panel,
 * keep the framebuffer LK left lit instead of driving a panel the JDI driver
 * was not written for: before any device is created, re-enable the
 * simple-framebuffer and its backlight, disable the native pipeline and the
 * panel supplies, and let the touch controller run without a panel to
 * follow. That is the devicetree the NT36675 touch driver was qualified
 * with on a Tianma NT36672C panel.
 *
 * A devicetree without the native pipeline, or a command line without any
 * panel token, is left as it is.
 */
#define pr_fmt(fmt) "a22x-display: " fmt

#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/string.h>

#define A22X_PANEL_FAMILY	"_fhdp_wt_dsi_vdo_cphy_90hz_"
#define A22X_PANEL_JDI		"hx83112f_fhdp_wt_dsi_vdo_cphy_90hz_jdi"

/* Test aid: take the fallback path on a JDI phone too. */
static bool force_fallback;
module_param(force_fallback, bool, 0444);

static const char * const a22x_fallback_disable[] = {
	"/display-ml/syscon@14000000",
	"/display-ml/mutex@14001000",
	"/display-ml/rdma@14007000",
	"/display-ml/phy@11e50000",
	"/display-ml/dsi-ml@14013000",
	"/clock-controller@1f000000",
	"/regulator-panel-vdd",
	"/regulator-panel-vsp",
	"/regulator-panel-vsn",
	/*
	 * The native tree adds a DISP power domain that the simple-framebuffer tree
	 * of beta4 (working on a Tianma NT36672C phone) does not have. With every
	 * DISP consumer disabled, Linux must not manage that domain either: the
	 * bootloader keeps the picture running. The domain is the only remaining
	 * user of the display clock controller, so that goes too. The /display-ml
	 * bus itself must stay: disabling it left the power controller without the
	 * domain's clocks, and every power domain (Wi-Fi, GPU) failed to register.
	 */
	"/syscon@10006000/power-controller/power-domain@6",
	"/display-ml/clock-controller-ml@14000000",
};

static const char * const a22x_fallback_enable[] = {
	"/chosen/framebuffer@7c9b0000",
	"/backlight@14013000",
};

static int a22x_status(struct of_changeset *ocs, const char *path,
		       const char *value, bool required)
{
	struct device_node *np = of_find_node_by_path(path);
	int ret;

	if (!np) {
		if (required)
			pr_err("%s is missing\n", path);
		return required ? -ENOENT : 0;
	}
	ret = of_changeset_update_prop_string(ocs, np, "status", value);
	of_node_put(np);
	return ret;
}

static bool a22x_other_panel(void)
{
	const char *bootargs;

	if (force_fallback)
		return true;
	if (!of_chosen || of_property_read_string(of_chosen, "bootargs", &bootargs))
		return false;
	return strstr(bootargs, A22X_PANEL_FAMILY) && !strstr(bootargs, A22X_PANEL_JDI);
}

static int __init a22x_display_select(void)
{
	struct device_node *fb, *dsi, *touch;
	struct property *panel;
	struct of_changeset ocs;
	bool native;
	int i, ret = 0;

	if (!of_machine_is_compatible("samsung,a22x"))
		return 0;

	/* Only the native-display devicetree needs a choice. */
	fb = of_find_node_by_path("/chosen/framebuffer@7c9b0000");
	dsi = of_find_node_by_path("/display-ml/dsi-ml@14013000");
	native = fb && dsi && !of_device_is_available(fb) && of_device_is_available(dsi);
	of_node_put(fb);
	of_node_put(dsi);
	if (!native)
		return 0;

	if (!a22x_other_panel()) {
		pr_info("JDI panel or no panel token: native MediaTek display\n");
		return 0;
	}

	of_changeset_init(&ocs);
	for (i = 0; !ret && i < ARRAY_SIZE(a22x_fallback_disable); i++)
		ret = a22x_status(&ocs, a22x_fallback_disable[i], "disabled", false);
	for (i = 0; !ret && i < ARRAY_SIZE(a22x_fallback_enable); i++)
		ret = a22x_status(&ocs, a22x_fallback_enable[i], "okay", true);

	touch = of_find_node_by_path("/spi1@11010000/novatek@0");
	if (!ret && touch) {
		panel = of_find_property(touch, "panel", NULL);
		if (panel)
			ret = of_changeset_remove_property(&ocs, touch, panel);
	}
	of_node_put(touch);

	if (!ret)
		ret = of_changeset_apply(&ocs);
	if (ret) {
		/* Nothing was changed: the native devicetree stays in force. */
		pr_err("cannot select the bootloader framebuffer: %d\n", ret);
		of_changeset_destroy(&ocs);
		return 0;
	}
	/* The changeset is kept: its properties are part of the live tree now. */
	pr_info("%s: bootloader framebuffer, touch without a panel follower\n",
		force_fallback ? "forced" : "panel is not JDI");
	return 0;
}
/* After the tree is unflattened, before of_platform_default_populate_init(). */
core_initcall(a22x_display_select);
