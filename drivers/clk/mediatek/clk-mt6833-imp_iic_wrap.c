// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6833 IMP_IIC_WRAP (I2C wrapper) clock gates.
 * Gate tables from the Samsung SM-A226B (MT6833) kernel source.
 */
#include <dt-bindings/clock/mt6833-clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs imp_iic_wrap_cg_regs = {
	.set_ofs = 0xe08,
	.clr_ofs = 0xe04,
	.sta_ofs = 0xe00,
};

#define GATE_IMP_IIC_WRAP(_id, _name, _parent, _shift)			\
	GATE_MTK_FLAGS(_id, _name, _parent, &imp_iic_wrap_cg_regs, _shift,	\
		&mtk_clk_gate_ops_setclr, CLK_OPS_PARENT_ENABLE)

static const struct mtk_gate imp_iic_wrap_c_clks[] = {
	GATE_IMP_IIC_WRAP(CLK_IMPC_AP_CLOCK_RO_I2C10, "impc_ap_i2c10", "i2c_pseudo", 0),
	GATE_IMP_IIC_WRAP(CLK_IMPC_AP_CLOCK_RO_I2C11, "impc_ap_i2c11", "i2c_pseudo", 1),
};

static const struct mtk_gate imp_iic_wrap_e_clks[] = {
	GATE_IMP_IIC_WRAP(CLK_IMPE_AP_CLOCK_RO_I2C3, "impe_ap_i2c3", "i2c_pseudo", 0),
};

static const struct mtk_gate imp_iic_wrap_n_clks[] = {
	GATE_IMP_IIC_WRAP(CLK_IMPN_AP_CLOCK_RO_I2C6, "impn_ap_i2c6", "i2c_pseudo", 0),
};

static const struct mtk_gate imp_iic_wrap_s_clks[] = {
	GATE_IMP_IIC_WRAP(CLK_IMPS_AP_CLOCK_RO_I2C8, "imps_ap_i2c8", "i2c_pseudo", 0),
	GATE_IMP_IIC_WRAP(CLK_IMPS_AP_CLOCK_RO_I2C9, "imps_ap_i2c9", "i2c_pseudo", 1),
};

static const struct mtk_gate imp_iic_wrap_w_clks[] = {
	GATE_IMP_IIC_WRAP(CLK_IMPW_AP_CLOCK_RO_I2C0, "impw_ap_i2c0", "i2c_pseudo", 0),
	GATE_IMP_IIC_WRAP(CLK_IMPW_AP_CLOCK_RO_I2C5, "impw_ap_i2c5", "i2c_pseudo", 1),
	GATE_IMP_IIC_WRAP(CLK_IMPW_AP_CLOCK_RO_I2C7, "impw_ap_i2c7", "i2c_pseudo", 2),
};

static const struct mtk_gate imp_iic_wrap_ws_clks[] = {
	GATE_IMP_IIC_WRAP(CLK_IMPWS_AP_CLOCK_RO_I2C1, "impws_ap_i2c1", "i2c_pseudo", 0),
	GATE_IMP_IIC_WRAP(CLK_IMPWS_AP_CLOCK_RO_I2C2, "impws_ap_i2c2", "i2c_pseudo", 1),
	GATE_IMP_IIC_WRAP(CLK_IMPWS_AP_CLOCK_RO_I2C4, "impws_ap_i2c4", "i2c_pseudo", 2),
};

static const struct mtk_clk_desc imp_iic_wrap_c_desc = {
	.clks = imp_iic_wrap_c_clks,
	.num_clks = ARRAY_SIZE(imp_iic_wrap_c_clks),
};

static const struct mtk_clk_desc imp_iic_wrap_e_desc = {
	.clks = imp_iic_wrap_e_clks,
	.num_clks = ARRAY_SIZE(imp_iic_wrap_e_clks),
};

static const struct mtk_clk_desc imp_iic_wrap_n_desc = {
	.clks = imp_iic_wrap_n_clks,
	.num_clks = ARRAY_SIZE(imp_iic_wrap_n_clks),
};

static const struct mtk_clk_desc imp_iic_wrap_s_desc = {
	.clks = imp_iic_wrap_s_clks,
	.num_clks = ARRAY_SIZE(imp_iic_wrap_s_clks),
};

static const struct mtk_clk_desc imp_iic_wrap_w_desc = {
	.clks = imp_iic_wrap_w_clks,
	.num_clks = ARRAY_SIZE(imp_iic_wrap_w_clks),
};

static const struct mtk_clk_desc imp_iic_wrap_ws_desc = {
	.clks = imp_iic_wrap_ws_clks,
	.num_clks = ARRAY_SIZE(imp_iic_wrap_ws_clks),
};

static const struct of_device_id of_match_clk_mt6833_imp_iic_wrap[] = {
	{ .compatible = "mediatek,mt6833-imp_iic_wrap_c", .data = &imp_iic_wrap_c_desc },
	{ .compatible = "mediatek,mt6833-imp_iic_wrap_e", .data = &imp_iic_wrap_e_desc },
	{ .compatible = "mediatek,mt6833-imp_iic_wrap_n", .data = &imp_iic_wrap_n_desc },
	{ .compatible = "mediatek,mt6833-imp_iic_wrap_s", .data = &imp_iic_wrap_s_desc },
	{ .compatible = "mediatek,mt6833-imp_iic_wrap_w", .data = &imp_iic_wrap_w_desc },
	{ .compatible = "mediatek,mt6833-imp_iic_wrap_ws", .data = &imp_iic_wrap_ws_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6833_imp_iic_wrap);

static struct platform_driver clk_mt6833_imp_iic_wrap_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6833-imp_iic_wrap",
		.of_match_table = of_match_clk_mt6833_imp_iic_wrap,
	},
};
module_platform_driver(clk_mt6833_imp_iic_wrap_drv);

MODULE_DESCRIPTION("MediaTek MT6833 I2C wrapper clocks driver");
MODULE_LICENSE("GPL");
