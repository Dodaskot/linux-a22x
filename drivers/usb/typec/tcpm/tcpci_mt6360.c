// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 MediaTek Inc.
 *
 * Author: ChiYuan Huang <cy_huang@richtek.com>
 */

#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/tcpci.h>
#include <linux/usb/tcpm.h>

#define MT6360_REG_PHYCTRL1	0x80
#define MT6360_REG_PHYCTRL3	0x82
#define MT6360_REG_PHYCTRL7	0x86
#define MT6360_REG_VCONNCTRL1	0x8C
#define MT6360_REG_MODECTRL2	0x8F
#define MT6360_REG_SWRESET	0xA0
#define MT6360_REG_DEBCTRL1	0xA1
#define MT6360_REG_DRPCTRL1	0xA2
#define MT6360_REG_DRPCTRL2	0xA3
#define MT6360_REG_I2CTORST	0xBF
#define MT6360_REG_PHYCTRL11	0xCA
#define MT6360_REG_RXCTRL1	0xCE
#define MT6360_REG_RXCTRL2	0xCF
#define MT6360_REG_CTDCTRL2	0xEC

/* MT6360_REG_VCONNCTRL1 */
#define MT6360_VCONNCL_ENABLE	BIT(0)
/* MT6360_REG_RXCTRL2 */
#define MT6360_OPEN40M_ENABLE	BIT(7)
/* MT6360_REG_CTDCTRL2 */
#define MT6360_RPONESHOT_ENABLE	BIT(6)

#define MT6360_ALERT_MASK	(TCPC_ALERT_TX_SUCCESS | TCPC_ALERT_TX_FAILED | \
				 TCPC_ALERT_TX_DISCARDED | TCPC_ALERT_RX_STATUS | \
				 TCPC_ALERT_RX_HARD_RST | TCPC_ALERT_CC_STATUS | \
				 TCPC_ALERT_POWER_STATUS)

struct mt6360_tcpc_info {
	struct tcpci_data tdata;
	struct tcpci *tcpci;
	struct device *dev;
	int irq;
	struct regulator *vbus;
	bool sourcing_vbus;
	bool irq_ready;
};

static int mt6360_tcpc_set_vbus(struct tcpci *tcpci, struct tcpci_data *tdata,
			      bool source, bool sink)
{
	struct mt6360_tcpc_info *mti = container_of(tdata, struct mt6360_tcpc_info,
						 tdata);
	int ret;

	if (source == mti->sourcing_vbus)
		return 0;

	ret = source ? regulator_enable(mti->vbus) : regulator_disable(mti->vbus);
	if (!ret)
		mti->sourcing_vbus = source;

	/* Let TCPCI also update its source/sink path commands. */
	return ret;
}

static void mt6360_tcpc_disable_vbus(void *data)
{
	struct mt6360_tcpc_info *mti = data;

	if (mti->sourcing_vbus)
		regulator_disable(mti->vbus);
}

static inline int mt6360_tcpc_write16(struct regmap *regmap,
				      unsigned int reg, u16 val)
{
	return regmap_raw_write(regmap, reg, &val, sizeof(u16));
}

static int mt6360_tcpc_init(struct tcpci *tcpci, struct tcpci_data *tdata)
{
	struct mt6360_tcpc_info *mti = container_of(tdata, struct mt6360_tcpc_info,
						 tdata);
	struct regmap *regmap = tdata->regmap;
	int ret;

	ret = regmap_write(regmap, MT6360_REG_SWRESET, 0x01);
	if (ret)
		return ret;

	/* after reset command, wait 1~2ms to wait IC action */
	usleep_range(1000, 2000);

	/* write all alert to masked */
	ret = mt6360_tcpc_write16(regmap, TCPC_ALERT_MASK, 0);
	if (ret)
		return ret;

	/* config I2C timeout reset enable , and timeout to 200ms */
	ret = regmap_write(regmap, MT6360_REG_I2CTORST, 0x8F);
	if (ret)
		return ret;

	/* config CC Detect Debounce : 26.7*val us */
	ret = regmap_write(regmap, MT6360_REG_DEBCTRL1, 0x10);
	if (ret)
		return ret;

	/* DRP Toggle Cycle : 51.2 + 6.4*val ms */
	ret = regmap_write(regmap, MT6360_REG_DRPCTRL1, 4);
	if (ret)
		return ret;

	/* DRP Duyt Ctrl : dcSRC: /1024 */
	ret = mt6360_tcpc_write16(regmap, MT6360_REG_DRPCTRL2, 330);
	if (ret)
		return ret;

	/* Enable VCONN Current Limit function */
	ret = regmap_update_bits(regmap, MT6360_REG_VCONNCTRL1, MT6360_VCONNCL_ENABLE,
				 MT6360_VCONNCL_ENABLE);
	if (ret)
		return ret;

	/* Enable cc open 40ms when pmic send vsysuv signal */
	ret = regmap_update_bits(regmap, MT6360_REG_RXCTRL2, MT6360_OPEN40M_ENABLE,
				 MT6360_OPEN40M_ENABLE);
	if (ret)
		return ret;

	/* Enable Rpdet oneshot detection */
	ret = regmap_update_bits(regmap, MT6360_REG_CTDCTRL2, MT6360_RPONESHOT_ENABLE,
				 MT6360_RPONESHOT_ENABLE);
	if (ret)
		return ret;

	/* BMC PHY */
	ret = mt6360_tcpc_write16(regmap, MT6360_REG_PHYCTRL1, 0x3A70);
	if (ret)
		return ret;

	ret = regmap_write(regmap, MT6360_REG_PHYCTRL3,  0x82);
	if (ret)
		return ret;

	ret = regmap_write(regmap, MT6360_REG_PHYCTRL7, 0x36);
	if (ret)
		return ret;

	ret = mt6360_tcpc_write16(regmap, MT6360_REG_PHYCTRL11, 0x3C60);
	if (ret)
		return ret;

	ret = regmap_write(regmap, MT6360_REG_RXCTRL1, 0xE8);
	if (ret)
		return ret;

	/* Set shipping mode off, AUTOIDLE on */
	ret = regmap_write(regmap, MT6360_REG_MODECTRL2, 0x7A);
	if (ret)
		return ret;

	/* A later TCPM reset must restore interrupts after the software reset. */
	if (mti->irq_ready)
		return mt6360_tcpc_write16(regmap, TCPC_ALERT_MASK, MT6360_ALERT_MASK);

	return 0;
}

static irqreturn_t mt6360_irq(int irq, void *dev_id)
{
	struct mt6360_tcpc_info *mti = dev_id;

	return tcpci_irq(mti->tcpci);
}

static int mt6360_tcpc_probe(struct platform_device *pdev)
{
	struct mt6360_tcpc_info *mti;
	int ret;

	mti = devm_kzalloc(&pdev->dev, sizeof(*mti), GFP_KERNEL);
	if (!mti)
		return -ENOMEM;

	mti->dev = &pdev->dev;

	mti->tdata.regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!mti->tdata.regmap) {
		dev_err(&pdev->dev, "Failed to get parent regmap\n");
		return -ENODEV;
	}

	mti->irq = platform_get_irq_byname(pdev, "PD_IRQB");
	if (mti->irq < 0)
		return mti->irq;

	mti->vbus = devm_regulator_get_optional(mti->dev, "vbus");
	if (IS_ERR(mti->vbus)) {
		if (PTR_ERR(mti->vbus) != -ENODEV)
			return dev_err_probe(mti->dev, PTR_ERR(mti->vbus),
					     "Failed to get VBUS regulator\n");
		mti->vbus = NULL;
	} else {
		ret = devm_add_action_or_reset(mti->dev, mt6360_tcpc_disable_vbus, mti);
		if (ret)
			return ret;
		mti->tdata.set_vbus = mt6360_tcpc_set_vbus;
	}

	mti->tdata.init = mt6360_tcpc_init;
	mti->tcpci = tcpci_register_port(&pdev->dev, &mti->tdata);
	if (IS_ERR(mti->tcpci)) {
		dev_err(&pdev->dev, "Failed to register tcpci port\n");
		return PTR_ERR(mti->tcpci);
	}

	ret = devm_request_threaded_irq(mti->dev, mti->irq, NULL, mt6360_irq, IRQF_ONESHOT,
					dev_name(&pdev->dev), mti);
	if (ret) {
		dev_err(mti->dev, "Failed to register irq\n");
		tcpci_unregister_port(mti->tcpci);
		return ret;
	}

	/* Generic TCPCI leaves enabling alerts to the interrupt-owning driver. */
	mti->irq_ready = true;
	ret = mt6360_tcpc_write16(mti->tdata.regmap, TCPC_ALERT_MASK, MT6360_ALERT_MASK);
	if (ret) {
		disable_irq(mti->irq);
		tcpci_unregister_port(mti->tcpci);
		return dev_err_probe(mti->dev, ret, "Failed to enable alerts\n");
	}

	device_init_wakeup(&pdev->dev, true);
	platform_set_drvdata(pdev, mti);

	return 0;
}

static void mt6360_tcpc_remove(struct platform_device *pdev)
{
	struct mt6360_tcpc_info *mti = platform_get_drvdata(pdev);

	disable_irq(mti->irq);
	tcpci_unregister_port(mti->tcpci);
}

static int __maybe_unused mt6360_tcpc_suspend(struct device *dev)
{
	struct mt6360_tcpc_info *mti = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		enable_irq_wake(mti->irq);

	return 0;
}

static int __maybe_unused mt6360_tcpc_resume(struct device *dev)
{
	struct mt6360_tcpc_info *mti = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		disable_irq_wake(mti->irq);

	return 0;
}

static SIMPLE_DEV_PM_OPS(mt6360_tcpc_pm_ops, mt6360_tcpc_suspend, mt6360_tcpc_resume);

static const struct of_device_id __maybe_unused mt6360_tcpc_of_id[] = {
	{ .compatible = "mediatek,mt6360-tcpc", },
	{},
};
MODULE_DEVICE_TABLE(of, mt6360_tcpc_of_id);

static struct platform_driver mt6360_tcpc_driver = {
	.driver = {
		.name = "mt6360-tcpc",
		.pm = &mt6360_tcpc_pm_ops,
		.of_match_table = mt6360_tcpc_of_id,
	},
	.probe = mt6360_tcpc_probe,
	.remove = mt6360_tcpc_remove,
};
module_platform_driver(mt6360_tcpc_driver);

MODULE_AUTHOR("ChiYuan Huang <cy_huang@richtek.com>");
MODULE_DESCRIPTION("MT6360 USB Type-C Port Controller Interface Driver");
MODULE_LICENSE("GPL v2");
