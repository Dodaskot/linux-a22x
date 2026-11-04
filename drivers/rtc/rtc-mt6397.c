// SPDX-License-Identifier: GPL-2.0-only
/*
* Copyright (c) 2014-2015 MediaTek Inc.
* Author: Tianping.Fang <tianping.fang@mediatek.com>
*/

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/mfd/mt6359/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/mfd/mt6397/rtc.h>

static int mtk_rtc_write_trigger(struct mt6397_rtc *rtc)
{
	int ret;
	u32 data;

	ret = regmap_write(rtc->regmap, rtc->addr_base + rtc->data->wrtgr, 1);
	if (ret < 0)
		return ret;

	ret = regmap_read_poll_timeout(rtc->regmap,
					rtc->addr_base + RTC_BBPU, data,
					!(data & RTC_BBPU_CBUSY),
					MTK_RTC_POLL_DELAY_US,
					MTK_RTC_POLL_TIMEOUT);
	if (ret < 0)
		dev_err(rtc->rtc_dev->dev.parent,
			"failed to write WRTGR: %d\n", ret);

	return ret;
}

/* MT6359(P) power-off sequence from the A22x vendor PMIC/RTC driver. */
#define MT6359_RTC_BBPU_PWREN	BIT(0)
#define MT6359_RTC_BBPU_CLR	BIT(1)
#define MT6359_RTC_BBPU_RELOAD	BIT(5)
#define MT6359_RTC_PDN1		0x2c
#define MT6359_RTC_GPIO_USERS	GENMASK(12, 8)
#define MT6359_RTC_CON		0x3c
#define MT6359_RTC_CON_F32KOB	BIT(5)
#define MT6359_PWRKEY_RELEASED	BIT(1)
#define MT6359_PWRHOLD		BIT(0)
#define MT6359_SDN_DLY_ENB	BIT(8)
#define MT6359_TMA_KEY		0x9ca7

static int mt6359_rtc_power_off(struct sys_off_data *data)
{
	struct mt6397_rtc *rtc = data->cb_data;
	const char *stage = "power-key release";
	unsigned int val;
	int ret, lock_ret, poll_ret;

	/*
	 * PWRAP's regmap uses a mutex and sleepable polling. Run after device
	 * shutdown, while interrupts and scheduling are still available.
	 */
	dev_info(data->dev, "MT6359 RTC/PMIC power-off sequence\n");
	mutex_lock(&rtc->lock);

	/* An asserted power key can immediately turn the PMIC on again. */
	ret = regmap_read_poll_timeout(rtc->regmap, MT6359_TOPSTATUS, val,
				      val & MT6359_PWRKEY_RELEASED,
				      10000, 1000000);
	if (ret)
		goto out_unlock;

	stage = "32 kHz output";
	ret = regmap_read(rtc->regmap, rtc->addr_base + MT6359_RTC_PDN1, &val);
	if (ret)
		goto out_unlock;
	if (!(val & MT6359_RTC_GPIO_USERS)) {
		ret = regmap_update_bits(rtc->regmap,
					 rtc->addr_base + MT6359_RTC_CON,
					 MT6359_RTC_CON_F32KOB,
					 MT6359_RTC_CON_F32KOB);
		if (ret)
			goto out_unlock;
		ret = mtk_rtc_write_trigger(rtc);
		if (ret)
			goto out_unlock;
	}

	stage = "RTC low-power shutdown clear";
	ret = regmap_write(rtc->regmap, rtc->addr_base + RTC_BBPU,
			   RTC_BBPU_KEY | MT6359_RTC_BBPU_CLR |
			   MT6359_RTC_BBPU_PWREN);
	if (ret)
		goto out_unlock;
	ret = regmap_write(rtc->regmap, rtc->addr_base + RTC_AL_MASK,
			   RTC_AL_MASK_DOW);
	if (ret)
		goto out_unlock;
	ret = mtk_rtc_write_trigger(rtc);
	if (ret)
		goto out_unlock;
	/*
	 * The vendor sequence continues if CLR remains set after 500 ms.
	 * Keep the read error separate: PWRAP can also return -ETIMEDOUT.
	 */
	poll_ret = read_poll_timeout(regmap_read, ret,
				     ret || !(val & MT6359_RTC_BBPU_CLR),
				     MTK_RTC_POLL_DELAY_US, 500000, false,
				     rtc->regmap, rtc->addr_base + RTC_BBPU,
				     &val);
	if (ret)
		goto out_unlock;
	if (poll_ret)
		dev_warn(data->dev,
			 "RTC_BBPU_CLR timeout (BBPU=%#x); continuing power-off\n",
			 val);

	stage = "RTC reload";
	ret = regmap_update_bits(rtc->regmap, rtc->addr_base + RTC_BBPU,
				 RTC_BBPU_KEY | MT6359_RTC_BBPU_RELOAD,
				 RTC_BBPU_KEY | MT6359_RTC_BBPU_RELOAD);
	if (ret)
		goto out_unlock;
	ret = mtk_rtc_write_trigger(rtc);
	if (ret)
		goto out_unlock;

	stage = "shutdown delay unlock";
	ret = regmap_write(rtc->regmap, MT6359_TOP_TMA_KEY, MT6359_TMA_KEY);
	if (!ret) {
		stage = "shutdown delay";
		ret = regmap_update_bits(rtc->regmap, MT6359_PSEQ_ELR1,
					 MT6359_SDN_DLY_ENB, 0);
	}
	/* Always relock, even when the unlock or protected update failed. */
	lock_ret = regmap_write(rtc->regmap, MT6359_TOP_TMA_KEY, 0);
	if (lock_ret) {
		dev_err(data->dev, "failed to relock MT6359 TMA key: %d\n",
			lock_ret);
		if (!ret) {
			stage = "shutdown delay relock";
			ret = lock_ret;
		}
	}
	if (ret)
		goto out_unlock;

	stage = "PWRHOLD release";
	dev_info(data->dev, "releasing MT6359 PWRHOLD\n");
	ret = regmap_update_bits(rtc->regmap, MT6359_PPCCTL0,
				 MT6359_PWRHOLD, 0);

out_unlock:
	mutex_unlock(&rtc->lock);
	if (ret) {
		dev_err(data->dev, "MT6359 power-off failed at %s: %d; using fallback\n",
			stage, ret);
		return NOTIFY_DONE;
	}

	/* Do not enter PSCI before the PMIC has had time to cut the rails. */
	msleep(1000);
	dev_err(data->dev, "MT6359 power-off did not complete; using fallback\n");
	return NOTIFY_DONE;
}

static irqreturn_t mtk_rtc_irq_handler_thread(int irq, void *data)
{
	struct mt6397_rtc *rtc = data;
	u32 irqsta, irqen;
	int ret;

	ret = regmap_read(rtc->regmap, rtc->addr_base + RTC_IRQ_STA, &irqsta);
	if ((ret >= 0) && (irqsta & RTC_IRQ_STA_AL)) {
		rtc_update_irq(rtc->rtc_dev, 1, RTC_IRQF | RTC_AF);
		irqen = irqsta & ~RTC_IRQ_EN_AL;
		mutex_lock(&rtc->lock);
		if (regmap_write(rtc->regmap, rtc->addr_base + RTC_IRQ_EN,
				 irqen) == 0)
			mtk_rtc_write_trigger(rtc);
		mutex_unlock(&rtc->lock);

		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int __mtk_rtc_read_time(struct mt6397_rtc *rtc,
			       struct rtc_time *tm, int *sec)
{
	int ret;
	u16 data[RTC_OFFSET_COUNT];

	mutex_lock(&rtc->lock);
	ret = regmap_bulk_read(rtc->regmap, rtc->addr_base + RTC_TC_SEC,
			       data, RTC_OFFSET_COUNT);
	if (ret < 0)
		goto exit;

	tm->tm_sec = data[RTC_OFFSET_SEC];
	tm->tm_min = data[RTC_OFFSET_MIN];
	tm->tm_hour = data[RTC_OFFSET_HOUR];
	tm->tm_mday = data[RTC_OFFSET_DOM];
	tm->tm_wday = data[RTC_OFFSET_DOW];
	tm->tm_mon = data[RTC_OFFSET_MTH] & RTC_TC_MTH_MASK;
	tm->tm_year = data[RTC_OFFSET_YEAR];

	ret = regmap_read(rtc->regmap, rtc->addr_base + RTC_TC_SEC, sec);
exit:
	mutex_unlock(&rtc->lock);
	return ret;
}

static int mtk_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);
	int sec, ret;

	do {
		ret = __mtk_rtc_read_time(rtc, tm, &sec);
		if (ret < 0)
			goto exit;
	} while (sec < tm->tm_sec);

	/* HW register start mon/wday from one, but tm_mon/tm_wday start from zero. */
	tm->tm_mon--;
	tm->tm_wday--;

exit:
	return ret;
}

static int mtk_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);
	int ret;
	u16 data[RTC_OFFSET_COUNT];

	tm->tm_mon++;
	tm->tm_wday++;

	data[RTC_OFFSET_SEC] = tm->tm_sec;
	data[RTC_OFFSET_MIN] = tm->tm_min;
	data[RTC_OFFSET_HOUR] = tm->tm_hour;
	data[RTC_OFFSET_DOM] = tm->tm_mday;
	data[RTC_OFFSET_DOW] = tm->tm_wday;
	data[RTC_OFFSET_MTH] = tm->tm_mon;
	data[RTC_OFFSET_YEAR] = tm->tm_year;

	mutex_lock(&rtc->lock);
	ret = regmap_bulk_write(rtc->regmap, rtc->addr_base + RTC_TC_SEC,
				data, RTC_OFFSET_COUNT);
	if (ret < 0)
		goto exit;

	/* Time register write to hardware after call trigger function */
	ret = mtk_rtc_write_trigger(rtc);

exit:
	mutex_unlock(&rtc->lock);
	return ret;
}

static int mtk_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alm)
{
	struct rtc_time *tm = &alm->time;
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);
	u32 irqen, pdn2;
	int ret;
	u16 data[RTC_OFFSET_COUNT];

	mutex_lock(&rtc->lock);
	ret = regmap_read(rtc->regmap, rtc->addr_base + RTC_IRQ_EN, &irqen);
	if (ret < 0)
		goto err_exit;
	ret = regmap_read(rtc->regmap, rtc->addr_base + RTC_PDN2, &pdn2);
	if (ret < 0)
		goto err_exit;

	ret = regmap_bulk_read(rtc->regmap, rtc->addr_base + RTC_AL_SEC,
			       data, RTC_OFFSET_COUNT);
	if (ret < 0)
		goto err_exit;

	alm->enabled = !!(irqen & RTC_IRQ_EN_AL);
	alm->pending = !!(pdn2 & RTC_PDN2_PWRON_ALARM);
	mutex_unlock(&rtc->lock);

	tm->tm_sec = data[RTC_OFFSET_SEC] & RTC_AL_SEC_MASK;
	tm->tm_min = data[RTC_OFFSET_MIN] & RTC_AL_MIN_MASK;
	tm->tm_hour = data[RTC_OFFSET_HOUR] & RTC_AL_HOU_MASK;
	tm->tm_mday = data[RTC_OFFSET_DOM] & RTC_AL_DOM_MASK;
	tm->tm_mon = data[RTC_OFFSET_MTH] & RTC_AL_MTH_MASK;
	tm->tm_year = data[RTC_OFFSET_YEAR] & RTC_AL_YEA_MASK;

	tm->tm_mon--;

	return 0;
err_exit:
	mutex_unlock(&rtc->lock);
	return ret;
}

static int mtk_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alm)
{
	struct rtc_time *tm = &alm->time;
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);
	int ret;
	u16 data[RTC_OFFSET_COUNT];

	tm->tm_mon++;

	mutex_lock(&rtc->lock);
	ret = regmap_bulk_read(rtc->regmap, rtc->addr_base + RTC_AL_SEC,
			       data, RTC_OFFSET_COUNT);
	if (ret < 0)
		goto exit;

	data[RTC_OFFSET_SEC] = ((data[RTC_OFFSET_SEC] & ~(RTC_AL_SEC_MASK)) |
				(tm->tm_sec & RTC_AL_SEC_MASK));
	data[RTC_OFFSET_MIN] = ((data[RTC_OFFSET_MIN] & ~(RTC_AL_MIN_MASK)) |
				(tm->tm_min & RTC_AL_MIN_MASK));
	data[RTC_OFFSET_HOUR] = ((data[RTC_OFFSET_HOUR] & ~(RTC_AL_HOU_MASK)) |
				(tm->tm_hour & RTC_AL_HOU_MASK));
	data[RTC_OFFSET_DOM] = ((data[RTC_OFFSET_DOM] & ~(RTC_AL_DOM_MASK)) |
				(tm->tm_mday & RTC_AL_DOM_MASK));
	data[RTC_OFFSET_MTH] = ((data[RTC_OFFSET_MTH] & ~(RTC_AL_MTH_MASK)) |
				(tm->tm_mon & RTC_AL_MTH_MASK));
	data[RTC_OFFSET_YEAR] = ((data[RTC_OFFSET_YEAR] & ~(RTC_AL_YEA_MASK)) |
				(tm->tm_year & RTC_AL_YEA_MASK));

	if (alm->enabled) {
		ret = regmap_bulk_write(rtc->regmap,
					rtc->addr_base + RTC_AL_SEC,
					data, RTC_OFFSET_COUNT);
		if (ret < 0)
			goto exit;
		ret = regmap_write(rtc->regmap, rtc->addr_base + RTC_AL_MASK,
				   RTC_AL_MASK_DOW);
		if (ret < 0)
			goto exit;
		ret = regmap_update_bits(rtc->regmap,
					 rtc->addr_base + RTC_IRQ_EN,
					 RTC_IRQ_EN_ONESHOT_AL,
					 RTC_IRQ_EN_ONESHOT_AL);
		if (ret < 0)
			goto exit;
	} else {
		ret = regmap_update_bits(rtc->regmap,
					 rtc->addr_base + RTC_IRQ_EN,
					 RTC_IRQ_EN_ONESHOT_AL, 0);
		if (ret < 0)
			goto exit;
	}

	/* All alarm time register write to hardware after calling
	 * mtk_rtc_write_trigger. This can avoid race condition if alarm
	 * occur happen during writing alarm time register.
	 */
	ret = mtk_rtc_write_trigger(rtc);
exit:
	mutex_unlock(&rtc->lock);
	return ret;
}

static const struct rtc_class_ops mtk_rtc_ops = {
	.read_time  = mtk_rtc_read_time,
	.set_time   = mtk_rtc_set_time,
	.read_alarm = mtk_rtc_read_alarm,
	.set_alarm  = mtk_rtc_set_alarm,
};

static int mtk_rtc_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct mt6397_chip *mt6397_chip = dev_get_drvdata(pdev->dev.parent);
	struct mt6397_rtc *rtc;
	int ret;

	rtc = devm_kzalloc(&pdev->dev, sizeof(struct mt6397_rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	rtc->addr_base = res->start;

	rtc->data = of_device_get_match_data(&pdev->dev);

	rtc->irq = platform_get_irq(pdev, 0);
	if (rtc->irq < 0)
		return rtc->irq;

	rtc->regmap = mt6397_chip->regmap;
	mutex_init(&rtc->lock);

	platform_set_drvdata(pdev, rtc);

	rtc->rtc_dev = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rtc->rtc_dev))
		return PTR_ERR(rtc->rtc_dev);

	ret = devm_request_threaded_irq(&pdev->dev, rtc->irq, NULL,
					mtk_rtc_irq_handler_thread,
					IRQF_ONESHOT | IRQF_TRIGGER_HIGH,
					"mt6397-rtc", rtc);

	if (ret) {
		dev_err(&pdev->dev, "Failed to request alarm IRQ: %d: %d\n",
			rtc->irq, ret);
		return ret;
	}

	device_init_wakeup(&pdev->dev, true);

	rtc->rtc_dev->ops = &mtk_rtc_ops;
	rtc->rtc_dev->range_min = RTC_TIMESTAMP_BEGIN_1900;
	rtc->rtc_dev->range_max = mktime64(2027, 12, 31, 23, 59, 59);
	rtc->rtc_dev->start_secs = mktime64(1968, 1, 2, 0, 0, 0);
	rtc->rtc_dev->set_start_time = true;

	ret = devm_rtc_register_device(rtc->rtc_dev);
	if (ret)
		return ret;

	if (of_property_read_bool(pdev->dev.of_node, "system-power-controller")) {
		if (!rtc->data->supports_poweroff ||
		    mt6397_chip->chip_id != MT6359_CHIP_ID)
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "power-off requires an MT6359 RTC/PMIC\n");

		ret = devm_register_sys_off_handler(&pdev->dev,
						   SYS_OFF_MODE_POWER_OFF_PREPARE,
						   SYS_OFF_PRIO_DEFAULT,
						   mt6359_rtc_power_off, rtc);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to register MT6359 power-off\n");
		dev_info(&pdev->dev, "registered MT6359 system power-off handler\n");
	}

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int mt6397_rtc_suspend(struct device *dev)
{
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		enable_irq_wake(rtc->irq);

	return 0;
}

static int mt6397_rtc_resume(struct device *dev)
{
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		disable_irq_wake(rtc->irq);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(mt6397_pm_ops, mt6397_rtc_suspend,
			mt6397_rtc_resume);

static const struct mtk_rtc_data mt6358_rtc_data = {
	.wrtgr = RTC_WRTGR_MT6358,
};

static const struct mtk_rtc_data mt6359_rtc_data = {
	.wrtgr = RTC_WRTGR_MT6358,
	.supports_poweroff = true,
};

static const struct mtk_rtc_data mt6397_rtc_data = {
	.wrtgr = RTC_WRTGR_MT6397,
};

static const struct of_device_id mt6397_rtc_of_match[] = {
	{ .compatible = "mediatek,mt6323-rtc", .data = &mt6397_rtc_data },
	{ .compatible = "mediatek,mt6357-rtc", .data = &mt6358_rtc_data },
	{ .compatible = "mediatek,mt6358-rtc", .data = &mt6358_rtc_data },
	{ .compatible = "mediatek,mt6359-rtc", .data = &mt6359_rtc_data },
	{ .compatible = "mediatek,mt6397-rtc", .data = &mt6397_rtc_data },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6397_rtc_of_match);

static struct platform_driver mtk_rtc_driver = {
	.driver = {
		.name = "mt6397-rtc",
		.of_match_table = mt6397_rtc_of_match,
		.pm = &mt6397_pm_ops,
	},
	.probe	= mtk_rtc_probe,
};

module_platform_driver(mtk_rtc_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Tianping Fang <tianping.fang@mediatek.com>");
MODULE_DESCRIPTION("RTC Driver for MediaTek MT6397 PMIC");
