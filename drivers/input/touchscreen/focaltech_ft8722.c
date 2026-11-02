// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal SPI touchscreen driver for the FocalTech FT8722.
 *
 * The SPI framing, firmware download, and touch record layout are based on
 * the FT8722 vendor driver. Gestures and factory interfaces are not handled.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/string.h>

#define FT8722_MAX_TOUCHES	10
#define FT8722_POINT_SIZE	6
#define FT8722_FRAME_SIZE	(FT8722_MAX_TOUCHES * FT8722_POINT_SIZE + 3)
#define FT8722_SPI_HEADER_SIZE	4
#define FT8722_SPI_DUMMY_SIZE	3
#define FT8722_SPI_CRC_SIZE	2
#define FT8722_SPI_PACKET_SIZE	1024
#define FT8722_SPI_READ_CMD	0xa0
#define FT8722_SPI_RETRIES	3
#define FT8722_SPI_RETRY_DELAY_US	150
#define FT8722_TOUCH_DATA_CMD	0x01
#define FT8722_RESET_DELAY_MS	10
#define FT8722_STARTUP_DELAY_MS	20

#define FT8722_FW_WRITE_CMD	0x00
#define FT8722_FW_START_CMD	0x55
#define FT8722_FW_READ_ID_CMD	0x90
#define FT8722_FW_ROM_ID_H	0x87
#define FT8722_FW_ROM_ID_L	0x22
#define FT8722_FW_APP_ID_REG	0xa3
#define FT8722_FW_APP_ID	0x87
#define FT8722_FW_SET_ADDR_CMD	0xad
#define FT8722_FW_WRITE_DATA_CMD	0xae
#define FT8722_FW_START_APP_CMD	0x08
#define FT8722_FW_ECC_CMD	0xcc
#define FT8722_FW_ECC_FINISH_CMD	0xce
#define FT8722_FW_ECC_READ_CMD	0xcd
#define FT8722_FW_ECC_FINISH	0xa5
#define FT8722_FW_DRAM_ADDR	0xd00000
#define FT8722_FW_INFO_OFFSET	0x100
#define FT8722_FW_MAX_APP_SIZE	(88 * 1024)
#define FT8722_FW_PACKET_SIZE	(32 * 1024 - 16)
#define FT8722_FW_ECC_CHUNK_SIZE	32766
#define FT8722_FW_BOOT_RETRIES	30
#define FT8722_FW_DOWNLOAD_RETRIES	5
#define FT8722_FW_RETRY_DELAY_MS	100
#define FT8722_FW_READY_RETRIES	10
#define FT8722_FW_READY_DELAY_MS	200
#define FT8722_FW_START_DELAY_MS	50
#define FT8722_FW_RESET_WAIT_MS	200

static const u8 ft8722_firmware[] = {
#include "firmware/FT8722_truly.i"
};

struct ft8722_ts {
	struct spi_device *spi;
	struct input_dev *input;
	struct gpio_desc *reset_gpio;
	struct touchscreen_properties prop;
	unsigned long active_slots;
	u32 max_x;
	u32 max_y;
};

static int ft8722_spi_sync_buf(struct ft8722_ts *ts, const u8 *tx, u8 *rx,
			       size_t len)
{
	struct spi_message message;
	struct spi_transfer *transfers;
	unsigned int count = DIV_ROUND_UP(len, FT8722_SPI_PACKET_SIZE);
	unsigned int i;
	size_t offset = 0;
	int ret;

	transfers = kcalloc(count, sizeof(*transfers), GFP_KERNEL);
	if (!transfers)
		return -ENOMEM;

	spi_message_init(&message);
	for (i = 0; i < count; i++) {
		size_t transfer_len = min_t(size_t,
					    len - offset,
					    FT8722_SPI_PACKET_SIZE);

		transfers[i].tx_buf = tx + offset;
		transfers[i].rx_buf = rx ? rx + offset : NULL;
		transfers[i].len = transfer_len;
		spi_message_add_tail(&transfers[i], &message);
		offset += transfer_len;
	}

	ret = spi_sync(ts->spi, &message);
	kfree(transfers);

	return ret;
}

static bool ft8722_panel_selected(void)
{
	return saved_command_line &&
	       strstr(saved_command_line,
		      "ft8722_fhdp_wt_dsi_vdo_cphy_90hz_truly");
}

static u16 ft8722_crc_kermit(const u8 *data, size_t len)
{
	u16 crc = 0xffff;
	unsigned int i, bit;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
	}

	return crc;
}

static int ft8722_spi_read(struct ft8722_ts *ts, u8 command,
			   void *data, size_t len)
{
	size_t prefix_len = FT8722_SPI_HEADER_SIZE + FT8722_SPI_DUMMY_SIZE;
	size_t transfer_len = prefix_len + len + FT8722_SPI_CRC_SIZE;
	u8 tx[FT8722_SPI_HEADER_SIZE + FT8722_SPI_DUMMY_SIZE +
	      FT8722_FRAME_SIZE + FT8722_SPI_CRC_SIZE] = { 0 };
	u8 rx[sizeof(tx)] = { 0 };
	u16 received_crc, calculated_crc;
	unsigned int attempt;
	int ret = -EIO;

	if (!len || len > FT8722_FRAME_SIZE)
		return -EINVAL;

	tx[0] = command;
	tx[1] = FT8722_SPI_READ_CMD;
	tx[2] = (len >> 8) & 0xff;
	tx[3] = len & 0xff;

	for (attempt = 0; attempt < FT8722_SPI_RETRIES; attempt++) {
		memset(rx, 0, sizeof(rx));
		ret = ft8722_spi_sync_buf(ts, tx, rx, transfer_len);
		udelay(FT8722_SPI_RETRY_DELAY_US);
		if (!ret) {
			if (rx[3] & 0xa0) {
				ret = -EIO;
			} else {
				received_crc = (rx[prefix_len + len + 1] << 8) |
					       rx[prefix_len + len];
				calculated_crc =
					ft8722_crc_kermit(rx + prefix_len, len);
				if (received_crc == calculated_crc) {
					memcpy(data, rx + prefix_len, len);
					return 0;
				}
				ret = -EBADMSG;
			}
		}
	}

	return ret;
}

static int ft8722_spi_write(struct ft8722_ts *ts, u8 command,
			    const void *data, size_t len)
{
	size_t transfer_len = FT8722_SPI_HEADER_SIZE;
	u8 *tx;
	u8 *rx;
	unsigned int attempt;
	int ret = -EIO;

	if (len > FT8722_FW_PACKET_SIZE || (len && !data))
		return -EINVAL;
	if (len)
		transfer_len += FT8722_SPI_DUMMY_SIZE + len;

	tx = kzalloc(transfer_len, GFP_KERNEL);
	rx = kzalloc(transfer_len, GFP_KERNEL);
	if (!tx || !rx) {
		ret = -ENOMEM;
		goto out;
	}

	tx[0] = command;
	tx[1] = FT8722_FW_WRITE_CMD;
	tx[2] = (len >> 8) & 0xff;
	tx[3] = len & 0xff;
	if (len)
		memcpy(tx + FT8722_SPI_HEADER_SIZE + FT8722_SPI_DUMMY_SIZE,
		       data, len);

	for (attempt = 0; attempt < FT8722_SPI_RETRIES; attempt++) {
		memset(rx, 0, transfer_len);
		ret = ft8722_spi_sync_buf(ts, tx, rx, transfer_len);
		udelay(FT8722_SPI_RETRY_DELAY_US);
		if (!ret && !(rx[3] & 0xa0))
			break;
		if (!ret)
			ret = -EIO;
	}

out:
	kfree(rx);
	kfree(tx);
	return ret;
}

static int ft8722_enter_bootloader(struct ft8722_ts *ts)
{
	u8 boot_id[2];
	unsigned int attempt;
	int ret = -EIO;

	for (attempt = 0; attempt < FT8722_FW_BOOT_RETRIES; attempt++) {
		gpiod_set_value_cansleep(ts->reset_gpio, 1);
		msleep(FT8722_RESET_DELAY_MS);
		gpiod_set_value_cansleep(ts->reset_gpio, 0);
		msleep(FT8722_STARTUP_DELAY_MS);

		ret = ft8722_spi_write(ts, FT8722_FW_START_CMD, NULL, 0);
		if (ret)
			continue;
		msleep(FT8722_STARTUP_DELAY_MS);

		ret = ft8722_spi_read(ts, FT8722_FW_READ_ID_CMD, boot_id,
				      sizeof(boot_id));
		if (!ret && boot_id[0] == FT8722_FW_ROM_ID_H &&
		    boot_id[1] == FT8722_FW_ROM_ID_L)
			return 0;
	}

	return ret ? ret : -ENODEV;
}

static int ft8722_write_ram(struct ft8722_ts *ts, u32 address,
			    const u8 *data, size_t len)
{
	size_t offset;

	for (offset = 0; offset < len; offset += FT8722_FW_PACKET_SIZE) {
		u32 current_address = address + offset;
		size_t packet_len = min_t(size_t, len - offset,
					  FT8722_FW_PACKET_SIZE);
		u8 address_data[] = {
			(current_address >> 16) & 0xff,
			(current_address >> 8) & 0xff,
			current_address & 0xff,
		};
		int ret;

		ret = ft8722_spi_write(ts, FT8722_FW_SET_ADDR_CMD,
				       address_data, sizeof(address_data));
		if (ret)
			return ret;

		ret = ft8722_spi_write(ts, FT8722_FW_WRITE_DATA_CMD,
				       data + offset, packet_len);
		if (ret)
			return ret;
	}

	return 0;
}

static u16 ft8722_firmware_ecc(const u8 *data, size_t len)
{
	u16 ecc = 0;
	size_t i;
	unsigned int bit;

	for (i = 0; i < len; i += 2) {
		ecc ^= (data[i] << 8) | data[i + 1];
		for (bit = 0; bit < 16; bit++)
			ecc = (ecc & 1) ? (ecc >> 1) ^ 0x8408 : ecc >> 1;
	}

	return ecc;
}

static int ft8722_check_ram_ecc(struct ft8722_ts *ts, u32 address,
				const u8 *data, size_t len)
{
	size_t offset;

	if (len & 1)
		return -EINVAL;

	for (offset = 0; offset < len; offset += FT8722_FW_ECC_CHUNK_SIZE) {
		u32 current_address = address + offset;
		size_t chunk_len = min_t(size_t, len - offset,
					 FT8722_FW_ECC_CHUNK_SIZE);
		u8 command_data[] = {
			(current_address >> 16) & 0xff,
			(current_address >> 8) & 0xff,
			current_address & 0xff,
			(chunk_len >> 16) & 0xff,
			(chunk_len >> 8) & 0xff,
			chunk_len & 0xff,
		};
		u8 ecc_status = 0;
		u8 ecc_data[2];
		u16 host_ecc;
		u16 controller_ecc;
		unsigned int attempt;
		int ret;

		ret = ft8722_spi_write(ts, FT8722_FW_ECC_CMD, command_data,
				       sizeof(command_data));
		if (ret)
			return ret;
		msleep(2);

		for (attempt = 0; attempt < 100; attempt++) {
			ret = ft8722_spi_read(ts, FT8722_FW_ECC_FINISH_CMD,
					      &ecc_status, sizeof(ecc_status));
			if (ret)
				return ret;
			if (ecc_status == FT8722_FW_ECC_FINISH)
				break;
			msleep(1);
		}
		if (attempt == 100)
			return -ETIMEDOUT;

		ret = ft8722_spi_read(ts, FT8722_FW_ECC_READ_CMD, ecc_data,
				      sizeof(ecc_data));
		if (ret)
			return ret;

		host_ecc = ft8722_firmware_ecc(data + offset, chunk_len);
		controller_ecc = (ecc_data[0] << 8) | ecc_data[1];
		if (host_ecc != controller_ecc)
			return -EBADMSG;
	}

	return 0;
}

static int ft8722_wait_for_application(struct ft8722_ts *ts)
{
	unsigned int attempt;
	u8 chip_id = 0;
	int ret = -EIO;

	for (attempt = 0; attempt < FT8722_FW_READY_RETRIES; attempt++) {
		ret = ft8722_spi_read(ts, FT8722_FW_APP_ID_REG, &chip_id,
				      sizeof(chip_id));
		if (!ret && chip_id == FT8722_FW_APP_ID)
			return 0;
		if (!ret)
			ret = -ENODEV;
		if (attempt + 1 < FT8722_FW_READY_RETRIES)
			msleep(FT8722_FW_READY_DELAY_MS);
	}

	dev_err(&ts->spi->dev,
		"application firmware did not start (chip ID 0x%02x)\n",
		chip_id);
	return ret;
}

static int ft8722_download_firmware_once(struct ft8722_ts *ts)
{
	u16 code_words, code_words_inv, dram_words, dram_words_inv;
	size_t code_len, dram_len;
	u8 start_app = FT8722_FW_START_APP_CMD;
	int ret;

	if (sizeof(ft8722_firmware) < FT8722_FW_INFO_OFFSET + 12)
		return -EINVAL;

	code_words = (ft8722_firmware[FT8722_FW_INFO_OFFSET] << 8) |
		     ft8722_firmware[FT8722_FW_INFO_OFFSET + 1];
	code_words_inv = (ft8722_firmware[FT8722_FW_INFO_OFFSET + 2] << 8) |
			 ft8722_firmware[FT8722_FW_INFO_OFFSET + 3];
	dram_words = (ft8722_firmware[FT8722_FW_INFO_OFFSET + 8] << 8) |
		     ft8722_firmware[FT8722_FW_INFO_OFFSET + 9];
	dram_words_inv = (ft8722_firmware[FT8722_FW_INFO_OFFSET + 10] << 8) |
			 ft8722_firmware[FT8722_FW_INFO_OFFSET + 11];
	if ((u16)(code_words + code_words_inv) != 0xffff ||
	    (u16)(dram_words + dram_words_inv) != 0xffff)
		return -EINVAL;

	code_len = (size_t)code_words * 2;
	dram_len = (size_t)dram_words * 2;
	if (!code_len || code_len > FT8722_FW_MAX_APP_SIZE ||
	    dram_len > FT8722_FW_MAX_APP_SIZE ||
	    code_len > sizeof(ft8722_firmware) ||
	    dram_len > sizeof(ft8722_firmware) - code_len)
		return -EINVAL;

	ret = ft8722_enter_bootloader(ts);
	if (ret)
		return ret;

	dev_info(&ts->spi->dev,
		 "downloading FT8722 firmware (PRAM %zu, DRAM %zu bytes)\n",
		 code_len, dram_len);

	ret = ft8722_write_ram(ts, 0, ft8722_firmware, code_len);
	if (ret)
		return ret;
	ret = ft8722_check_ram_ecc(ts, 0, ft8722_firmware, code_len);
	if (ret)
		return ret;

	if (dram_len) {
		ret = ft8722_write_ram(ts, FT8722_FW_DRAM_ADDR,
				       ft8722_firmware + code_len, dram_len);
		if (ret)
			return ret;
		ret = ft8722_check_ram_ecc(ts, FT8722_FW_DRAM_ADDR,
					   ft8722_firmware + code_len,
					   dram_len);
		if (ret)
			return ret;
	}

	ret = ft8722_spi_write(ts, start_app, NULL, 0);
	if (ret)
		return ret;
	msleep(FT8722_FW_START_DELAY_MS);

	ret = ft8722_wait_for_application(ts);
	if (ret)
		return ret;

	dev_info(&ts->spi->dev, "FT8722 firmware started successfully\n");
	return 0;
}

static int ft8722_download_firmware(struct ft8722_ts *ts)
{
	unsigned int attempt;
	int ret = -EIO;

	for (attempt = 0; attempt < FT8722_FW_DOWNLOAD_RETRIES; attempt++) {
		ret = ft8722_download_firmware_once(ts);
		if (!ret)
			return 0;

		dev_warn(&ts->spi->dev,
			 "firmware download attempt %u failed: %d\n",
			 attempt + 1, ret);
		if (attempt + 1 < FT8722_FW_DOWNLOAD_RETRIES)
			msleep(FT8722_FW_RETRY_DELAY_MS);
	}

	return ret;
}

static void ft8722_release_slots(struct ft8722_ts *ts,
				 unsigned long slots)
{
	unsigned int id;

	for_each_set_bit(id, &slots, FT8722_MAX_TOUCHES) {
		input_mt_slot(ts->input, id);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	}
}

static int ft8722_report_frame(struct ft8722_ts *ts, const u8 *frame)
{
	unsigned long active_slots = 0;
	unsigned long released_slots;
	unsigned long seen_slots = 0;
	unsigned int point_count = frame[1] & 0x0f;
	unsigned int i;

	if (point_count > FT8722_MAX_TOUCHES)
		return -EPROTO;

	for (i = 0; i < FT8722_MAX_TOUCHES; i++) {
		const u8 *point = frame + 2 + i * FT8722_POINT_SIZE;
		unsigned int event = point[0] >> 6;
		unsigned int id = point[2] >> 4;
		unsigned int pressure, area;
		unsigned int x, y;

		if (id >= FT8722_MAX_TOUCHES)
			break;
		if (event == 3 || test_and_set_bit(id, &seen_slots))
			continue;
		if (event == 1)
			continue;
		if (event != 0 && event != 2)
			continue;
		if (!point_count)
			continue;

		x = ((point[0] & 0x0f) << 8) | point[1];
		y = ((point[2] & 0x0f) << 8) | point[3];
		if (x > ts->max_x || y > ts->max_y)
			continue;

		active_slots |= BIT(id);
		input_mt_slot(ts->input, id);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(ts->input, &ts->prop, x, y, true);
		pressure = point[4] ? point[4] : 0x3f;
		area = point[5] >> 4;
		input_report_abs(ts->input, ABS_MT_PRESSURE, pressure);
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, area ? area : 0x09);
	}

	released_slots = ts->active_slots & ~active_slots;
	ft8722_release_slots(ts, released_slots);
	ts->active_slots = active_slots;

	input_mt_report_pointer_emulation(ts->input, true);
	input_sync(ts->input);

	return 0;
}

static irqreturn_t ft8722_irq_thread(int irq, void *data)
{
	struct ft8722_ts *ts = data;
	u8 frame[FT8722_FRAME_SIZE];
	int ret;

	ret = ft8722_spi_read(ts, FT8722_TOUCH_DATA_CMD, frame, sizeof(frame));
	if (ret) {
		dev_err_ratelimited(&ts->spi->dev,
				    "failed to read touch report: %d\n", ret);
		return IRQ_HANDLED;
	}

	ret = ft8722_report_frame(ts, frame);
	if (ret)
		dev_err_ratelimited(&ts->spi->dev,
				    "invalid touch report: %d\n", ret);

	return IRQ_HANDLED;
}

static int ft8722_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct ft8722_ts *ts;
	unsigned long irq_flags;
	int ret;

	if (!ft8722_panel_selected())
		return -ENODEV;
	dev_info(&spi->dev, "selected by FT8722 panel name in bootargs\n");

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "failed to configure SPI\n");

	if (spi->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "missing touchscreen IRQ\n");

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->spi = spi;
	ts->max_x = 1080;
	ts->max_y = 2408;
	if (device_property_present(dev, "touchscreen-size-x")) {
		ret = device_property_read_u32(dev, "touchscreen-size-x",
					       &ts->max_x);
		if (ret)
			return dev_err_probe(dev, ret,
					     "invalid touchscreen-size-x\n");
	}
	if (device_property_present(dev, "touchscreen-size-y")) {
		ret = device_property_read_u32(dev, "touchscreen-size-y",
					       &ts->max_y);
		if (ret)
			return dev_err_probe(dev, ret,
					     "invalid touchscreen-size-y\n");
	}
	if (!ts->max_x || !ts->max_y)
		return dev_err_probe(dev, -EINVAL,
				     "invalid touchscreen dimensions\n");

	ts->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio),
				     "failed to get reset GPIO\n");

	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	msleep(1);
	gpiod_set_value_cansleep(ts->reset_gpio, 0);
	msleep(FT8722_FW_RESET_WAIT_MS);

	ret = ft8722_download_firmware(ts);
	if (ret) {
		dev_err(&spi->dev,
			"firmware download failed after %u attempts: %d\n",
			FT8722_FW_DOWNLOAD_RETRIES, ret);
		/*
		 * Do not register an input device while the controller is still
		 * in its bootloader. Its responses are not touch reports.
		 */
		gpiod_set_value_cansleep(ts->reset_gpio, 0);
		return ret;
	}

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = "FocalTech FT8722 Touchscreen";
	ts->input->id.bustype = BUS_SPI;
	input_set_capability(ts->input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, ts->max_x, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, ts->max_y, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 0xff, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 0xff, 0, 0);
	input_set_drvdata(ts->input, ts);
	__set_bit(INPUT_PROP_DIRECT, ts->input->propbit);

	ret = input_mt_init_slots(ts->input, FT8722_MAX_TOUCHES,
				  INPUT_MT_DIRECT);
	if (ret)
		return ret;

	touchscreen_parse_properties(ts->input, true, &ts->prop);

	ret = input_register_device(ts->input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register input device\n");

	irq_flags = irq_get_trigger_type(spi->irq);
	if (irq_flags == IRQ_TYPE_NONE)
		irq_flags = IRQF_TRIGGER_FALLING;
	irq_flags |= IRQF_ONESHOT;

	ret = devm_request_threaded_irq(dev, spi->irq, NULL,
					ft8722_irq_thread, irq_flags,
					dev_name(dev), ts);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ\n");

	spi_set_drvdata(spi, ts);
	dev_info(dev, "FT8722 touchscreen initialized\n");

	return 0;
}

static const struct of_device_id ft8722_of_match[] = {
	{ .compatible = "focaltech,ft8722" },
	{}
};
MODULE_DEVICE_TABLE(of, ft8722_of_match);

static struct spi_driver ft8722_driver = {
	.driver = {
		.name = "focaltech-ft8722",
		.of_match_table = ft8722_of_match,
	},
	.probe = ft8722_probe,
};
module_spi_driver(ft8722_driver);

MODULE_DESCRIPTION("FocalTech FT8722 SPI touchscreen driver");
MODULE_LICENSE("GPL");
