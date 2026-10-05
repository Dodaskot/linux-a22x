// SPDX-License-Identifier: GPL-2.0-only
/* MT6833 display clocks. Register map from MediaTek/Samsung GPL sources. */
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/mt6833-clk.h>

#include "clk-mtk.h"
#include "clk-gate.h"

static const struct mtk_gate_regs mm0_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x108,
	.sta_ofs = 0x100,
};

static const struct mtk_gate_regs mm1_cg_regs = {
	.set_ofs = 0x1a4,
	.clr_ofs = 0x1a8,
	.sta_ofs = 0x1a0,
};

#define GATE_MM0(id, name, shift) \
	GATE_MTK(id, name, "disp_ck", &mm0_cg_regs, shift, &mtk_clk_gate_ops_setclr)
#define GATE_MM1(id, name, shift) \
	GATE_MTK(id, name, "disp_ck", &mm1_cg_regs, shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate mm_clks[] = {
	GATE_MM0(CLK_MM_DISP_MUTEX0, "mm_disp_mutex0", 0),
	GATE_MM0(CLK_MM_APB_BUS, "mm_apb_bus", 1),
	GATE_MM0(CLK_MM_DISP_OVL0, "mm_disp_ovl0", 2),
	GATE_MM0(CLK_MM_DISP_RDMA0, "mm_disp_rdma0", 3),
	GATE_MM0(CLK_MM_DISP_OVL0_2L, "mm_disp_ovl0_2l", 4),
	GATE_MM0(CLK_MM_DISP_WDMA0, "mm_disp_wdma0", 5),
	GATE_MM0(CLK_MM_DISP_CCORR1, "mm_disp_ccorr1", 6),
	GATE_MM0(CLK_MM_DISP_RSZ0, "mm_disp_rsz0", 7),
	GATE_MM0(CLK_MM_DISP_AAL0, "mm_disp_aal0", 8),
	GATE_MM0(CLK_MM_DISP_CCORR0, "mm_disp_ccorr0", 9),
	GATE_MM0(CLK_MM_DISP_COLOR0, "mm_disp_color0", 10),
	GATE_MM0(CLK_MM_SMI_INFRA, "mm_smi_infra", 11),
	GATE_MM0(CLK_MM_DISP_DSC_WRAP, "mm_disp_dsc_wrap", 12),
	GATE_MM0(CLK_MM_DISP_GAMMA0, "mm_disp_gamma0", 13),
	GATE_MM0(CLK_MM_DISP_POSTMASK0, "mm_disp_postmask0", 14),
	GATE_MM0(CLK_MM_DISP_SPR0, "mm_disp_spr0", 15),
	GATE_MM0(CLK_MM_DISP_DITHER0, "mm_disp_dither0", 16),
	GATE_MM0(CLK_MM_SMI_COMMON, "mm_smi_common", 17),
	GATE_MM0(CLK_MM_DISP_CM0, "mm_disp_cm0", 18),
	GATE_MM0(CLK_MM_DSI0, "mm_dsi0", 19),
	GATE_MM0(CLK_MM_DISP_FAKE_ENG0, "mm_disp_fake_eng0", 20),
	GATE_MM0(CLK_MM_DISP_FAKE_ENG1, "mm_disp_fake_eng1", 21),
	GATE_MM0(CLK_MM_SMI_GALS, "mm_smi_gals", 22),
	GATE_MM0(CLK_MM_SMI_IOMMU, "mm_smi_iommu", 24),
	GATE_MM1(CLK_MM_DSI0_DSI_CK_DOMAIN, "mm_dsi0_dsi_domain", 0),
	GATE_MM1(CLK_MM_DISP_26M, "mm_disp_26m_ck", 10),
};

static const struct mtk_clk_desc mm_desc = {
	.clks = mm_clks,
	.num_clks = ARRAY_SIZE(mm_clks),
};

static const struct platform_device_id clk_mt6833_mm_ids[] = {
	{ "clk-mt6833-mm", (kernel_ulong_t)&mm_desc },
	{ }
};
MODULE_DEVICE_TABLE(platform, clk_mt6833_mm_ids);

static const struct of_device_id clk_mt6833_mm_match[] = {
	{ .compatible = "mediatek,mt6833-display-clocks", .data = &mm_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, clk_mt6833_mm_match);

static int clk_mt6833_mm_probe(struct platform_device *pdev)
{
	return pdev->dev.of_node ? mtk_clk_simple_probe(pdev) : mtk_clk_pdev_probe(pdev);
}

static void clk_mt6833_mm_remove(struct platform_device *pdev)
{
	if (pdev->dev.of_node)
		mtk_clk_simple_remove(pdev);
	else
		mtk_clk_pdev_remove(pdev);
}

static struct platform_driver clk_mt6833_mm_driver = {
	.probe = clk_mt6833_mm_probe,
	.remove = clk_mt6833_mm_remove,
	.driver = {
		.name = "clk-mt6833-mm",
		.of_match_table = clk_mt6833_mm_match,
	},
	.id_table = clk_mt6833_mm_ids,
};
module_platform_driver(clk_mt6833_mm_driver);

MODULE_DESCRIPTION("MediaTek MT6833 display clock driver");
MODULE_LICENSE("GPL");
