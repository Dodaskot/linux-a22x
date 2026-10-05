/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_MEDIATEK_MT6833_MMSYS_H
#define __SOC_MEDIATEK_MT6833_MMSYS_H

static const struct mtk_mmsys_routes mt6833_mmsys_routes[] = {
	MMSYS_ROUTE(RDMA0, DSI0, 0x0f0c, GENMASK(1, 0), 0),
	MMSYS_ROUTE(RDMA0, DSI0, 0x0f30, GENMASK(1, 0), 0),
};

#endif
