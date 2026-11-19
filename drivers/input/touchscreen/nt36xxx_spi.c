// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NT36XXX SPI Touchscreen Driver
 *
 * Copyright (C) 2020 - 2021 Goodix, Inc.
 * Copyright (C) 2023 Linaro Ltd.
 * Copyright (C) 2023-2024 George Chan <gchan9527@gmail.com>
 *
 * Based on goodix_ts_berlin driver.
 */
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/input/touchscreen.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#include "nt36xxx.h"

#define SPI_READ_PREFIX_LEN	1
#define SPI_WRITE_PREFIX_LEN	1

#define DEBUG 0

/* Stock CTP_SPI_READ()/CTP_SPI_WRITE() retry a failed transfer 5 times. */
#define NT36XXX_SPI_RETRIES	5

/*
 * there are two kinds of spi read/write:
 * 	(a)spi_read()/spi_write()/spi_write_then_read(),
 * 	(b)and the spi_sync itself.
 *
 * we have to choose one and stick together, cross-use otherwise caused problem.
 * the addressing mode is | 0xff 0xXX 0xYY | 0xZ1 ... data1...| 0xZ2 ...data2... | ...
 * 	0xXX is bit[23..16]
 * 	0xYY is bit[15..7]
 * above describe a 'page select' ops
 * 	0xZ1 is bit[7..0], addr for read ops
 *	0xZ2 is bit[7..0] | 0x80, addr for write ops
 * there is no restriction on the read write order.
*/
static int nt36xxx_spi_write(void *dev, const void *data,
                                   size_t len)
{
	struct spi_device *spi = to_spi_device((struct device *)dev);
	int32_t ret;
	u32 reg;
	u8 addr[4];
	void *data1;
	int retry;

	if (len < sizeof(reg))
		return -EINVAL;
	data1 = kmemdup(data, len, GFP_KERNEL | GFP_DMA);
	if (!data1)
		return -ENOMEM;

	reg = get_unaligned_le32(data);
	addr[0] = 0xff;
	addr[1] = reg >> 15;
	addr[2] = reg >> 7;
	addr[3] = (reg & 0x7f) | 0x80;
	memcpy(data1, addr, 4);

	dev_dbg(dev, "%s len=0x%lx", __func__, len);

	for (retry = 0; retry < NT36XXX_SPI_RETRIES; retry++) {
		ret = spi_write(spi, data1, 3);
		if (!ret)
			ret = spi_write(spi, data1 + 3, len - 3);
		if (!ret)
			break;
	}
	if (ret)
		dev_err(dev, "transfer err %d\n ", ret);
	else if (DEBUG) {

		print_hex_dump(KERN_INFO, __func__, DUMP_PREFIX_OFFSET,
			16, 1, data, 3, true);

		print_hex_dump(KERN_INFO, __func__, DUMP_PREFIX_OFFSET,
			16, 1, data + 3, (len - 3) > 0x20 ? 0x20 : len - 3 , true);
	}

	kfree(data1);
	return ret;
}

static int nt36xxx_spi_read(void *dev, const void *reg_buf,
                                  size_t reg_size, void *val_buf,
                                  size_t val_size)
{
	struct spi_device *spi = to_spi_device(dev);
	static unsigned int logged_large_reads;
	struct spi_transfer xfer[2] = { };
	int ret, retry;
	u32 reg;
	u8 addr[4];
	u8 *buf;

	if (reg_size != sizeof(reg))
		return -EINVAL;
	reg = get_unaligned_le32(reg_buf);
	addr[0] = 0xff;
	addr[1] = reg >> 15;
	addr[2] = reg >> 7;
	addr[3] = reg & 0x7f;

	/*
	 * NT36675 returns one dummy byte after the read command; the Samsung
	 * 4.14 SPI driver discards rbuf[0] (command) and rbuf[1] (dummy).
	 *
	 * The data transfer must start on a 4-byte boundary: the MediaTek SPI
	 * controller only uses DMA for longer transfers with aligned buffers,
	 * and its multi-round FIFO path returned stale bytes for 65-byte touch
	 * frames on a real A22x (small reads were correct). The Himax driver on
	 * the same controller reads full duplex from one aligned buffer, so do
	 * the same: buf[0] command, buf[4] dummy, buf[5..] data.
	 */
	if (val_size >= NT36XXX_TRANSFER_LEN + 8)
		return -EINVAL;
	buf = kmalloc(4 + 1 + val_size, GFP_KERNEL | GFP_DMA);
	if (!buf)
		return -ENOMEM;
	buf[0] = addr[3];
	xfer[0].tx_buf = buf;
	xfer[0].len = 1;
	xfer[1].tx_buf = buf + 4;
	xfer[1].rx_buf = buf + 4;
	xfer[1].len = 1 + val_size;

	for (retry = 0; retry < NT36XXX_SPI_RETRIES; retry++) {
		memset(buf + 4, 0, 1 + val_size);
		ret = spi_write(spi, addr, 3);
		if (!ret)
			ret = spi_sync_transfer(spi, xfer, ARRAY_SIZE(xfer));
		if (!ret)
			break;
	}
	if (ret) {
		dev_err(dev, "read 0x%06x failed: %d\n", reg, ret);
		kfree(buf);
		return ret;
	}
	memcpy(val_buf, buf + 5, val_size);

	/* Bring-up aid: the dummy byte and the head of the first long reads. */
	if (val_size > 32 && logged_large_reads < 3) {
		logged_large_reads++;
		dev_info(dev, "read 0x%06x len %zu dummy 0x%02x data %8ph\n",
			 reg, val_size, buf[4], buf + 5);
	}
	kfree(buf);

	if (DEBUG) {
		print_hex_dump(KERN_INFO, __func__, DUMP_PREFIX_OFFSET,
			16, 1, addr, 3, true);

		print_hex_dump(KERN_INFO, __func__, DUMP_PREFIX_OFFSET,
			16, 1, addr, (val_size) > 0x20 ? 0x20 : val_size % 0x20 , true);

		print_hex_dump(KERN_INFO, __func__, DUMP_PREFIX_OFFSET,
			16, 1, val_buf, (val_size > 0x20) ? 0x20 : val_size % 0x20 , true);
	}

	return ret;
}

static const struct regmap_config nt36xxx_regmap_config_32bit = {
	.name = "nt36xxx_hw",
	.reg_bits = 32,
	.val_bits = 8,
	.read = nt36xxx_spi_read,
	.write = nt36xxx_spi_write,

	.max_raw_read = NT36XXX_TRANSFER_LEN + 8,
	.max_raw_write = NT36XXX_TRANSFER_LEN + 8,

	.zero_flag_mask = true, /* this is needed to make sure addr is not write_masked */
	.cache_type = REGCACHE_NONE,
};

static const struct input_id nt36xxx_spi_input_id = {
	.bustype = BUS_SPI,
};

static int nt36xxx_spi_probe(struct spi_device *spi)
{
	struct regmap_config *regmap_config;
	struct regmap *regmap;
	size_t max_size;
	int ret = 0;

	dev_dbg(&spi->dev, "%s %d", __func__, __LINE__);

	regmap_config = devm_kmemdup(&spi->dev, &nt36xxx_regmap_config_32bit,
				     sizeof(*regmap_config), GFP_KERNEL);
	if (!regmap_config) {
		dev_err(&spi->dev, "memdup regmap_config fail\n");
		return -ENOMEM;
	}

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret) {
		dev_err(&spi->dev, "SPI setup error %d\n", ret);
		return ret;
	}

	/* don't exceed max specified SPI CLK frequency */
	if (spi->max_speed_hz > MAX_SPI_FREQ_HZ) {
		dev_err(&spi->dev, "SPI CLK %d Hz?\n", spi->max_speed_hz);
		return -EINVAL;
	}

	max_size = spi_max_transfer_size(spi);
	if (max_size <= SPI_READ_PREFIX_LEN)
		return -EINVAL;
	regmap_config->max_raw_read = min_t(size_t, max_size - SPI_READ_PREFIX_LEN,
					   NT36XXX_TRANSFER_LEN);
	regmap_config->max_raw_write = min_t(size_t, max_size - SPI_WRITE_PREFIX_LEN,
					    NT36XXX_TRANSFER_LEN);

	regmap = devm_regmap_init(&spi->dev, NULL, spi, regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return nt36xxx_probe(&spi->dev, spi->irq,
				   &nt36xxx_spi_input_id, regmap);
}

static const struct nt36xxx_chip_data default_config = {
	.config = &nt36xxx_regmap_config_32bit,
	.mmap = nt36676f_memory_maps,
	.max_x = 1080,
	.max_y = 2400,
	.abs_x_max = 1080,
	.abs_y_max = 2400,
	.id = &nt36xxx_spi_input_id,
};

static const struct nt36xxx_chip_data miatoll_tianma_nt36675 = {
	.config = &nt36xxx_regmap_config_32bit,
	.mmap = nt36675_memory_maps,
	.fw_name = "novatek_ts_tianma_fw.bin",
	.max_x = 1080,
	.max_y = 2400,
	.abs_x_max = 1080,
	.abs_y_max = 2400,
	.id = &nt36xxx_spi_input_id,
};

/*
 * A22 5G Tianma NT36672C TDDI: trim ID 0c:00:00:72:66:03 selects the NT36675
 * memory map with hardware CRC in the stock driver. Stock DT and the
 * reporter's probe log use irq flags 0x2001 (rising edge | oneshot); the
 * shared DT node carries Himax's falling-edge trigger.
 */
static const struct nt36xxx_chip_data samsung_a22x_nt36675 = {
	.config = &nt36xxx_regmap_config_32bit,
	.mmap = nt36675_memory_maps,
	.fw_name = "novatek_ts_fw.bin",
	.max_x = 1080,
	.max_y = 2408,
	.abs_x_max = 1080,
	.abs_y_max = 2408,
	.id = &nt36xxx_spi_input_id,
	.chip = NT36675_IC,
	.irq_trigger = IRQF_TRIGGER_RISING,
};

static const struct nt36xxx_chip_data generic_nt36676f = {
	.config = &nt36xxx_regmap_config_32bit,
	.mmap = nt36676f_memory_maps,
	.max_x = 1080,
	.max_y = 2400,
	.abs_x_max = 1080,
	.abs_y_max = 2400,
	.id = &nt36xxx_spi_input_id,
};

static const struct nt36xxx_chip_data generic_nt36772 = {
	.config = &nt36xxx_regmap_config_32bit,
	.mmap = nt36772_memory_maps,
	.max_x = 1080,
	.max_y = 2400,
	.abs_x_max = 1080,
	.abs_y_max = 2400,
	.id = &nt36xxx_spi_input_id,
};

static const struct nt36xxx_chip_data generic_nt36525 = {
	.config = &nt36xxx_regmap_config_32bit,
	.mmap = nt36525_memory_maps,
	.max_x = 1080,
	.max_y = 2400,
	.abs_x_max = 1080,
	.abs_y_max = 2400,
	.id = &nt36xxx_spi_input_id,
};

static const struct spi_device_id nt36xxx_spi_ids[] = {
	{ "NVT-ts-spi", 0 },
	{ "nt36675-spi", 0 },
	{ "nt36672a-spi", 0 },
	{ "nt36676f-spi", 0 },
	{ "nt36772-spi", 0 },
	{ "nt36525-spi", 0 },
	{ "NVT-default-spi", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, nt36xxx_spi_ids);

static const struct of_device_id nt36xxx_spi_of_match[] = {
	{ .compatible = "novatek,NVT-ts-spi", .data = &samsung_a22x_nt36675, },
	{ .compatible = "novatek,nt36675-spi", .data = &miatoll_tianma_nt36675, },
	{ .compatible = "novatek,nt36672a-spi", .data = &miatoll_tianma_nt36675, },
	{ .compatible = "novatek,nt36676f-spi", .data = &generic_nt36676f, },
	{ .compatible = "novatek,nt36772-spi", .data = &generic_nt36772, },
	{ .compatible = "novatek,nt36525-spi", .data = &generic_nt36525, },
	/*
	 * this is served for two special purpose.
	 * (1) detect/display model only, and bail out in the end
	 * (2) checking device varients, mixed use of novatek and focaltech spi ic
	 * TODO: might add auto select mmap for unknown nvt device.
	 */
	{ .compatible = "novatek,NVT-default-spi", .data = &default_config, },
	{ }
};
MODULE_DEVICE_TABLE(of, nt36xxx_spi_of_match);

static struct spi_driver nt36xxx_spi_driver = {
	.driver = {
		.name   = "nt36675-spi",
		.of_match_table = nt36xxx_spi_of_match,
		.pm = pm_sleep_ptr(&nt36xxx_pm_ops),
	},
	.probe = nt36xxx_spi_probe,
	.id_table = nt36xxx_spi_ids,
};
module_spi_driver(nt36xxx_spi_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NT36XXX SPI Touchscreen driver");
MODULE_AUTHOR("Neil Armstrong <neil.armstrong@linaro.org>");
MODULE_AUTHOR("George Chan <gchan9527@gmail.com>");
