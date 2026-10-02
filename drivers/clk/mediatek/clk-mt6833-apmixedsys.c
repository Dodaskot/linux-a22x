// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6833 (Dimensity 700) APMIXEDSYS PLLs.
 *
 * The PLL block is register compatible with MT8192; register offsets and
 * per-PLL differences follow the Samsung SM-A226B (MT6833) kernel source.
 * CPU/CCI/memory PLLs are left to firmware, as on MT8192.
 */

#include <dt-bindings/clock/mt6833-clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include "clk-mtk.h"
#include "clk-pll.h"

#define MT6833_PLL_FMAX		(3800UL * MHZ)
#define MT6833_PLL_FMIN		(1500UL * MHZ)
#define MT6833_INTEGER_BITS	8

#define PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags,		\
			_rst_bar_mask, _pcwbits, _pd_reg, _pd_shift,	\
			_tuner_reg, _tuner_en_reg, _tuner_en_bit,	\
			_pcw_reg, _pcw_shift, _pcw_chg_reg,		\
			_en_reg, _pll_en_bit) {				\
		.id = _id,						\
		.name = _name,						\
		.reg = _reg,						\
		.pwr_reg = _pwr_reg,					\
		.en_mask = _en_mask,					\
		.flags = _flags,					\
		.rst_bar_mask = _rst_bar_mask,				\
		.fmax = MT6833_PLL_FMAX,				\
		.fmin = MT6833_PLL_FMIN,				\
		.pcwbits = _pcwbits,					\
		.pcwibits = MT6833_INTEGER_BITS,			\
		.pd_reg = _pd_reg,					\
		.pd_shift = _pd_shift,					\
		.tuner_reg = _tuner_reg,				\
		.tuner_en_reg = _tuner_en_reg,				\
		.tuner_en_bit = _tuner_en_bit,				\
		.pcw_reg = _pcw_reg,					\
		.pcw_shift = _pcw_shift,				\
		.pcw_chg_reg = _pcw_chg_reg,				\
		.en_reg = _en_reg,					\
		.pll_en_bit = _pll_en_bit,				\
	}

#define PLL_B(_id, _name, _reg, _pwr_reg, _en_mask, _flags,		\
			_rst_bar_mask, _pcwbits, _pd_reg, _pd_shift,	\
			_tuner_reg, _tuner_en_reg, _tuner_en_bit,	\
			_pcw_reg, _pcw_shift)				\
		PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags,	\
			_rst_bar_mask, _pcwbits, _pd_reg, _pd_shift,	\
			_tuner_reg, _tuner_en_reg, _tuner_en_bit,	\
			_pcw_reg, _pcw_shift, 0, 0, 0)

static const struct mtk_pll_data plls[] = {
	PLL_B(CLK_APMIXED_MAINPLL, "mainpll", 0x0340, 0x034c, 0xff000000,
	      HAVE_RST_BAR | PLL_AO, BIT(23), 22, 0x0344, 24, 0, 0, 0, 0x0344, 0),
	PLL_B(CLK_APMIXED_UNIVPLL, "univpll", 0x0308, 0x0314, 0xff000000,
	      HAVE_RST_BAR, BIT(23), 22, 0x030c, 24, 0, 0, 0, 0x030c, 0),
	PLL(CLK_APMIXED_USBPLL, "usbpll", 0x03c4, 0x03cc, 0x00000000,
	    0, 0, 22, 0x03c4, 24, 0, 0, 0, 0x03c4, 0, 0x03c4, 0x03cc, 2),
	PLL_B(CLK_APMIXED_MSDCPLL, "msdcpll", 0x0350, 0x035c, 0x00000000,
	      0, 0, 22, 0x0354, 24, 0, 0, 0, 0x0354, 0),
	PLL_B(CLK_APMIXED_MMPLL, "mmpll", 0x0360, 0x036c, 0xff000000,
	      HAVE_RST_BAR, BIT(23), 22, 0x0364, 24, 0, 0, 0, 0x0364, 0),
	PLL_B(CLK_APMIXED_ADSPPLL, "adsppll", 0x0370, 0x037c, 0x00000000,
	      0, 0, 22, 0x0374, 24, 0, 0, 0, 0x0374, 0),
	PLL_B(CLK_APMIXED_MFGPLL, "mfgpll", 0x0268, 0x0274, 0x00000000,
	      0, 0, 22, 0x026c, 24, 0, 0, 0, 0x026c, 0),
	PLL_B(CLK_APMIXED_TVDPLL, "tvdpll", 0x0380, 0x038c, 0x00000000,
	      0, 0, 22, 0x0384, 24, 0, 0, 0, 0x0384, 0),
	PLL_B(CLK_APMIXED_APLL1, "apll1", 0x0318, 0x0328, 0x00000000,
	      0, 0, 32, 0x031c, 24, 0x0040, 0x000c, 0, 0x0320, 0),
	PLL_B(CLK_APMIXED_APLL2, "apll2", 0x032c, 0x033c, 0x00000000,
	      0, 0, 32, 0x0330, 24, 0x0044, 0x000c, 5, 0x0334, 0),
	PLL_B(CLK_APMIXED_NPUPLL, "npupll", 0x03b4, 0x03c0, 0x00000000,
	      0, 0, 22, 0x03b8, 24, 0, 0, 0, 0x03b8, 0),
};

static const struct of_device_id of_match_clk_mt6833_apmixed[] = {
	{ .compatible = "mediatek,mt6833-apmixedsys" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6833_apmixed);

static int clk_mt6833_apmixed_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(CLK_APMIXED_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	r = mtk_clk_register_plls(&pdev->dev, plls, ARRAY_SIZE(plls), clk_data);
	if (r)
		goto free_clk_data;

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		goto unregister_plls;

	platform_set_drvdata(pdev, clk_data);
	return 0;

unregister_plls:
	mtk_clk_unregister_plls(plls, ARRAY_SIZE(plls), clk_data);
free_clk_data:
	mtk_free_clk_data(clk_data);
	return r;
}

static struct platform_driver clk_mt6833_apmixed_drv = {
	.probe = clk_mt6833_apmixed_probe,
	.driver = {
		.name = "clk-mt6833-apmixed",
		.of_match_table = of_match_clk_mt6833_apmixed,
	},
};
module_platform_driver(clk_mt6833_apmixed_drv);

MODULE_DESCRIPTION("MediaTek MT6833 apmixedsys clocks driver");
MODULE_LICENSE("GPL");
