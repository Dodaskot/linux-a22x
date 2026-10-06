// SPDX-License-Identifier: GPL-2.0-only
/* MT6833 MDP clocks, from Samsung's GPL clock tables. */
#include <dt-bindings/clock/mt6833-clk.h>
#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include "clk-gate.h"
#include "clk-mtk.h"

struct mt6833_mdp_clock_data {
	struct clk *bus;
	struct regmap *spm;
	struct regmap *gates;
	struct clk_hw_onecell_data *clocks;
	bool bus_held;
};

static const struct mtk_gate_regs mdp0_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x108,
	.sta_ofs = 0x100,
};
static const struct mtk_gate_regs mdp1_cg_regs = {
	.set_ofs = 0x124,
	.clr_ofs = 0x128,
	.sta_ofs = 0x120,
};
#define GATE_MDP0(id, name, shift) \
	GATE_MTK(id, name, "mdp_ck", &mdp0_cg_regs, shift, &mtk_clk_gate_ops_setclr)
#define GATE_MDP1(id, name, shift) \
	GATE_MTK(id, name, "mdp_ck", &mdp1_cg_regs, shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate mdp_clks[] = {
	GATE_MDP0(CLK_MDP_RDMA0, "mdp_rdma0", 0),
	GATE_MDP0(CLK_MDP_TDSHP0, "mdp_tdshp0", 1),
	GATE_MDP0(CLK_MDP_IMG_DL_ASYNC0, "mdp_img_dl_async0", 2),
	GATE_MDP0(CLK_MDP_IMG_DL_ASYNC1, "mdp_img_dl_async1", 3),
	GATE_MDP0(CLK_MDP_RDMA1, "mdp_rdma1", 4),
	GATE_MDP0(CLK_MDP_TDSHP1, "mdp_tdshp1", 5),
	GATE_MDP0(CLK_MDP_SMI0, "mdp_smi0", 6),
	GATE_MDP0(CLK_MDP_APB_BUS, "mdp_apb_bus", 7),
	GATE_MDP0(CLK_MDP_WROT0, "mdp_wrot0", 8),
	GATE_MDP0(CLK_MDP_RSZ0, "mdp_rsz0", 9),
	GATE_MDP0(CLK_MDP_HDR0, "mdp_hdr0", 10),
	GATE_MDP0(CLK_MDP_MUTEX0, "mdp_mutex0", 11),
	GATE_MDP0(CLK_MDP_WROT1, "mdp_wrot1", 12),
	GATE_MDP0(CLK_MDP_RSZ1, "mdp_rsz1", 13),
	GATE_MDP0(CLK_MDP_FAKE_ENG0, "mdp_fake_eng0", 14),
	GATE_MDP0(CLK_MDP_AAL0, "mdp_aal0", 15),
	GATE_MDP0(CLK_MDP_AAL1, "mdp_aal1", 16),
	GATE_MDP0(CLK_MDP_COLOR0, "mdp_color0", 17),
	GATE_MDP1(CLK_MDP_IMG_DL_RELAY0_ASYNC0, "mdp_img_dl_rel0_as0", 0),
	GATE_MDP1(CLK_MDP_IMG_DL_RELAY1_ASYNC1, "mdp_img_dl_rel1_as1", 8),
};

static int mt6833_mdp_runtime_resume(struct device *dev)
{
	struct mt6833_mdp_clock_data *data = dev_get_drvdata(dev);
	unsigned int status, status2;
	int ret;

	ret = regmap_read(data->spm, 0x16c, &status);
	if (ret)
		return ret;
	ret = regmap_read(data->spm, 0x170, &status2);
	if (ret)
		return ret;
	/* The boot-owned MDP island has no verified power-switch sequence yet.
	 * Refuse register access when it is off; do not invent a power-on path.
	 */
	if (!(status & status2 & BIT(20)))
		return -EHOSTDOWN;
	if (data->bus_held)
		return 0;
	ret = clk_prepare_enable(data->bus);
	if (!ret)
		data->bus_held = true;
	return ret;
}

static int mt6833_mdp_runtime_suspend(struct device *dev)
{
	struct mt6833_mdp_clock_data *data = dev_get_drvdata(dev);
	unsigned int gates;
	int ret;

	/* Keep LK's path intact until the unused DMA/fake engines are gated.
	 * In particular, registering the provider must not cut the boot image.
	 */
	ret = regmap_read(data->gates, 0x100, &gates);
	if (ret)
		return ret;
	if ((gates & 0x5151) != 0x5151)
		return 0;
	if (data->bus_held) {
		clk_disable_unprepare(data->bus);
		data->bus_held = false;
	}
	return 0;
}

static const struct dev_pm_ops mt6833_mdp_pm_ops = {
	RUNTIME_PM_OPS(mt6833_mdp_runtime_suspend, mt6833_mdp_runtime_resume, NULL)
	NOIRQ_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static int mt6833_mdp_clock_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6833_mdp_clock_data *data;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->spm = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(data->spm))
		return dev_err_probe(dev, PTR_ERR(data->spm), "missing SPM status map\n");
	data->bus = devm_clk_get(dev, "bus");
	if (IS_ERR(data->bus))
		return dev_err_probe(dev, PTR_ERR(data->bus), "missing MDP bus clock\n");
	data->gates = device_node_to_regmap(dev->of_node);
	if (IS_ERR(data->gates))
		return PTR_ERR(data->gates);
	data->clocks = mtk_devm_alloc_clk_data(dev, CLK_MDP_NR_CLK);
	if (!data->clocks)
		return -ENOMEM;
	platform_set_drvdata(pdev, data);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "MDP register path unavailable\n");
	ret = mtk_clk_register_gates(dev, dev->of_node, mdp_clks,
				     ARRAY_SIZE(mdp_clks), data->clocks);
	if (ret)
		goto put_runtime;
	ret = of_clk_add_hw_provider(dev->of_node, of_clk_hw_onecell_get, data->clocks);
	if (ret)
		mtk_clk_unregister_gates(mdp_clks, ARRAY_SIZE(mdp_clks), data->clocks);
put_runtime:
	pm_runtime_put_sync(dev);
	if (ret && data->bus_held) {
		clk_disable_unprepare(data->bus);
		data->bus_held = false;
	}
	return ret;
}

static void mt6833_mdp_clock_remove(struct platform_device *pdev)
{
	struct mt6833_mdp_clock_data *data = platform_get_drvdata(pdev);
	int ret;

	ret = pm_runtime_resume_and_get(&pdev->dev);
	of_clk_del_provider(pdev->dev.of_node);
	mtk_clk_unregister_gates(mdp_clks, ARRAY_SIZE(mdp_clks), data->clocks);
	if (ret >= 0)
		pm_runtime_put_sync(&pdev->dev);
	if (data->bus_held)
		clk_disable_unprepare(data->bus);
}

static const struct of_device_id mt6833_mdp_clock_match[] = {
	{ .compatible = "mediatek,mt6833-mdp-clocks" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6833_mdp_clock_match);

static struct platform_driver mt6833_mdp_clock_driver = {
	.probe = mt6833_mdp_clock_probe,
	.remove = mt6833_mdp_clock_remove,
	.driver = {
		.name = "clk-mt6833-mdp",
		.of_match_table = mt6833_mdp_clock_match,
		.pm = pm_ptr(&mt6833_mdp_pm_ops),
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6833_mdp_clock_driver);

MODULE_DESCRIPTION("MediaTek MT6833 MDP clocks with owned runtime register access");
MODULE_LICENSE("GPL");
