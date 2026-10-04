// SPDX-License-Identifier: GPL-2.0-only
/*
 * MN78911 ambient light + proximity sensor (Samsung Galaxy A22 5G).
 *
 * No datasheet. The register usage comes from the stock SCP firmware and
 * from measurements on the device; the chip runs with its power-on
 * configuration (gain, integration time, LED current are not set):
 *   0x00 mode: bit1 ALS, bit0 PS
 *   0x02 control: 1 = stop, 2 = start
 *   0x38 BE16 proximity (IR reflection, ignores visible light),
 *        only valid with both ALS and PS enabled
 *   0x3a BE16 light, bit 15 is a flag, 0x7fff = saturated
 * Light counts are not calibrated: ~12-20 in a dim room, saturated under a
 * lamp; the scale is a rough guess good enough for auto-brightness.
 */
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>
#include <linux/unaligned.h>

#define MN78911_REG_MODE	0x00
#define MN78911_REG_CTRL	0x02
#define MN78911_REG_DATA	0x38

#define MN78911_MODE_ALS_PS	0x03
#define MN78911_CTRL_STOP	0x01
#define MN78911_CTRL_START	0x02

/* rough: a dim room (~10 lux) reads ~20 counts */
#define MN78911_LUX_SCALE_MICRO	500000

struct mn78911 {
	struct i2c_client *client;
	u32 near_level;
};

static int mn78911_start(struct i2c_client *client)
{
	int ret = i2c_smbus_write_byte_data(client, MN78911_REG_MODE,
					    MN78911_MODE_ALS_PS);

	if (ret)
		return ret;
	return i2c_smbus_write_byte_data(client, MN78911_REG_CTRL,
					 MN78911_CTRL_START);
}

static int mn78911_stop(struct i2c_client *client)
{
	return i2c_smbus_write_byte_data(client, MN78911_REG_CTRL,
					 MN78911_CTRL_STOP);
}

static int mn78911_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct mn78911 *data = iio_priv(indio_dev);
	u8 buf[4];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = i2c_smbus_read_i2c_block_data(data->client,
						    MN78911_REG_DATA,
						    sizeof(buf), buf);
		if (ret < 0)
			return ret;
		if (ret != sizeof(buf))
			return -EIO;
		if (chan->type == IIO_PROXIMITY)
			*val = get_unaligned_be16(&buf[0]);
		else
			*val = get_unaligned_be16(&buf[2]) & 0x7fff;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = MN78911_LUX_SCALE_MICRO;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static ssize_t mn78911_read_near_level(struct iio_dev *indio_dev,
				       uintptr_t priv,
				       const struct iio_chan_spec *chan,
				       char *buf)
{
	struct mn78911 *data = iio_priv(indio_dev);

	return sysfs_emit(buf, "%u\n", data->near_level);
}

static const struct iio_chan_spec_ext_info mn78911_ps_ext_info[] = {
	{
		.name = "nearlevel",
		.shared = IIO_SEPARATE,
		.read = mn78911_read_near_level,
	},
	{ }
};

static const struct iio_chan_spec mn78911_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	}, {
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.ext_info = mn78911_ps_ext_info,
	},
};

static const struct iio_info mn78911_info = {
	.read_raw = mn78911_read_raw,
};

static void mn78911_disable(void *client)
{
	mn78911_stop(client);
}

static int mn78911_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct mn78911 *data;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;
	data = iio_priv(indio_dev);
	data->client = client;
	data->near_level = 200;
	device_property_read_u32(dev, "proximity-near-level", &data->near_level);

	ret = mn78911_start(client);
	if (ret)
		return dev_err_probe(dev, ret, "failed to start\n");
	ret = devm_add_action_or_reset(dev, mn78911_disable, client);
	if (ret)
		return ret;

	indio_dev->name = "mn78911";
	indio_dev->info = &mn78911_info;
	indio_dev->channels = mn78911_channels;
	indio_dev->num_channels = ARRAY_SIZE(mn78911_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	return devm_iio_device_register(dev, indio_dev);
}

static int mn78911_suspend(struct device *dev)
{
	return mn78911_stop(to_i2c_client(dev));
}

static int mn78911_resume(struct device *dev)
{
	return mn78911_start(to_i2c_client(dev));
}

static DEFINE_SIMPLE_DEV_PM_OPS(mn78911_pm_ops, mn78911_suspend, mn78911_resume);

/* vendor prefix unknown; the part is only known by its number */
static const struct of_device_id mn78911_of_match[] = {
	{ .compatible = "a22x,mn78911" },
	{ }
};
MODULE_DEVICE_TABLE(of, mn78911_of_match);

static const struct i2c_device_id mn78911_id[] = {
	{ "mn78911" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mn78911_id);

static struct i2c_driver mn78911_driver = {
	.driver = {
		.name = "mn78911",
		.of_match_table = mn78911_of_match,
		.pm = pm_sleep_ptr(&mn78911_pm_ops),
	},
	.probe = mn78911_probe,
	.id_table = mn78911_id,
};
module_i2c_driver(mn78911_driver);

MODULE_DESCRIPTION("MN78911 ambient light and proximity sensor");
MODULE_LICENSE("GPL");
