// SPDX-License-Identifier: GPL-2.0
/*
 * Himax HX83112F (zero-flash TDDI) touch, mainline SPI platform glue.
 *
 * Replaces the MediaTek "tpd" glue of the Samsung SM-A226B driver with a
 * plain SPI driver: reset and interrupt come from the device tree, the IC
 * firmware (Himax_firmware_fw_jdi.bin) is loaded through request_firmware().
 */
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/property.h>
#include "himax_platform.h"
#include "himax_common.h"

int i2c_error_count;
bool ic_boot_done;
bool himax_gesture_status = DISABLE;

/* LCM id as reported by the stock bootloader: 5 = JDI panel. */
int g_lcm_name = 5;

static uint8_t *gBuffer;
static struct spi_device *hx_spi;
static struct gpio_desc *hx_reset_gpio;
static struct gpio_desc *hx_irq_gpio;
struct device *g_device;

int himax_dev_set(struct himax_ts_data *ts)
{
	ts->input_dev = input_allocate_device();
	if (!ts->input_dev)
		return -ENOMEM;
	ts->input_dev->name = "himax-touchscreen";

	if (!ic_data->HX_STYLUS_FUNC)
		return 0;

	ts->stylus_dev = input_allocate_device();
	if (!ts->stylus_dev) {
		input_free_device(ts->input_dev);
		return -ENOMEM;
	}
	ts->stylus_dev->name = "himax-stylus";
	return 0;
}

int himax_input_register_device(struct input_dev *input_dev)
{
	return input_register_device(input_dev);
}

int himax_parse_dt(struct himax_ts_data *ts, struct himax_platform_data *pdata)
{
	struct device *dev = ts->dev;
	u32 x = 1080, y = 2408;

	device_property_read_u32(dev, "touchscreen-size-x", &x);
	device_property_read_u32(dev, "touchscreen-size-y", &y);

	pdata->gpio_reset = -1;
	pdata->gpio_irq = -1;
	pdata->abs_x_min = 0;
	pdata->abs_x_max = x;
	pdata->abs_y_min = 0;
	pdata->abs_y_max = y;
	pdata->screenWidth = x;
	pdata->screenHeight = y;
	pdata->protocol_type = PROTOCOL_TYPE_B;
	return 0;
}

static int himax_spi_read(uint8_t *command, uint8_t command_len, uint8_t *data,
			  uint32_t length)
{
	struct spi_message message;
	struct spi_transfer xfer[2] = {};
	int retry, error = -EIO;

	spi_message_init(&message);
	xfer[0].tx_buf = command;
	xfer[0].len = command_len;
	spi_message_add_tail(&xfer[0], &message);
	xfer[1].tx_buf = data;
	xfer[1].rx_buf = data;
	xfer[1].len = length;
	spi_message_add_tail(&xfer[1], &message);

	for (retry = 0; retry < HIMAX_BUS_RETRY_TIMES; retry++) {
		error = spi_sync(hx_spi, &message);
		if (!error)
			return 0;
	}
	E("%s: SPI read failed: %d\n", __func__, error);
	return -EIO;
}

int himax_bus_read(uint8_t command, uint8_t *data, uint32_t length)
{
	uint8_t spi_format_buf[3] = { 0xF3, command, 0x00 };
	int result;

	mutex_lock(&private_ts->rw_lock);
	result = himax_spi_read(spi_format_buf, 3, data, length);
	mutex_unlock(&private_ts->rw_lock);
	return result;
}

int himax_bus_write(uint8_t command, uint8_t *data, uint32_t length)
{
	int result;

	mutex_lock(&private_ts->rw_lock);
	gBuffer[0] = 0xF2;
	gBuffer[1] = command;
	memcpy(&gBuffer[2], data, length);
	result = spi_write(hx_spi, gBuffer, length + 2);
	mutex_unlock(&private_ts->rw_lock);
	return result;
}

uint8_t himax_int_gpio_read(int pinnum)
{
	return hx_irq_gpio ? gpiod_get_raw_value_cansleep(hx_irq_gpio) : 1;
}

void himax_int_enable(int enable)
{
	struct himax_ts_data *ts = private_ts;
	unsigned long irqflags;

	spin_lock_irqsave(&ts->irq_lock, irqflags);
	if (enable == 1 && atomic_read(&ts->irq_state) == 0) {
		atomic_set(&ts->irq_state, 1);
		enable_irq(ts->hx_irq);
		ts->irq_enabled = 1;
	} else if (enable == 0 && atomic_read(&ts->irq_state) == 1) {
		atomic_set(&ts->irq_state, 0);
		disable_irq_nosync(ts->hx_irq);
		ts->irq_enabled = 0;
	}
	spin_unlock_irqrestore(&ts->irq_lock, irqflags);
}

void himax_rst_gpio_set(int pinnum, uint8_t value)
{
	/* The common code passes physical levels (0 = in reset). */
	if (hx_reset_gpio)
		gpiod_set_raw_value_cansleep(hx_reset_gpio, value);
}

int himax_gpio_power_config(struct himax_platform_data *pdata)
{
	/* TDDI: touch shares the display supplies, which LK leaves enabled. */
	himax_rst_gpio_set(0, 1);
	msleep(20);
	himax_rst_gpio_set(0, 0);
	msleep(20);
	himax_rst_gpio_set(0, 1);
	return 0;
}

void himax_gpio_power_deconfig(struct himax_platform_data *pdata)
{
}

static irqreturn_t himax_ts_thread(int irq, void *ptr)
{
	himax_ts_work(ptr);
	return IRQ_HANDLED;
}

int himax_int_en_set(void)
{
	return 0;
}

int himax_ts_register_interrupt(void)
{
	struct himax_ts_data *ts = private_ts;
	unsigned long flags = ic_data->HX_INT_IS_EDGE ?
			      IRQF_TRIGGER_FALLING : IRQF_TRIGGER_LOW;
	int ret;

	ts->hx_irq = hx_spi->irq;
	ts->use_irq = 1;
	ret = request_threaded_irq(ts->hx_irq, NULL, himax_ts_thread,
				   flags | IRQF_ONESHOT, HIMAX_common_NAME, ts);
	if (ret) {
		E("%s: request_irq %d failed: %d\n", __func__, ts->hx_irq, ret);
		ts->use_irq = 0;
		return ret;
	}
	ts->irq_enabled = 1;
	atomic_set(&ts->irq_state, 1);
	return 0;
}

int himax_ts_unregister_interrupt(void)
{
	struct himax_ts_data *ts = private_ts;

	if (ts->hx_irq && ts->use_irq)
		free_irq(ts->hx_irq, ts);
	return 0;
}

static int himax_hx83112f_probe(struct spi_device *spi)
{
	struct himax_ts_data *ts;
	int ret;

	hx_reset_gpio = devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_ASIS);
	if (IS_ERR(hx_reset_gpio))
		return dev_err_probe(&spi->dev, PTR_ERR(hx_reset_gpio), "reset gpio\n");
	if (hx_reset_gpio)
		gpiod_direction_output_raw(hx_reset_gpio, 1);
	hx_irq_gpio = devm_gpiod_get_optional(&spi->dev, "irq", GPIOD_IN);
	if (IS_ERR(hx_irq_gpio))
		return dev_err_probe(&spi->dev, PTR_ERR(hx_irq_gpio), "irq gpio\n");

	/* command[2] + address[4] + data[] */
	gBuffer = devm_kzalloc(&spi->dev, HX_MAX_WRITE_SZ + 6, GFP_KERNEL);
	ts = kzalloc(sizeof(*ts), GFP_KERNEL);
	if (!gBuffer || !ts) {
		kfree(ts);
		return -ENOMEM;
	}

	hx_spi = spi;
	spi->mode = SPI_MODE_3;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		goto err;

	spi_set_drvdata(spi, ts);
	ts->dev = &spi->dev;
	g_device = &spi->dev;
	private_ts = ts;
	mutex_init(&ts->rw_lock);

	g_fw_boot_upgrade_name = "Himax_firmware_fw_jdi.bin";
	g_fw_mp_upgrade_name = "Himax_firmware_mp_jdi.bin";

	ret = himax_chip_common_init();
	if (ret < 0)
		goto err;
	dev_info(&spi->dev, "HX83112F touch ready, irq %d\n", spi->irq);
	return 0;
err:
	kfree(ts);
	private_ts = NULL;
	return ret;
}

static void himax_hx83112f_remove(struct spi_device *spi)
{
	if (g_hx_chip_inited)
		himax_chip_common_deinit();
}

static int himax_hx83112f_suspend(struct device *dev)
{
	himax_chip_common_suspend(private_ts);
	return 0;
}

static int himax_hx83112f_resume(struct device *dev)
{
	himax_chip_common_resume(private_ts);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(himax_hx83112f_pm, himax_hx83112f_suspend,
				himax_hx83112f_resume);

static const struct of_device_id himax_hx83112f_of_match[] = {
	{ .compatible = "himax,hx83112f" },
	/* name used by the stock dtbo overlay on SM-A226B */
	{ .compatible = "himax,common" },
	{ }
};
MODULE_DEVICE_TABLE(of, himax_hx83112f_of_match);

static const struct spi_device_id himax_hx83112f_id[] = {
	{ "hx83112f" },
	{ }
};
MODULE_DEVICE_TABLE(spi, himax_hx83112f_id);

static struct spi_driver himax_hx83112f_driver = {
	.driver = {
		.name = HIMAX_common_NAME,
		.of_match_table = himax_hx83112f_of_match,
		.pm = pm_sleep_ptr(&himax_hx83112f_pm),
	},
	.id_table = himax_hx83112f_id,
	.probe = himax_hx83112f_probe,
	.remove = himax_hx83112f_remove,
};
module_spi_driver(himax_hx83112f_driver);

MODULE_DESCRIPTION("Himax HX83112F zero-flash TDDI touchscreen (SPI)");
MODULE_LICENSE("GPL");
