// SPDX-License-Identifier: GPL-2.0-only
/* Guarded ownership of the unused A22 modem power domain during bring-up. */
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

struct mt6833_modem_power {
	struct regmap *spm;
	struct mutex lock;
	bool held;
	bool system_sleep;
};

static int modem_check_power(struct mt6833_modem_power *md, bool on)
{
	u32 status, control, isolation;
	int ret;

	ret = regmap_read(md->spm, 0x16c, &status);
	if (ret)
		return ret;
	ret = regmap_read(md->spm, 0x300, &control);
	if (ret)
		return ret;
	ret = regmap_read(md->spm, 0x398, &isolation);
	if (ret)
		return ret;
	if (on)
		return (status & BIT(0)) && (control & BIT(0)) &&
		       (control & BIT(2)) && !(isolation & GENMASK(1, 0)) ? 0 : -EIO;

	return !(status & BIT(0)) && !(control & (BIT(0) | BIT(2))) &&
	       (isolation & GENMASK(1, 0)) == GENMASK(1, 0) ? 0 : -EIO;
}

static ssize_t parked_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct mt6833_modem_power *md = dev_get_drvdata(dev);

	guard(mutex)(&md->lock);
	return sysfs_emit(buf, "%u\n", !md->held);
}

static ssize_t parked_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct mt6833_modem_power *md = dev_get_drvdata(dev);
	bool park;
	int ret, restore;

	ret = kstrtobool(buf, &park);
	if (ret)
		return ret;
	guard(mutex)(&md->lock);
	if (md->system_sleep)
		return -EBUSY;
	if (park == !md->held) {
		ret = modem_check_power(md, !park);
		return ret ? ret : count;
	}
	if (!park) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0)
			return ret;
		md->held = true;
		ret = modem_check_power(md, true);
		return ret ? ret : count;
	}

	/* genpd runtime_suspend may succeed even if its power_off failed.
	 * Check real power/reset/isolation before reporting a parked modem.
	 */
	ret = pm_runtime_put_sync_suspend(dev);
	md->held = false;
	if (ret >= 0)
		ret = modem_check_power(md, false);
	if (!ret)
		return count;

	restore = pm_runtime_resume_and_get(dev);
	if (restore >= 0) {
		md->held = true;
		restore = modem_check_power(md, true);
	}
	if (restore < 0)
		dev_err(dev, "failed to restore modem power after park error: %d\n",
			restore);
	return ret;
}
static struct device_attribute dev_attr_parked = __ATTR(parked, 0600,
						       parked_show, parked_store);

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct mt6833_modem_power *md = dev_get_drvdata(dev);
	u32 status, control, isolation, requests;
	int ret;

	guard(mutex)(&md->lock);
	ret = regmap_read(md->spm, 0x16c, &status);
	if (ret)
		return ret;
	ret = regmap_read(md->spm, 0x300, &control);
	if (ret)
		return ret;
	ret = regmap_read(md->spm, 0x398, &isolation);
	if (ret)
		return ret;
	ret = regmap_read(md->spm, 0x114, &requests);
	if (ret)
		return ret;
	return sysfs_emit(buf, "held=%u\npowered=%u\ncontrol=0x%x\nisolation=0x%x\nrequests=0x%x\n",
			  md->held, !!(status & BIT(0)), control, isolation, requests);
}
static DEVICE_ATTR_RO(status);

static struct attribute *mt6833_modem_attrs[] = {
	&dev_attr_parked.attr,
	&dev_attr_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mt6833_modem);

static int modem_prepare(struct device *dev)
{
	struct mt6833_modem_power *md = dev_get_drvdata(dev);

	guard(mutex)(&md->lock);
	md->system_sleep = true;
	return 0;
}

static void modem_complete(struct device *dev)
{
	struct mt6833_modem_power *md = dev_get_drvdata(dev);

	guard(mutex)(&md->lock);
	md->system_sleep = false;
}

static const struct dev_pm_ops modem_pm_ops = {
	.prepare = modem_prepare,
	.complete = modem_complete,
};

static int mt6833_modem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6833_modem_power *md;
	int ret;

	if (!of_machine_is_compatible("samsung,a22x") || !dev->pm_domain)
		return -ENODEV;
	md = devm_kzalloc(dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;
	md->spm = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(md->spm))
		return dev_err_probe(dev, PTR_ERR(md->spm), "missing SPM syscon\n");
	mutex_init(&md->lock);
	platform_set_drvdata(pdev, md);
	ret = modem_check_power(md, true);
	if (ret)
		return dev_err_probe(dev, ret, "modem boot state is inconsistent\n");
	ret = device_init_wakeup(dev, true);
	if (ret)
		return ret;
	ret = pm_runtime_set_active(dev);
	if (ret) {
		device_init_wakeup(dev, false);
		return ret;
	}
	pm_runtime_get_noresume(dev);
	md->held = true;
	pm_runtime_enable(dev);
	dev_info(dev, "MD1 owned and held on; parking requires explicit root opt-in\n");
	return 0;
}

static const struct of_device_id modem_of_match[] = {
	{ .compatible = "mediatek,mt6833-modem-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, modem_of_match);

static struct platform_driver mt6833_modem_driver = {
	.probe = mt6833_modem_probe,
	.driver = {
		.name = "mt6833-modem-power",
		.of_match_table = modem_of_match,
		.pm = &modem_pm_ops,
		.dev_groups = mt6833_modem_groups,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6833_modem_driver);
MODULE_LICENSE("GPL");
