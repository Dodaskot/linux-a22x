// SPDX-License-Identifier: GPL-2.0
/*
 * Backlight for MediaTek DSI panels left running by the bootloader.
 *
 * Until there is a full mediatek-drm pipeline for the SoC, the display keeps
 * scanning out the bootloader's video-mode setup (shown through
 * simple-framebuffer). The panel brightness is a DCS command (0x51), which
 * the MediaTek DSI controller can inject into the running video stream as a
 * "VM command" during vertical front porch - the same mechanism the vendor
 * kernel and LK use.
 */

#include <linux/backlight.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define DSI_START		0x000
#define VM_CMD_START		BIT(16)
#define DSI_INTSTA		0x00c
#define VM_CMD_DONE		BIT(5)
#define DSI_VM_CMD_CON		0x130
#define VM_CMD_EN		BIT(0)
#define VM_CMD_HSTX		BIT(3)
#define VM_CMD_TS_VFP_EN	BIT(5)

#define MIPI_DCS_SHORT_WRITE_PARAM	0x15
#define DCS_SET_DISPLAY_BRIGHTNESS	0x51
#define DCS_WRITE_CONTROL_DISPLAY	0x53

struct mtk_dsi_vm_bl {
	void __iomem *regs;
	struct mutex lock;
	bool hs;
};

static int mtk_dsi_vm_bl_send(struct mtk_dsi_vm_bl *bl, u8 cmd, u8 param)
{
	u32 val;
	int ret;

	val = (param << 24) | (cmd << 16) | (MIPI_DCS_SHORT_WRITE_PARAM << 8) |
	      VM_CMD_EN | VM_CMD_TS_VFP_EN;
	if (bl->hs)
		val |= VM_CMD_HSTX;

	mutex_lock(&bl->lock);
	writel(val, bl->regs + DSI_VM_CMD_CON);
	writel(readl(bl->regs + DSI_INTSTA) & ~VM_CMD_DONE, bl->regs + DSI_INTSTA);
	writel(readl(bl->regs + DSI_START) & ~VM_CMD_START, bl->regs + DSI_START);
	writel(readl(bl->regs + DSI_START) | VM_CMD_START, bl->regs + DSI_START);
	/* Sent during the next vertical front porch: at most one frame. */
	ret = readl_poll_timeout(bl->regs + DSI_INTSTA, val, val & VM_CMD_DONE,
				 100, 50 * USEC_PER_MSEC);
	mutex_unlock(&bl->lock);

	return ret;
}

static int mtk_dsi_vm_bl_update_status(struct backlight_device *bd)
{
	struct mtk_dsi_vm_bl *bl = bl_get_data(bd);

	return mtk_dsi_vm_bl_send(bl, DCS_SET_DISPLAY_BRIGHTNESS,
				  backlight_get_brightness(bd));
}

static const struct backlight_ops mtk_dsi_vm_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = mtk_dsi_vm_bl_update_status,
};

static int mtk_dsi_vm_bl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
		.max_brightness = 255,
		.brightness = 255,
	};
	struct backlight_device *bd;
	struct mtk_dsi_vm_bl *bl;
	struct resource *res;
	u32 val;

	bl = devm_kzalloc(dev, sizeof(*bl), GFP_KERNEL);
	if (!bl)
		return -ENOMEM;
	mutex_init(&bl->lock);

	/* Shared with the (disabled) DSI node: map without claiming. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	bl->regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!bl->regs)
		return -ENOMEM;

	bl->hs = of_property_read_bool(dev->of_node, "mediatek,hs-transfer");
	of_property_read_u32(dev->of_node, "default-brightness-level",
			     &props.brightness);
	if (!of_property_read_u32(dev->of_node, "max-brightness", &val))
		props.max_brightness = val;

	/*
	 * Some panels (e.g. the SM-A226B Himax TDDI) only drive their LED PWM
	 * output when BCTRL is set in WRCTRLD; otherwise 0x51 just scales the
	 * pixel data and the LEDs stay at full power.
	 */
	if (!of_property_read_u32(dev->of_node, "mediatek,wrctrld", &val)) {
		int ret = mtk_dsi_vm_bl_send(bl, DCS_WRITE_CONTROL_DISPLAY, val);

		if (ret)
			dev_warn(dev, "WRCTRLD write failed: %d\n", ret);
	}

	bd = devm_backlight_device_register(dev, dev_name(dev), dev, bl,
					    &mtk_dsi_vm_bl_ops, &props);
	if (IS_ERR(bd))
		return PTR_ERR(bd);

	platform_set_drvdata(pdev, bd);
	return backlight_update_status(bd);
}

static const struct of_device_id mtk_dsi_vm_bl_of_match[] = {
	{ .compatible = "mediatek,dsi-vm-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_dsi_vm_bl_of_match);

static struct platform_driver mtk_dsi_vm_bl_driver = {
	.driver = {
		.name = "mtk-dsi-vm-backlight",
		.of_match_table = mtk_dsi_vm_bl_of_match,
	},
	.probe = mtk_dsi_vm_bl_probe,
};
module_platform_driver(mtk_dsi_vm_bl_driver);

MODULE_DESCRIPTION("DCS brightness over MediaTek DSI video-mode commands");
MODULE_LICENSE("GPL");
