/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MT6833 (Dimensity 700) power domains: GPU (MFG) and audio.
 *
 * The SPM MFG control/status layout and the MFG1 bus protection masks are
 * the same as MT8192; MT6833 additionally gates the MFG1 AXI "way" in
 * infra_pdn (0x10215000 + 0x4c, bit 11; bit 12 must stay set), as done by
 * the Samsung SM-A226B kernel (clk-mt6833-pg.c). That register is its own
 * status: closing the way has no ack, opening it reads back bit 11.
 */
#ifndef __SOC_MEDIATEK_MT6833_PM_DOMAINS_H
#define __SOC_MEDIATEK_MT6833_PM_DOMAINS_H

#include <linux/soc/mediatek/infracfg.h>
#include <dt-bindings/power/mediatek,mt6833-power.h>
#include "mtk-pm-domains.h"

#define MT6833_INFRA_NAO_MFG1_WAY_EN		0x04c
#define MT6833_INFRA_NAO_MFG1_WAY_EN_MASK	BIT(11)

static enum scpsys_bus_prot_block scpsys_bus_prot_blocks_mt6833[] = {
	BUS_PROT_BLOCK_INFRA, BUS_PROT_BLOCK_INFRA_NAO,
};

static const struct scpsys_domain_data scpsys_domain_data_mt6833[] = {
	[MT6833_POWER_DOMAIN_MD1] = {
		.name = "md1",
		.sta_mask = BIT(0),
		.ctl_offs = 0x0300,
		.pwr_sta_offs = 0x016c,
		/* Vendor MD1 polls only the primary status register. */
		.pwr_sta2nd_offs = 0x016c,
		.ext_buck_iso_offs = 0x0398,
		.ext_buck_iso_mask = BIT(0) | BIT(1),
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, BIT(7), 0x2a0, 0x2a4, 0x228),
			BUS_PROT_WR_IGN(INFRA, BIT(0) | BIT(10) | BIT(16),
					0xb84, 0xb88, 0xb90),
		},
		.caps = MTK_SCPD_MODEM_PWRSEQ | MTK_SCPD_EXT_BUCK_ISO |
			MTK_SCPD_MODEM_OFF_ACK_FIRST | MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT6833_POWER_DOMAIN_MFG0] = {
		.name = "mfg0",
		.sta_mask = BIT(2),
		.ctl_offs = 0x0308,
		.pwr_sta_offs = 0x016c,
		.pwr_sta2nd_offs = 0x0170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.caps = MTK_SCPD_DOMAIN_SUPPLY,
	},
	[MT6833_POWER_DOMAIN_MFG1] = {
		.name = "mfg1",
		.sta_mask = BIT(3),
		.ctl_offs = 0x030c,
		.pwr_sta_offs = 0x016c,
		.pwr_sta2nd_offs = 0x0170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.bp_cfg = {
			/* Closed first on power-down, opened last on power-up. */
			_BUS_PROT_STA(INFRA_NAO, INFRA_NAO,
				      MT6833_INFRA_NAO_MFG1_WAY_EN_MASK,
				      MT6833_INFRA_NAO_MFG1_WAY_EN,
				      MT6833_INFRA_NAO_MFG1_WAY_EN,
				      MT6833_INFRA_NAO_MFG1_WAY_EN_MASK,
				      MT6833_INFRA_NAO_MFG1_WAY_EN,
				      BUS_PROT_INVERTED | BUS_PROT_REG_UPDATE |
				      BUS_PROT_IGNORE_CLR_ACK),
			BUS_PROT_WR(INFRA,
				    MT8192_TOP_AXI_PROT_EN_1_MFG1,
				    MT8192_TOP_AXI_PROT_EN_1_SET,
				    MT8192_TOP_AXI_PROT_EN_1_CLR,
				    MT8192_TOP_AXI_PROT_EN_1_STA1),
			BUS_PROT_WR(INFRA,
				    MT8192_TOP_AXI_PROT_EN_2_MFG1,
				    MT8192_TOP_AXI_PROT_EN_2_SET,
				    MT8192_TOP_AXI_PROT_EN_2_CLR,
				    MT8192_TOP_AXI_PROT_EN_2_STA1),
			BUS_PROT_WR(INFRA,
				    MT8192_TOP_AXI_PROT_EN_MFG1,
				    MT8192_TOP_AXI_PROT_EN_SET,
				    MT8192_TOP_AXI_PROT_EN_CLR,
				    MT8192_TOP_AXI_PROT_EN_STA1),
			BUS_PROT_WR(INFRA,
				    MT8192_TOP_AXI_PROT_EN_2_MFG1_2ND,
				    MT8192_TOP_AXI_PROT_EN_2_SET,
				    MT8192_TOP_AXI_PROT_EN_2_CLR,
				    MT8192_TOP_AXI_PROT_EN_2_STA1),
		},
		.caps = MTK_SCPD_DOMAIN_SUPPLY | MTK_SCPD_HAS_INFRA_NAO,
	},
	[MT6833_POWER_DOMAIN_MFG2] = {
		.name = "mfg2",
		.sta_mask = BIT(4),
		.ctl_offs = 0x0310,
		.pwr_sta_offs = 0x016c,
		.pwr_sta2nd_offs = 0x0170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
	[MT6833_POWER_DOMAIN_MFG3] = {
		.name = "mfg3",
		.sta_mask = BIT(5),
		.ctl_offs = 0x0314,
		.pwr_sta_offs = 0x016c,
		.pwr_sta2nd_offs = 0x0170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
	[MT6833_POWER_DOMAIN_AUDIO] = {
		.name = "audio",
		.sta_mask = BIT(22),
		.ctl_offs = 0x0358,
		.pwr_sta_offs = 0x016c,
		.pwr_sta2nd_offs = 0x0170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.bp_cfg = {
			BUS_PROT_WR(INFRA,
				    MT8192_TOP_AXI_PROT_EN_2_AUDIO,
				    MT8192_TOP_AXI_PROT_EN_2_SET,
				    MT8192_TOP_AXI_PROT_EN_2_CLR,
				    MT8192_TOP_AXI_PROT_EN_2_STA1),
		},
	},
	[MT6833_POWER_DOMAIN_DISP] = {
		.name = "disp",
		/* MM gate registers must only be accessed after bus release. */
		.caps = MTK_SCPD_STRICT_BUS_PROTECTION,
		.sta_mask = BIT(21),
		.ctl_offs = 0x0354,
		.pwr_sta_offs = 0x016c,
		.pwr_sta2nd_offs = 0x0170,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(12),
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA,
				BIT(0) | BIT(2) | BIT(10) | BIT(12) | BIT(16) | BIT(24) | BIT(26),
				0x2d4, 0x2d8, 0x2ec),
			BUS_PROT_WR_IGN(INFRA, BIT(8), 0xdcc, 0xdd0, 0xdd8),
			BUS_PROT_WR_IGN(INFRA, BIT(6) | BIT(23), 0x2a0, 0x2a4, 0x228),
			BUS_PROT_WR_IGN(INFRA,
				BIT(1) | BIT(3) | BIT(17) | BIT(25) | BIT(27),
				0x2d4, 0x2d8, 0x2ec),
			BUS_PROT_WR_IGN(INFRA, BIT(9), 0xdcc, 0xdd0, 0xdd8),
		},
	},
	/* same SPM/bus-protection layout as MT8192 (vendor clk-mt6833-pg.c SYS_CONN) */
	[MT6833_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = BIT(1),
		.ctl_offs = 0x0304,
		.pwr_sta_offs = 0x016c,
		.pwr_sta2nd_offs = 0x0170,
		.sram_pdn_bits = 0,
		.sram_pdn_ack_bits = 0,
		.bp_cfg = {
			BUS_PROT_WR(INFRA,
				    MT8192_TOP_AXI_PROT_EN_CONN,
				    MT8192_TOP_AXI_PROT_EN_SET,
				    MT8192_TOP_AXI_PROT_EN_CLR,
				    MT8192_TOP_AXI_PROT_EN_STA1),
			BUS_PROT_WR(INFRA,
				    MT8192_TOP_AXI_PROT_EN_CONN_2ND,
				    MT8192_TOP_AXI_PROT_EN_SET,
				    MT8192_TOP_AXI_PROT_EN_CLR,
				    MT8192_TOP_AXI_PROT_EN_STA1),
			BUS_PROT_WR(INFRA,
				    MT8192_TOP_AXI_PROT_EN_1_CONN,
				    MT8192_TOP_AXI_PROT_EN_1_SET,
				    MT8192_TOP_AXI_PROT_EN_1_CLR,
				    MT8192_TOP_AXI_PROT_EN_1_STA1),
		},
		.caps = MTK_SCPD_KEEP_DEFAULT_OFF,
	},
};

static const struct scpsys_soc_data mt6833_scpsys_data = {
	.domains_data = scpsys_domain_data_mt6833,
	.num_domains = ARRAY_SIZE(scpsys_domain_data_mt6833),
	.bus_prot_blocks = scpsys_bus_prot_blocks_mt6833,
	.num_bus_prot_blocks = ARRAY_SIZE(scpsys_bus_prot_blocks_mt6833),
};

#endif /* __SOC_MEDIATEK_MT6833_PM_DOMAINS_H */
