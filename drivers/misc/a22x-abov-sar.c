// SPDX-License-Identifier: GPL-2.0-only
/*
 * ABOV capacitive SAR/grip sensor on the Samsung Galaxy A22 5G (i2c9, 0x20).
 *
 * Without a driver the chip runs its power-on scan for ever. The stock
 * driver stops it (CTRL_MODE 0x07 = 0x02) unless userspace enables the grip
 * channels, and puts it into sleep or stop for system suspend. This driver
 * only keeps the chip stopped; "mode" in sysfs allows ACTIVE/SLEEP/STOP for
 * measurements. No sensing is reported.
 */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/pm.h>

#define ABOV_CTRL_MODE		0x07
#define ABOV_MODE_ACTIVE	0x00
#define ABOV_MODE_SLEEP		0x01
#define ABOV_MODE_STOP		0x02

struct abov_sar {
	struct i2c_client *client;
	u8 mode;
};

static int abov_sar_set_mode(struct abov_sar *sar, u8 mode)
{
	int ret = i2c_smbus_write_byte_data(sar->client, ABOV_CTRL_MODE, mode);

	if (ret)
		return ret;
	ret = i2c_smbus_read_byte_data(sar->client, ABOV_CTRL_MODE);
	if (ret < 0)
		return ret;
	return ret == mode ? 0 : -EIO;
}

static ssize_t mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct abov_sar *sar = dev_get_drvdata(dev);
	int ret = i2c_smbus_read_byte_data(sar->client, ABOV_CTRL_MODE);

	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", ret);
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct abov_sar *sar = dev_get_drvdata(dev);
	u8 mode;
	int ret;

	ret = kstrtou8(buf, 0, &mode);
	if (ret)
		return ret;
	if (mode > ABOV_MODE_STOP)
		return -EINVAL;
	ret = abov_sar_set_mode(sar, mode);
	if (ret)
		return ret;
	sar->mode = mode;
	return count;
}
static DEVICE_ATTR_RW(mode);

static struct attribute *abov_sar_attrs[] = {
	&dev_attr_mode.attr,
	NULL
};
ATTRIBUTE_GROUPS(abov_sar);

static int abov_sar_probe(struct i2c_client *client)
{
	struct abov_sar *sar;
	int ret;

	sar = devm_kzalloc(&client->dev, sizeof(*sar), GFP_KERNEL);
	if (!sar)
		return -ENOMEM;
	sar->client = client;
	i2c_set_clientdata(client, sar);

	ret = i2c_smbus_read_byte_data(client, ABOV_CTRL_MODE);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret, "no response\n");
	dev_info(&client->dev, "power-on mode %#x, stopping\n", ret);

	sar->mode = ABOV_MODE_STOP;
	ret = abov_sar_set_mode(sar, sar->mode);
	if (ret)
		return dev_err_probe(&client->dev, ret, "cannot stop sensor\n");
	return 0;
}

static int abov_sar_resume(struct device *dev)
{
	struct abov_sar *sar = dev_get_drvdata(dev);

	/* The supply stays on; re-assert the selected mode in case it changed. */
	return abov_sar_set_mode(sar, sar->mode);
}
static DEFINE_SIMPLE_DEV_PM_OPS(abov_sar_pm_ops, NULL, abov_sar_resume);

static const struct of_device_id abov_sar_of_match[] = {
	{ .compatible = "a22x,abov-sar" },
	{ }
};
MODULE_DEVICE_TABLE(of, abov_sar_of_match);

static struct i2c_driver abov_sar_driver = {
	.driver = {
		.name = "a22x-abov-sar",
		.of_match_table = abov_sar_of_match,
		.dev_groups = abov_sar_groups,
		.pm = pm_sleep_ptr(&abov_sar_pm_ops),
	},
	.probe = abov_sar_probe,
};
module_i2c_driver(abov_sar_driver);

MODULE_DESCRIPTION("Samsung A22 5G ABOV SAR sensor stop driver");
MODULE_LICENSE("GPL");
