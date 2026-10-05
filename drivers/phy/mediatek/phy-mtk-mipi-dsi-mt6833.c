// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6833 C-PHY transmitter. Based on the MT6833 C-PHY entry in Samsung's
 * SM-A226B kernel mtk_mipi_tx.c (Copyright MediaTek Inc.). That entry uses
 * the MT6873 C-PHY PLL sequence, not the MT6833 D-PHY sequence.
 */
#include "phy-mtk-io.h"
#include "phy-mtk-mipi-dsi.h"

#define MIPITX_LANE_CON		0x000c
#define MIPITX_VOLTAGE_SEL	0x0010
#define MIPITX_PRESERVED		0x0014
#define MIPITX_PLL_PWR		0x0028
#define PLL_PWR_ON		BIT(0)
#define PLL_ISO_EN		BIT(1)
#define MIPITX_PLL_CON0		0x002c
#define MIPITX_PLL_CON1		0x0030
#define PLL_EN			BIT(4)
#define PLL_POSDIV		GENMASK(10, 8)
#define MIPITX_PLL_CON4		0x003c
#define MIPITX_PHY_SEL0		0x0040
#define MIPITX_PHY_SEL1		0x0044
#define MIPITX_PHY_SEL2		0x0048
#define MIPITX_PHY_SEL3		0x004c

static int mt6833_cphy_pll_prepare(struct clk_hw *hw)
{
	struct mtk_mipi_tx *tx = mtk_mipi_tx_from_clk_hw(hw);
	void __iomem *base = tx->regs;
	unsigned long ref_rate = clk_hw_get_rate(clk_hw_get_parent(hw));
	u32 posdiv;
	u64 pcw;

	if (!ref_rate || tx->data_rate < 125000000 || tx->data_rate > 2500000000U)
		return -EINVAL;
	if (tx->data_rate >= 2000000000U)
		posdiv = 0;
	else if (tx->data_rate >= 1000000000)
		posdiv = 1;
	else if (tx->data_rate >= 500000000)
		posdiv = 2;
	else if (tx->data_rate > 250000000)
		posdiv = 3;
	else
		posdiv = 4;

	/* C-PHY analogue voltage and bandgap defaults from the stock driver. */
	writel(0x4444236a, base + MIPITX_VOLTAGE_SEL);
	writel(0, base + MIPITX_PRESERVED);
	writel(0x00ff12e0, base + MIPITX_PLL_CON4);
	writel(0x3fff0088, base + MIPITX_LANE_CON);
	writel(0x3fff00c8, base + MIPITX_LANE_CON);
	mtk_phy_clear_bits(base + MIPITX_PLL_CON1, PLL_EN);
	mtk_phy_set_bits(base + MIPITX_PLL_PWR, PLL_PWR_ON);
	usleep_range(30, 100);
	mtk_phy_clear_bits(base + MIPITX_PLL_PWR, PLL_ISO_EN);

	pcw = div_u64((u64)tx->data_rate << (24 + posdiv), ref_rate);
	writel(pcw, base + MIPITX_PLL_CON0);
	mtk_phy_update_field(base + MIPITX_PLL_CON1, PLL_POSDIV, posdiv);
	mtk_phy_set_bits(base + MIPITX_PLL_CON1, PLL_EN);
	usleep_range(50, 100);
	return 0;
}

static void mt6833_cphy_pll_unprepare(struct clk_hw *hw)
{
	struct mtk_mipi_tx *tx = mtk_mipi_tx_from_clk_hw(hw);
	void __iomem *base = tx->regs;

	mtk_phy_clear_bits(base + MIPITX_PLL_CON1, PLL_EN);
	mtk_phy_set_bits(base + MIPITX_PLL_PWR, PLL_ISO_EN);
	mtk_phy_clear_bits(base + MIPITX_PLL_PWR, PLL_PWR_ON);
	writel(0x3fff0080, base + MIPITX_LANE_CON);
	writel(0x3fff0000, base + MIPITX_LANE_CON);
}

static int mt6833_cphy_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	req->rate = clamp_val(req->rate, 125000000, 2500000000UL);
	return 0;
}

static const struct clk_ops mt6833_cphy_clk_ops = {
	.prepare = mt6833_cphy_pll_prepare,
	.unprepare = mt6833_cphy_pll_unprepare,
	.determine_rate = mt6833_cphy_determine_rate,
	.set_rate = mtk_mipi_tx_pll_set_rate,
	.recalc_rate = mtk_mipi_tx_pll_recalc_rate,
};

static void mt6833_cphy_enable_signal(struct phy *phy)
{
	struct mtk_mipi_tx *tx = phy_get_drvdata(phy);

	/* Three trios, default pad mapping with no board lane swap. */
	writel(0x65432101, tx->regs + MIPITX_PHY_SEL0);
	writel(0x24210987, tx->regs + MIPITX_PHY_SEL1);
	writel(0x68543102, tx->regs + MIPITX_PHY_SEL2);
	writel(0x00000007, tx->regs + MIPITX_PHY_SEL3);
}

static void mt6833_cphy_disable_signal(struct phy *phy)
{
	/* The C-PHY PLL unprepare sequence also disables the pad drivers. */
}

const struct mtk_mipitx_data mt6833_cphy_mipitx_data = {
	.mipi_tx_clk_ops = &mt6833_cphy_clk_ops,
	.mipi_tx_enable_signal = mt6833_cphy_enable_signal,
	.mipi_tx_disable_signal = mt6833_cphy_disable_signal,
};
