// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Novatek NT36xxx series touchscreens
 *
 * Copyright (C) 2010 - 2018 Novatek, Inc.
 * Copyright (C) 2020 XiaoMi, Inc.
 * Copyright (C) 2020 AngeloGioacchino Del Regno <kholk11@gmail.com>
 * Copyright (C) 2023-2025 George Chan <gchan9527@gmail.com>
 *
 * Based on nt36xxx.c i2c driver from AngeloGioacchino Del Regno
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/devm-helpers.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/irqnr.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/printk.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>
#include <drm/drm_panel.h>

#include "nt36xxx.h"

/* Remote bring-up aid: log the raw bytes of the first N touch frames. */
static unsigned int debug_frames;
module_param(debug_frames, uint, 0644);
MODULE_PARM_DESC(debug_frames, "log the raw bytes of the first N touch frames");

/* Main mmap to spi addr */
enum {
	MMAP_BASELINE_ADDR,
	MMAP_BASELINE_BTN_ADDR,
	MMAP_BLD_CRC_EN_ADDR,
	MMAP_BLD_DES_ADDR,
	MMAP_BLD_ILM_DLM_CRC_ADDR,
	MMAP_BLD_LENGTH_ADDR,
	MMAP_BOOT_RDY_ADDR,
	MMAP_DIFF_BTN_PIPE0_ADDR,
	MMAP_DIFF_BTN_PIPE1_ADDR,
	MMAP_DIFF_PIPE0_ADDR,
	MMAP_DIFF_PIPE1_ADDR,
	MMAP_DLM_DES_ADDR,
	MMAP_DLM_LENGTH_ADDR,
	MMAP_DMA_CRC_EN_ADDR,
	MMAP_DMA_CRC_FLAG_ADDR,
	MMAP_ENG_RST_ADDR,
	MMAP_EVENT_BUF_ADDR,
	MMAP_G_DLM_CHECKSUM_ADDR,
	MMAP_G_ILM_CHECKSUM_ADDR,
	MMAP_ILM_DES_ADDR,
	MMAP_ILM_LENGTH_ADDR,
	MMAP_POR_CD_ADDR,
	MMAP_RAW_BTN_PIPE0_ADDR,
	MMAP_RAW_BTN_PIPE1_ADDR,
	MMAP_RAW_PIPE0_ADDR,
	MMAP_RAW_PIPE1_ADDR,
	MMAP_READ_FLASH_CHECKSUM_ADDR,
	MMAP_RW_FLASH_DATA_ADDR,
	MMAP_R_DLM_CHECKSUM_ADDR,
	MMAP_R_ILM_CHECKSUM_ADDR,
	MMAP_SPI_RD_FAST_ADDR,
	MMAP_SWRST_N8_ADDR,

	/* below are magic numbers in source code */
	MMAP_MAGIC_NUMBER_0X1F64E_ADDR,

	/* this addr is not specific to */
	MMAP_TOP_ADDR,
	MMAP_MAX_ADDR = MMAP_TOP_ADDR,
} nt36xxx_ts_mem_map;

static struct drm_panel_follower_funcs nt36xxx_panel_follower_funcs;

struct nt36xxx_ts {
	struct regmap *regmap;

	struct input_dev *input;
	struct regulator_bulk_data *supplies;
	bool supplies_acquired;
	struct gpio_desc *reset_gpio;
        struct gpio_desc *irq_gpio;
	int irq;
	struct device *dev;

	struct mutex lock;

#define NT36XXX_STATUS_SUSPEND			BIT(0)
#define NT36XXX_STATUS_DOWNLOAD_COMPLETE	BIT(1)
#define NT36XXX_STATUS_DOWNLOAD_RECOVER		BIT(2)
#define NT36XXX_STATUS_PREPARE_FIRMWARE		BIT(3)
#define NT36XXX_STATUS_NEED_FIRMWARE      	BIT(4)
/* Asleep with the panel, watching for a double tap (wake_gesture). */
#define NT36XXX_STATUS_GESTURE			BIT(5)

/*
 * Vendor AOT mode: while the panel sleeps with its rails on, host command 0x13 puts
 * the touch part into gesture mode instead of deep sleep (0x11), and a double tap
 * (gesture 15, or 15 in a data-protocol frame of the gesture page) reports KEY_WAKEUP.
 * Only on the panel-follower path, where the panel driver keeps the rails on.
 */
#define NT36XXX_CMD_ENTER_GESTURE		0x13
#define NT36XXX_GESTURE_DOUBLE_TAP		15
#define NT36XXX_GESTURE_DATA_PROTOCOL		30
#define NT36XXX_GESTURE_FUNCPAGE		0x01

	unsigned int status;

	struct touchscreen_properties prop;
	struct nt36xxx_fw_info fw_info;
	struct nt36xxx_abs_object abs_obj;

	struct drm_panel_follower panel_follower;

	struct delayed_work work;
	unsigned int download_attempts;
	unsigned int wdt_frames;
	unsigned int frames_logged;
	bool contact_seen;
	/* Touch-frame read: stock shape first, page select as the fallback. */
	bool frame_page_select;
	unsigned int bad_frames;
	unsigned int frame_mode_switches;
	bool diag_dumped;

	/* this is a duplicate with nt36xxx_chip_data and since the address might
	 * change in boot/init/download stages so make it a copy of initial map and
	 * update accordingly
	 */
	u32 *mmap;
	u32 mmap_data[MMAP_MAX_ADDR];

	struct nvt_fw_parse_data fw_data;
	struct nvt_ts_bin_map *bin_map;

	uint8_t hw_crc;

	const char * fw_name;
	struct firmware fw_entry; /* containing request fw data */
	const struct nt36xxx_chip_data *data;
};

static bool wake_gesture = true;
module_param(wake_gesture, bool, 0644);
MODULE_PARM_DESC(wake_gesture, "A double tap wakes the screen while the panel sleeps (panel follower only)");

static const struct nt36xxx_trim_table trim_id_table[] = {
	/* TODO: port and test all related module */
	{
		/* SM-A226B reporter's successful stock NVT-ts probe. */
		.id = { 0x0C, 0xFF, 0xFF, 0x72, 0x66, 0x03 },
		.mask = { 1, 0, 0, 1, 1, 1 },
		.mapid = NT36675_IC,
		.hw_crc = 2,
	},
	{
		.id = { 0x0A, 0xFF, 0xFF, 0x72, 0x66, 0x03 },
		.mask = { 1, 0, 0, 1, 1, 1 },
		.mapid = NT36672A_IC,
	},
	{
		.id = { 0x55, 0x00, 0xFF, 0x00, 0x00, 0x00 },
		.mask = { 1, 1, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0x55, 0x72, 0xFF, 0x00, 0x00, 0x00 },
		.mask = { 1, 1, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xAA, 0x00, 0xFF, 0x00, 0x00, 0x00 },
		.mask = { 1, 1, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xAA, 0x72, 0xFF, 0x00, 0x00, 0x00 },
		.mask = { 1, 1, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x72, 0x67, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x70, 0x66, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x70, 0x67, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x72, 0x66, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x25, 0x65, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x70, 0x68, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36772_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x76, 0x66, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36676F_IC,
	},
	{
		.id = { 0xFF, 0xFF, 0xFF, 0x75, 0x66, 0x03},
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mapid = NT36675_IC,
		.hw_crc = 2,
	},
	{ },
};

const u32 nt36675_memory_maps[] = {
	[MMAP_EVENT_BUF_ADDR] = 0x22D00,
	[MMAP_RAW_PIPE0_ADDR] = 0x24000,
	[MMAP_RAW_PIPE1_ADDR] = 0x24000,
	[MMAP_BASELINE_ADDR] = 0x21B90,
	[MMAP_DIFF_PIPE0_ADDR] = 0x20C60,
	[MMAP_DIFF_PIPE1_ADDR] = 0x24C60,
	[MMAP_READ_FLASH_CHECKSUM_ADDR] = 0x24000,
	[MMAP_RW_FLASH_DATA_ADDR] = 0x24002,
	[MMAP_BOOT_RDY_ADDR] = 0x3F10D,
	[MMAP_BLD_LENGTH_ADDR] = 0x3F138,
	[MMAP_ILM_LENGTH_ADDR] = 0x3F118,
	[MMAP_DLM_LENGTH_ADDR] = 0x3F130,
	[MMAP_BLD_DES_ADDR] = 0x3F114,
	[MMAP_ILM_DES_ADDR] = 0x3F128,
	[MMAP_DLM_DES_ADDR] = 0x3F12C,
	[MMAP_G_ILM_CHECKSUM_ADDR] = 0x3F100,
	[MMAP_G_DLM_CHECKSUM_ADDR] = 0x3F104,
	[MMAP_R_ILM_CHECKSUM_ADDR] = 0x3F120,
	[MMAP_R_DLM_CHECKSUM_ADDR] = 0x3F124,
	[MMAP_BLD_CRC_EN_ADDR] = 0x3F30E,
	[MMAP_DMA_CRC_EN_ADDR] = 0x3F136,
	[MMAP_BLD_ILM_DLM_CRC_ADDR] = 0x3F133,
	[MMAP_DMA_CRC_FLAG_ADDR] = 0x3F134,

	/* below are specified by dts, so it might change by project-based */
	[MMAP_SPI_RD_FAST_ADDR] = 0x03F310,
	[MMAP_SWRST_N8_ADDR] = 0x03F0FE,

	[MMAP_ENG_RST_ADDR] = 0x7FFF80,
	[MMAP_MAGIC_NUMBER_0X1F64E_ADDR] = 0x1F64E,

	[MMAP_TOP_ADDR] = 0xffffff,
};

const u32 nt36672a_memory_maps[] = {
	[MMAP_EVENT_BUF_ADDR] = 0x21C00,
	[MMAP_RAW_PIPE0_ADDR] = 0x20000,
	[MMAP_RAW_PIPE1_ADDR] = 0x23000,
	[MMAP_BASELINE_ADDR] = 0x20BFC,
	[MMAP_BASELINE_BTN_ADDR] = 0x23BFC,
	[MMAP_DIFF_PIPE0_ADDR] = 0x206DC,
	[MMAP_DIFF_PIPE1_ADDR] = 0x236DC,
	[MMAP_RAW_BTN_PIPE0_ADDR] = 0x20510,
	[MMAP_RAW_BTN_PIPE1_ADDR] = 0x23510,
	[MMAP_DIFF_BTN_PIPE0_ADDR] = 0x20BF0,
	[MMAP_DIFF_BTN_PIPE1_ADDR] = 0x23BF0,
	[MMAP_READ_FLASH_CHECKSUM_ADDR] = 0x24000,
	[MMAP_RW_FLASH_DATA_ADDR] = 0x24002,
	/* Phase 2 Host Download */
	[MMAP_BOOT_RDY_ADDR] = 0x3F10D,
	/* BLD CRC */
	[MMAP_BLD_LENGTH_ADDR] = 0x3F10E,    //0x3F10E ~ 0x3F10F     (2 bytes)
	[MMAP_ILM_LENGTH_ADDR] = 0x3F118,    //0x3F118 ~ 0x3F119     (2 bytes)
	[MMAP_DLM_LENGTH_ADDR] = 0x3F130,    //0x3F130 ~ 0x3F131     (2 bytes)
	[MMAP_BLD_DES_ADDR] = 0x3F114,    //0x3F114 ~ 0x3F116     (3 bytes)
	[MMAP_ILM_DES_ADDR] = 0x3F128,    //0x3F128 ~ 0x3F12A     (3 bytes)
	[MMAP_DLM_DES_ADDR] = 0x3F12C,    //0x3F12C ~ 0x3F12E     (3 bytes)
	[MMAP_G_ILM_CHECKSUM_ADDR] = 0x3F100,    //0x3F100 ~ 0x3F103     (4 bytes)
	[MMAP_G_DLM_CHECKSUM_ADDR] = 0x3F104,    //0x3F104 ~ 0x3F107     (4 bytes)
	[MMAP_R_ILM_CHECKSUM_ADDR] = 0x3F120,    //0x3F120 ~ 0x3F123 (4 bytes)
	[MMAP_R_DLM_CHECKSUM_ADDR] = 0x3F124,    //0x3F124 ~ 0x3F127 (4 bytes)
	[MMAP_BLD_CRC_EN_ADDR] = 0x3F30E,
	[MMAP_DMA_CRC_EN_ADDR] = 0x3F132,
	[MMAP_BLD_ILM_DLM_CRC_ADDR] = 0x3F133,
	[MMAP_DMA_CRC_FLAG_ADDR] = 0x3F134,

	/* below are specified by dts, so it might change by project-based */
	[MMAP_SPI_RD_FAST_ADDR] = 0x03F310,
	[MMAP_SWRST_N8_ADDR] = 0x03F0FE,

	[MMAP_ENG_RST_ADDR] = 0x7FFF80,
	[MMAP_MAGIC_NUMBER_0X1F64E_ADDR] = 0x1F64E,

	[MMAP_TOP_ADDR] = 0xffffff,
};

const u32 nt36676f_memory_maps[] = {
	[MMAP_EVENT_BUF_ADDR] = 0x11A00,
	[MMAP_RAW_PIPE0_ADDR] = 0x10000,
	[MMAP_RAW_PIPE1_ADDR] = 0x12000,
	[MMAP_BASELINE_ADDR] = 0x10B08,
	[MMAP_BASELINE_BTN_ADDR] = 0x12B08,
	[MMAP_DIFF_PIPE0_ADDR] = 0x1064C,
	[MMAP_DIFF_PIPE1_ADDR] = 0x1264C,
	[MMAP_RAW_BTN_PIPE0_ADDR] = 0x10634,
	[MMAP_RAW_BTN_PIPE1_ADDR] = 0x12634,
	[MMAP_DIFF_BTN_PIPE0_ADDR] = 0x10AFC,
	[MMAP_DIFF_BTN_PIPE1_ADDR] = 0x12AFC,
	[MMAP_READ_FLASH_CHECKSUM_ADDR] = 0x14000,
	[MMAP_RW_FLASH_DATA_ADDR] = 0x14002,

	/* below are specified by dts, so it might change by project-based */
	[MMAP_SPI_RD_FAST_ADDR] = 0x03F310,
	[MMAP_SWRST_N8_ADDR] = 0x03F0FE,

	[MMAP_ENG_RST_ADDR] = 0x7FFF80,
	[MMAP_MAGIC_NUMBER_0X1F64E_ADDR] = 0x1F64E,

	[MMAP_TOP_ADDR] = 0xffffff,
};

const u32 nt36772_memory_maps[] = {
	[MMAP_EVENT_BUF_ADDR] = 0x11E00,
	[MMAP_RAW_PIPE0_ADDR] = 0x10000,
	[MMAP_RAW_PIPE1_ADDR] = 0x12000,
	[MMAP_BASELINE_ADDR] = 0x10E70,
	[MMAP_BASELINE_BTN_ADDR] = 0x12E70,
	[MMAP_DIFF_PIPE0_ADDR] = 0x10830,
	[MMAP_DIFF_PIPE1_ADDR] = 0x12830,
	[MMAP_RAW_BTN_PIPE0_ADDR] = 0x10E60,
	[MMAP_RAW_BTN_PIPE1_ADDR] = 0x12E60,
	[MMAP_DIFF_BTN_PIPE0_ADDR] = 0x10E68,
	[MMAP_DIFF_BTN_PIPE1_ADDR] = 0x12E68,
	[MMAP_READ_FLASH_CHECKSUM_ADDR] = 0x14000,
	[MMAP_RW_FLASH_DATA_ADDR] = 0x14002,
	/* Phase 2 Host Download */
	[MMAP_BOOT_RDY_ADDR] = 0x1F141,
	[MMAP_POR_CD_ADDR] = 0x1F61C,
	/* BLD CRC */
	[MMAP_R_ILM_CHECKSUM_ADDR] = 0x1BF00,

	/* below are specified by dts, so it might change by project-based */
	[MMAP_SPI_RD_FAST_ADDR] = 0x03F310,
	[MMAP_SWRST_N8_ADDR] = 0x03F0FE,

	[MMAP_ENG_RST_ADDR] = 0x7FFF80,
	[MMAP_MAGIC_NUMBER_0X1F64E_ADDR] = 0x1F64E,

	[MMAP_TOP_ADDR] = 0xffffff,
};

const u32 nt36525_memory_maps[] = {
	[MMAP_EVENT_BUF_ADDR] = 0x11A00,
	[MMAP_RAW_PIPE0_ADDR] = 0x10000,
	[MMAP_RAW_PIPE1_ADDR] = 0x12000,
	[MMAP_BASELINE_ADDR] = 0x10B08,
	[MMAP_BASELINE_BTN_ADDR] = 0x12B08,
	[MMAP_DIFF_PIPE0_ADDR] = 0x1064C,
	[MMAP_DIFF_PIPE1_ADDR] = 0x1264C,
	[MMAP_RAW_BTN_PIPE0_ADDR] = 0x10634,
	[MMAP_RAW_BTN_PIPE1_ADDR] = 0x12634,
	[MMAP_DIFF_BTN_PIPE0_ADDR] = 0x10AFC,
	[MMAP_DIFF_BTN_PIPE1_ADDR] = 0x12AFC,
	[MMAP_READ_FLASH_CHECKSUM_ADDR] = 0x14000,
	[MMAP_RW_FLASH_DATA_ADDR] = 0x14002,

	/* Phase 2 Host Download */
	[MMAP_BOOT_RDY_ADDR] = 0x1F141,
	[MMAP_POR_CD_ADDR] = 0x1F61C,
	/* BLD CRC */
	[MMAP_R_ILM_CHECKSUM_ADDR] = 0x1BF00,

	/* below are specified by dts, so it might change by project-based */
	[MMAP_SPI_RD_FAST_ADDR] = 0x03F310,
	[MMAP_SWRST_N8_ADDR] = 0x03F0FE,

	[MMAP_ENG_RST_ADDR] = 0x7FFF80,
	[MMAP_MAGIC_NUMBER_0X1F64E_ADDR] = 0x1F64E,

	[MMAP_TOP_ADDR] = 0xffffff,
};

static void __maybe_unused _debug_irq(struct nt36xxx_ts *ts, int line)
{
        struct irq_desc *desc;
        desc = irq_data_to_desc( irq_get_irq_data(ts->irq));
        dev_info(ts->dev, "%d irq_desc depth=%d", line, desc->depth );
}

#define debug_irq(a) _debug_irq(a, __LINE__)

static int nt36xxx_eng_reset_idle(struct nt36xxx_ts *ts)
{
	int ret;

	if (ts->status & NT36XXX_STATUS_SUSPEND)
		return -EIO;

	if(!ts->mmap) {
		dev_err(ts->dev, "%s %s empty", __func__, "ts->mmap");
		return -EINVAL;
	}

	if(ts->mmap[MMAP_ENG_RST_ADDR] == 0) {
		dev_err(ts->dev, "%s %s empty", __func__, "MMAP_ENG_RST_ADDR");
		return -EINVAL;
	}

	/* HACK to output something without read */
	ret = regmap_write(ts->regmap, ts->mmap[MMAP_ENG_RST_ADDR],
					   0x5a);
	if (ret) {
		dev_err(ts->dev, "%s regmap write error\n", __func__);
		return ret;
	}

	/* Wait until the MCU resets the fw state */
	usleep_range(15000, 16000);

	/* seemed not long enough */
	msleep(30);
	return ret;
}

/*
 * nt36xxx_bootloader_reset - Reset MCU to bootloader
 * @ts: Main driver structure
 *
 * Return: Always zero for success, negative number for error
 */
static int nt36xxx_bootloader_reset(struct nt36xxx_ts *ts)
{
	int ret = 0;

	if (ts->status & NT36XXX_STATUS_SUSPEND)
		return -EIO;

	//in spi version, need to set page to SWRST_N8_ADDR
	if (ts->mmap[MMAP_SWRST_N8_ADDR]) {
		ret = regmap_write(ts->regmap, ts->mmap[MMAP_SWRST_N8_ADDR],
			   NT36XXX_CMD_BOOTLOADER_RESET);
	        if (ret)
			return ret;
	} else {
		pr_info("plz make sure MMAP_SWRST_N8_ADDR is set!\n");
		return -EINVAL;
	}

	/* MCU has to reboot from bootloader: this is the typical boot time */
	msleep(35);

	if (ts->mmap[MMAP_SPI_RD_FAST_ADDR]) {
                ret = regmap_write(ts->regmap, ts->mmap[MMAP_SPI_RD_FAST_ADDR], 0);
                if (ret)
                        return ret;
	}

	return ret;
}

/**
 * nt36xxx_check_reset_state - Check the boot state during reset
 * @ts: Main driver structure
 * @fw_state: Enumeration containing firmware states
 *
 * Return: Always zero for success, negative number for error
 */
static int nt36xxx_check_reset_state(struct nt36xxx_ts *ts,
				     enum nt36xxx_fw_state fw_state)
{
	u8 state = 0;
	int ret = 0, retry = NT36XXX_MAX_FW_RST_RETRY;

	do {
		if (ts->status & NT36XXX_STATUS_SUSPEND)
			return -EIO;

		/* data[0] is the byte at EVENT_BUF + 0x60 (vendor buf[1]). */
		ret = regmap_raw_read(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR]
				 | NT36XXX_EVT_RESET_COMPLETE, &state, 1);
		if (!ret && state >= fw_state && state <= NT36XXX_STATE_MAX)
			return 0;
		usleep_range(10000, 11000);
	} while (--retry);

	dev_err(ts->dev, "Firmware reset failed: state 0x%02x, ret %d\n",
		state, ret);
	return -EBUSY;
}

static bool nt36xxx_bytes_all(const u8 *buf, size_t len, u8 a, u8 b)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (buf[i] != a && buf[i] != b)
			return false;
	return true;
}

/* Vendor nvt_ts_point_data_checksum(): two's complement of data[0..63]. */
static bool nt36xxx_point_checksum_ok(const u8 *data)
{
	u8 sum = 0;
	int i;

	for (i = 0; i < POINT_DATA_CHECKSUM_LEN - 1; i++)
		sum += data[i];
	return (u8)(~sum + 1) == data[POINT_DATA_CHECKSUM_LEN - 1];
}

/* Runs in the IRQ thread with the line masked: nothing else uses the bus. */
static void nt36xxx_diag_dump(struct nt36xxx_ts *ts)
{
	u8 buf[128];
	int half, i;

	dev_warn(ts->dev, "no valid touch frame in either read mode; event page dump:\n");
	for (half = 0; half < 2; half++) {
		u32 addr = ts->mmap[MMAP_EVENT_BUF_ADDR] + 128 * half;

		if (regmap_raw_read(ts->regmap, addr, buf, sizeof(buf)))
			break;
		for (i = 0; i < sizeof(buf); i += 16)
			dev_warn(ts->dev, "diag %05x: %16ph\n", addr + i, buf + i);
	}
	if (!regmap_raw_read(ts->regmap, NT36XXX_CHIP_VER_TRIM_ADDR & ~0x7f, buf, 16))
		dev_warn(ts->dev, "diag %05x: %16ph\n",
			 NT36XXX_CHIP_VER_TRIM_ADDR & ~0x7f, buf);
	nt36xxx_bus_set_page(ts->dev, ts->mmap[MMAP_EVENT_BUF_ADDR]);
}

/* A frame failed its checksum: after a run of them try the other read mode. */
static void nt36xxx_bad_frame(struct nt36xxx_ts *ts)
{
	dev_warn_ratelimited(ts->dev, "touch frame checksum mismatch\n");
	if (++ts->bad_frames < NT36XXX_BAD_FRAMES_SWITCH)
		return;
	ts->bad_frames = 0;
	if (ts->frame_mode_switches >= 2) {
		if (!ts->diag_dumped) {
			ts->diag_dumped = true;
			nt36xxx_diag_dump(ts);
		}
		return;
	}
	ts->frame_mode_switches++;
	ts->frame_page_select = !ts->frame_page_select;
	ts->frames_logged = 0;
	dev_warn(ts->dev, "%u bad frames: reading frames %s a page select now\n",
		 NT36XXX_BAD_FRAMES_SWITCH, ts->frame_page_select ? "with" : "without");
	nt36xxx_bus_set_page(ts->dev, ts->mmap[MMAP_EVENT_BUF_ADDR]);
}

/* Read one touch frame and report its contacts. */
/*
 * Stock nvt_ts_work_func(): no page select, one transfer, 108 data bytes from
 * offset 0 of the page left at EVENT_BUF. On a real A22x a page select before
 * each frame returned a static block instead.
 */
static int nt36xxx_read_frame(struct nt36xxx_ts *ts, u8 *point, size_t length)
{
	if (ts->frame_page_select)
		return regmap_raw_read(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR],
				       point, length);
	return nt36xxx_bus_read_nopage(ts->dev, 0, point, length);
}

/* Vendor nvt_ts_wakeup_gesture_report(), for the double tap only. */
static void nt36xxx_gesture(struct nt36xxx_ts *ts)
{
	u8 point[NT36XXX_FRAME_READ_LEN];
	u8 id;

	if (nt36xxx_read_frame(ts, point, sizeof(point)) < 0)
		return;
	/* point[0] is the stock buffer's byte 1: the gesture id in its high five bits. */
	id = point[0] >> 3;
	if (id == NT36XXX_GESTURE_DATA_PROTOCOL && point[1] == NT36XXX_GESTURE_FUNCPAGE)
		id = point[2];
	if (id != NT36XXX_GESTURE_DOUBLE_TAP)
		return;
	dev_info(ts->dev, "double tap: wake\n");
	input_report_key(ts->input, KEY_WAKEUP, 1);
	input_sync(ts->input);
	input_report_key(ts->input, KEY_WAKEUP, 0);
	input_sync(ts->input);
}

static void nt36xxx_report(struct nt36xxx_ts *ts)
{
	struct nt36xxx_abs_object *obj = &ts->abs_obj;
	struct input_dev *input = ts->input;
	u8 point[NT36XXX_FRAME_READ_LEN];
	int i, ret;

	if (ts->status & NT36XXX_STATUS_SUSPEND)
		return;

	ret = nt36xxx_read_frame(ts, point, sizeof(point));
	if (ret < 0) {
		dev_err_ratelimited(ts->dev,
				    "Cannot read touch point data: %d\n", ret);
		return;
	}

	if (ts->frames_logged < debug_frames) {
		ts->frames_logged++;
		dev_info(ts->dev, "frame %u: %32ph\n", ts->frames_logged, point);
		dev_info(ts->dev, "frame %u: %33ph\n", ts->frames_logged, point + 32);
	}

	/*
	 * Vendor nvt_wdt_fw_recovery(): data[0..5] all 0xFD/0xFE. The firmware
	 * is reloaded only after more than NT36XXX_WDT_FRAMES consecutive
	 * watchdog frames; a non-watchdog frame resets the count.
	 */
	if (nt36xxx_bytes_all(point, 6, 0xFD, 0xFE)) {
		if (++ts->wdt_frames > NT36XXX_WDT_FRAMES) {
			ts->wdt_frames = 0;
			mutex_lock(&ts->lock);
			ts->status |= NT36XXX_STATUS_DOWNLOAD_RECOVER;
			mutex_unlock(&ts->lock);
		}
		return;
	}
	ts->wdt_frames = 0;

	/* ESD pattern: the stock driver drops it without other action. */
	if (nt36xxx_bytes_all(point, 6, 0x77, 0x77))
		return;

	if (!nt36xxx_point_checksum_ok(point)) {
		nt36xxx_bad_frame(ts);
		return;
	}
	ts->bad_frames = 0;

	for (i = 0; i < TOUCH_MAX_FINGER_NUM; i++) {
		const u8 *p = &point[6 * i];
		u8 input_id = p[0] >> 3;
		u8 status = p[0] & 0x07;
		int tool;

		if (input_id == 0 || input_id > TOUCH_MAX_FINGER_NUM)
			continue;

		/* 1 enter, 2 move, 6 glove; 5 palm and 7 metal jig are
		 * contacts the stock driver flags through ABS_MT_CUSTOM. */
		switch (status) {
		case 0x01:
		case 0x02:
		case 0x06:
			tool = MT_TOOL_FINGER;
			break;
		case 0x05:
		case 0x07:
			tool = MT_TOOL_PALM;
			break;
		default:
			continue;
		}

		obj->x = (p[1] << 4) | (p[3] >> 4);
		obj->y = (p[2] << 4) | (p[3] & 0x0f);
		if (obj->x > ts->prop.max_x || obj->y > ts->prop.max_y)
			continue;

		obj->tm = p[4] ? p[4] : 1;

		obj->z = p[5];
		if (i < 2)
			obj->z += point[61 + i] << 8;
		if (obj->z > TOUCH_MAX_PRESSURE)
			obj->z = TOUCH_MAX_PRESSURE;
		if (obj->z == 0)
			obj->z = 1;

		input_mt_slot(input, input_id - 1);
		input_mt_report_slot_state(input, tool, true);
		touchscreen_report_pos(input, &ts->prop, obj->x, obj->y, true);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, obj->tm);
		input_report_abs(input, ABS_MT_PRESSURE, obj->z);

		if (!ts->contact_seen) {
			ts->contact_seen = true;
			dev_info(ts->dev, "first contact: id %u status %u at %u,%u\n",
				 input_id, status, obj->x, obj->y);
		}
	}

	input_mt_sync_frame(input);
	input_sync(input);
}

static irqreturn_t nt36xxx_irq_handler(int irq, void *dev_id)
{
	struct nt36xxx_ts *ts = dev_id;

	if (!ts->mmap)
		goto exit;

	if (ts->status & NT36XXX_STATUS_SUSPEND) {
		if (ts->status & NT36XXX_STATUS_GESTURE)
			nt36xxx_gesture(ts);
		goto exit;
	}

	/* IRQF_ONESHOT keeps the line masked until this thread returns. */
	nt36xxx_report(ts);

exit:
	if (ts->status & NT36XXX_STATUS_DOWNLOAD_RECOVER) {
		/* The completed-download flag would otherwise make the work
		 * return without reloading the firmware. */
		mutex_lock(&ts->lock);
		ts->status &= ~(NT36XXX_STATUS_DOWNLOAD_RECOVER |
				NT36XXX_STATUS_DOWNLOAD_COMPLETE);
		ts->download_attempts = 0;
		mutex_unlock(&ts->lock);
		if (ts->fw_name) {
			dev_warn(ts->dev, "watchdog frames: reloading firmware\n");
			schedule_delayed_work(&ts->work, 0);
		}
	}

	return IRQ_HANDLED;
}


static const struct nt36xxx_trim_table *nt36xxx_match_trim(const u8 *id)
{
	const struct nt36xxx_trim_table *t;
	int i;

	for (t = trim_id_table; t->mapid; t++) {
		for (i = 0; i < NT36XXX_ID_LEN_MAX; i++)
			if (t->mask[i] && id[i] != t->id[i])
				break;
		if (i == NT36XXX_ID_LEN_MAX)
			return t;
	}
	return NULL;
}

/**
 * nt36xxx_chip_version_init - Detect Novatek NT36xxx family IC
 * @ts: Main driver structure
 *
 * Vendor check_chip_ver_trim(): bootloader reset before every attempt, five
 * attempts at the current trim address, then five at the old one. The bytes
 * returned by regmap start at the trim address (vendor buf[1..6]).
 *
 * Return: zero when the expected chip was identified, negative errno otherwise
 */
static int nt36xxx_chip_version_init(struct nt36xxx_ts *ts)
{
	static const u32 trim_addrs[] = {
		NT36XXX_CHIP_VER_TRIM_ADDR,
		NT36XXX_CHIP_VER_TRIM_OLD_ADDR,
	};
	const struct nt36xxx_trim_table *t;
	u8 id[NT36XXX_ID_LEN_MAX];
	int a, retry, ret = -ENODEV;

	for (a = 0; a < ARRAY_SIZE(trim_addrs); a++) {
		for (retry = 0; retry < NT36XXX_MAX_RETRIES; retry++) {
			ret = nt36xxx_bootloader_reset(ts);
			if (ret) {
				dev_err(ts->dev, "Can't reset the nvt IC: %d\n", ret);
				return ret;
			}

			ret = regmap_raw_read(ts->regmap, trim_addrs[a], id,
					      sizeof(id));
			if (ret) {
				usleep_range(10000, 11000);
				continue;
			}

			t = nt36xxx_match_trim(id);
			if (!t) {
				/* First attempt per address at info level: the
				 * bytes show a framing shift on real hardware. */
				if (!retry)
					dev_info(ts->dev, "trim 0x%05x: %6ph not known\n",
						 trim_addrs[a], id);
				else
					dev_dbg(ts->dev, "trim 0x%05x attempt %d: %6ph\n",
						trim_addrs[a], retry, id);
				ret = -ENODEV;
				usleep_range(10000, 11000);
				continue;
			}

			if (ts->data->chip && t->mapid != ts->data->chip) {
				dev_err(ts->dev, "trim %6ph is chip %d, not %d\n",
					id, t->mapid, ts->data->chip);
				return -ENODEV;
			}
			if (!t->hw_crc) {
				dev_err(ts->dev, "trim %6ph has no hw CRC support\n", id);
				return -ENODEV;
			}

			ts->hw_crc = t->hw_crc;
			dev_info(ts->dev, "This is NVT touch IC, trim %6ph at 0x%05x, mapid %d\n",
				 id, trim_addrs[a], t->mapid);
			return 0;
		}
	}

	dev_info(ts->dev, "NVT trim ID not identified: %d\n", ret);
	return ret;
}

/*
 * this function is nearly direct copy from vendor source
*/
static int32_t nvt_bin_header_parser(struct device *dev, int hw_crc, const u8 *fwdata, size_t fwsize, struct nvt_ts_bin_map **bin_map_ptr, uint8_t *partition_ptr, uint8_t ilm_dlm_num)
{
	uint8_t list = 0;
	uint32_t pos = 0x00;
	uint32_t end = 0x00;
	uint8_t info_sec_num = 0;
	uint8_t ovly_sec_num = 0;
	uint8_t ovly_info = 0;
	uint8_t partition;
	struct nvt_ts_bin_map *bin_map;

	/* Validate the firmware's own indexes before dereferencing any section. */
	if (fwsize < 0x30 || !hw_crc)
		return -EINVAL;
	end = get_unaligned_le32(fwdata);
	if (end < 0x30 || end > fwsize || (end - 0x30) % 0x10)
		return -EINVAL;
	if ((end - 0x30) / 0x10 > U8_MAX - ilm_dlm_num)
		return -EINVAL;
	info_sec_num = (end - 0x30) / 0x10;

	/*
	 * Find the DLM OVLY section
	 * [0:3] Overlay Section Number
	 * [4]   Overlay Info
	 */
	ovly_info = (fwdata[0x28] & 0x10) >> 4;
	ovly_sec_num = (ovly_info) ? (fwdata[0x28] & 0x0F) : 0;
	if (info_sec_num + ovly_sec_num > U8_MAX - ilm_dlm_num)
		return -EINVAL;

	/*
	 * calculate all partition number
	 * ilm_dlm_num (ILM & DLM) + ovly_sec_num + info_sec_num
	 */
	*partition_ptr = partition = ilm_dlm_num + ovly_sec_num + info_sec_num;
	dev_dbg(dev, "ovly_info = %d, ilm_dlm_num = %d, ovly_sec_num = %d, info_sec_num = %d, partition = %d\n",
			ovly_info, ilm_dlm_num, ovly_sec_num, info_sec_num, partition);

	/* allocated memory for header info */
	*bin_map_ptr = bin_map = (struct nvt_ts_bin_map *)kzalloc((partition + 1) * sizeof(struct nvt_ts_bin_map), GFP_KERNEL);
	if(bin_map == NULL) {
		dev_err(dev, "kzalloc for bin_map failed!\n");
		return -ENOMEM;
	}

	for (list = 0; list < partition; list++) {
		/*
		 * [1] parsing ILM & DLM header info
		 * bin_addr : sram_addr : size (12-bytes)
		 * crc located at 0x18 & 0x1C
		 */
		if (list < ilm_dlm_num) {
			memcpy(&bin_map[list].bin_addr, &(fwdata[0 + list*12]), 4);
			memcpy(&bin_map[list].sram_addr, &(fwdata[4 + list*12]), 4);
			memcpy(&bin_map[list].size, &(fwdata[8 + list*12]), 4);
			memcpy(&bin_map[list].crc, &(fwdata[0x18 + list*4]), 4);

                        if (!hw_crc) {
				dev_err(dev, "%s %d sw-crc not support", __func__, __LINE__);
				return -EINVAL;
			}

			if (list == 0)
				sprintf(bin_map[list].name, "ILM");
			else if (list == 1)
				sprintf(bin_map[list].name, "DLM");
		}

		/*
		 * [2] parsing others header info
		 * sram_addr : size : bin_addr : crc (16-bytes)
		 */
		if ((list >= ilm_dlm_num) && (list < (ilm_dlm_num + info_sec_num))) {

			/* others partition located at 0x30 offset */
			pos = 0x30 + (0x10 * (list - ilm_dlm_num));

			memcpy(&bin_map[list].sram_addr, &(fwdata[pos]), 4);
			memcpy(&bin_map[list].size, &(fwdata[pos+4]), 4);
			memcpy(&bin_map[list].bin_addr, &(fwdata[pos+8]), 4);
                        memcpy(&bin_map[list].crc, &(fwdata[pos+12]), 4);

			if (!hw_crc) {
				dev_info(dev, "ok, hw_crc not presents!");
				return -EINVAL;
			}

			/* detect header end to protect parser function */
			if ((bin_map[list].bin_addr == 0) && (bin_map[list].size != 0)) {
				sprintf(bin_map[list].name, "Header");
			} else {
				sprintf(bin_map[list].name, "Info-%d", (list - ilm_dlm_num));
			}
		}

		/*
		 * [3] parsing overlay section header info
		 * sram_addr : size : bin_addr : crc (16-bytes)
		 */
		if (list >= (ilm_dlm_num + info_sec_num)) {
			size_t overlay_offset;

			/* overlay info located at DLM (list = 1) start addr */
			overlay_offset = (size_t)bin_map[1].bin_addr +
				0x10 * (list - ilm_dlm_num - info_sec_num);
			if (overlay_offset > fwsize || fwsize - overlay_offset < 16)
				return -EINVAL;
			pos = overlay_offset;

			memcpy(&bin_map[list].sram_addr, &(fwdata[pos]), 4);
			memcpy(&bin_map[list].size, &(fwdata[pos+4]), 4);
			memcpy(&bin_map[list].bin_addr, &(fwdata[pos+8]), 4);
			memcpy(&bin_map[list].crc, &(fwdata[pos+12]), 4);

			if (!hw_crc) {
                                dev_err(dev, "%s %d sw_crc not support", __func__, __LINE__);
                                return -EINVAL;
			}

			sprintf(bin_map[list].name, "Overlay-%d", (list- ilm_dlm_num - info_sec_num));
		}

		/* BIN size error detect */
		/* The ROM format stores payload length minus one for nonempty
		 * partitions. Inclusive end must be inside the firmware buffer. */
		if (bin_map[list].bin_addr > fwsize ||
		    (bin_map[list].size && bin_map[list].size >=
		     fwsize - bin_map[list].bin_addr)) {
			dev_err(dev, "access range (0x%08X to 0x%08X) is larger than bin size!\n",
					bin_map[list].bin_addr, bin_map[list].bin_addr + bin_map[list].size);
			return -EINVAL;
		}

		dev_dbg(dev, "[%d][%s] SRAM (0x%08X), SIZE (0x%08X), BIN (0x%08X), CRC (0x%08X)\n",
                              list, bin_map[list].name,
                              bin_map[list].sram_addr, bin_map[list].size,  bin_map[list].bin_addr, bin_map[list].crc);
	}

	return 0;
}

static int32_t nt36xxx_download_firmware_hw_crc(struct nt36xxx_ts *ts) {
	uint32_t list = 0;
	uint32_t bin_addr, sram_addr, size;
	struct nvt_ts_bin_map *bin_map = ts->bin_map;

	if (nt36xxx_bootloader_reset(ts) < 0)
		return -EIO;

	for (list = 0; list < ts->fw_data.partition; list++) {
		int j;

		/* initialize variable */
		sram_addr = bin_map[list].sram_addr;
		size = bin_map[list].size;
		bin_addr = bin_map[list].bin_addr;

		/* ignore reserved partition (Reserved Partition size is zero) */
		if (!size) {
			dev_dbg(ts->dev, "found empty part %d. skipping ", list);
			continue;
		} else {
			size = size + 1;
			dev_dbg(ts->dev, "found useful part %d. size 0x%x ", list, size);
		}

		bin_map[list].loaded = 1;

		if (size / NT36XXX_TRANSFER_LEN)
			dev_dbg(ts->dev, "%s %d paged write [%s] 0x%x, window 0x%x, residue 0x%x",
					__func__, __LINE__, bin_map[list].name, size,
					NT36XXX_TRANSFER_LEN, size % NT36XXX_TRANSFER_LEN);

		for (j = 0; j < size; j += NT36XXX_TRANSFER_LEN) {
			int window_size = ((size - j) / NT36XXX_TRANSFER_LEN) ? NT36XXX_TRANSFER_LEN :
						((size - j) % NT36XXX_TRANSFER_LEN);

			int ret = regmap_bulk_write(ts->regmap, sram_addr + j, &ts->fw_entry.data[bin_addr + j],
							 window_size);

			if (ret < 0)
				return ret;
			/* A panel power-down invalidates the partial upload. */
			if (ts->status & NT36XXX_STATUS_SUSPEND)
				return -EIO;
		}

	}

	return 0;
}

static void nt36xxx_release_memory(void *data);
static int _nt36xxx_boot_prepare_firmware(struct nt36xxx_ts *ts) {
	int i, ret;
	size_t fw_need_write_size = 0;
	size_t fw_size;
	const struct firmware *fw_entry;
	void *data;

	/* add one more guard */
	if (ts->status & NT36XXX_STATUS_PREPARE_FIRMWARE)
		return 0;

	/* supposed we need to load once and use many time */
	if (ts->fw_entry.data)
		return 0;

	ret = request_firmware(&fw_entry, ts->fw_name, ts->dev);
	if (ret) {
		dev_err(ts->dev, "request fw fail name=%s\n", ts->fw_name);
		return ret;
	}
	if (fw_entry->size < 4096 || fw_entry->size > 512 * 1024) {
		release_firmware(fw_entry);
		return -EINVAL;
	}

	/*
	 * must allocate in DMA buffer otherwise fail spi tx DMA
	 * so we need to manage our own fw struct
	 * pm_resume need to re-upload fw for NT36675 IC
	 *
	 */
	fw_size = fw_entry->size;
	ts->fw_entry.data = data = kmemdup(fw_entry->data, fw_size, GFP_KERNEL | GFP_DMA);

	release_firmware(fw_entry);
	if (!ts->fw_entry.data) {
		dev_err(ts->dev, "memdup fw_data fail\n");
		return -ENOMEM;
	}
	ts->fw_entry.size = fw_size;

	for (i = (ts->fw_entry.size / 4096); i > 0; i--) {
		if (strncmp(&ts->fw_entry.data[i * 4096 - 3], "NVT", 3) == 0) {
			fw_need_write_size = i * 4096;
			break;
		}

		if (strncmp(&ts->fw_entry.data[i * 4096 - 3], "MOD", 3) == 0) {
			fw_need_write_size = i * 4096;
			break;
		}
	}

	if (fw_need_write_size == 0) {
		dev_err(ts->dev, "fw parsing error\n");
		kfree(data);
		ts->fw_entry.data = NULL;
		ts->fw_entry.size = 0;
		if (ts->bin_map) {
			kfree(ts->bin_map);
			ts->bin_map = NULL;
		}
		return -EIO;
	}

	if (*(ts->fw_entry.data + (fw_need_write_size - 4096)) + *(ts->fw_entry.data +
						((fw_need_write_size - 4096) + 1)) != 0xFF) {
		dev_err(ts->dev, "bin file FW_VER + FW_VER_BAR should be 0xFF!");
		dev_err(ts->dev, "FW_VER=0x%02X, FW_VER_BAR=0x%02X\n",
			*(ts->fw_entry.data+(fw_need_write_size - 4096)),
			*(ts->fw_entry.data+(fw_need_write_size - 4096 + 1)));

		kfree (data);
		ts->fw_entry.data = NULL;
		ts->fw_entry.size = 0;
		if (ts->bin_map) {
			kfree(ts->bin_map);
			ts->bin_map = NULL;
		}
		return -EIO;
	}

	ts->fw_data.ilm_dlm_num = 2;

	ret = nvt_bin_header_parser(ts->dev, ts->hw_crc, ts->fw_entry.data, ts->fw_entry.size,
			&ts->bin_map, &ts->fw_data.partition, ts->fw_data.ilm_dlm_num);
	if (ret) {
		kfree (data);
		ts->fw_entry.data = NULL;
		ts->fw_entry.size = 0;
		if(ret != -ENOMEM){
			if (ts->bin_map) {
				kfree(ts->bin_map);
				ts->bin_map = NULL;
			}
		}

		/* really dont let the tasklet re-enter since no needed for broken fw data */
		ts->status |= NT36XXX_STATUS_DOWNLOAD_COMPLETE;
		dev_err(ts->dev, "Parsing fw error, stop re-loading fw now on, ret=0x%x!", ret);
		return ret;
	}

	ts->status |= NT36XXX_STATUS_PREPARE_FIRMWARE;

	ret = devm_add_action_or_reset(ts->dev, nt36xxx_release_memory, ts);
	if (ret)
		return ret;

	return 0;
}

/* One stock nvt_download_firmware_hw_crc() pass, up to the INIT state. */
static int nt36xxx_boot_firmware_once(struct nt36xxx_ts *ts)
{
	int i, ret;
	u8 val[1];

	if (ts->hw_crc) {
		ret = nt36xxx_download_firmware_hw_crc(ts);
		if (ret) {
			dev_err(ts->dev, "nt36xxx_download_firmware_hw_crc fail!");
			return ret;
		}

	} else {
		dev_err(ts->dev, "non-hw_crc model is not support yet!");
		return -EIO;
	}

	/*
	 * nvt_set_bld_hw_crc(): SRAM start (3 bytes), header size field
	 * (3 bytes, bit 16 at most) and golden CRC (4 bytes), little endian.
	 */
	for (i = 0; i < ts->fw_data.partition; i++) {
		struct nvt_ts_bin_map *m = &ts->bin_map[i];
		u8 des[3], len[3], crc[4];
		int des_reg, len_reg, crc_reg;

		if (!strcmp(m->name, "ILM")) {
			des_reg = MMAP_ILM_DES_ADDR;
			len_reg = MMAP_ILM_LENGTH_ADDR;
			crc_reg = MMAP_G_ILM_CHECKSUM_ADDR;
		} else if (!strcmp(m->name, "DLM")) {
			des_reg = MMAP_DLM_DES_ADDR;
			len_reg = MMAP_DLM_LENGTH_ADDR;
			crc_reg = MMAP_G_DLM_CHECKSUM_ADDR;
		} else {
			continue;
		}

		put_unaligned_le24(m->sram_addr, des);
		len[0] = m->size;
		len[1] = m->size >> 8;
		len[2] = (m->size >> 16) & 0x01;
		put_unaligned_le32(m->crc, crc);

		ret = regmap_raw_write(ts->regmap, ts->mmap[des_reg], des, sizeof(des));
		if (!ret)
			ret = regmap_raw_write(ts->regmap, ts->mmap[len_reg], len, sizeof(len));
		if (!ret)
			ret = regmap_raw_write(ts->regmap, ts->mmap[crc_reg], crc, sizeof(crc));
		if (ret)
			return ret;
	}

	/* nvt_bld_crc_enable(): read-modify-write bit 7. */
	ret = regmap_raw_read(ts->regmap, ts->mmap[MMAP_BLD_CRC_EN_ADDR], val, 1);
	if (ret)
		return ret;
	val[0] |= BIT(7);
	ret = regmap_raw_write(ts->regmap, ts->mmap[MMAP_BLD_CRC_EN_ADDR], val, 1);
	if (ret)
		return ret;

	/* nvt_fw_crc_enable() */
	val[0] = 0;
	ret = regmap_raw_write(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR] | NT36XXX_EVT_RESET_COMPLETE, val, 1);
	if (ret)
		return ret;
	val[0] = 0xae;
	ret = regmap_raw_write(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR] | NT36XXX_EVT_HOST_CMD, val, 1);
	if (ret)
		return ret;

	/* nvt_boot_ready() */
	val[0] = 0x1;
	ret = regmap_raw_write(ts->regmap, ts->mmap[MMAP_BOOT_RDY_ADDR], val, 1);
	if (ret)
		return ret;

	/* add checking to those sleep func such that can suspend early */
	if (ts->status & NT36XXX_STATUS_SUSPEND)
		return -EIO;

	/* old logic 5ms, retention to 10ms */
	usleep_range(10000, 11000);

	/* nvt_check_fw_reset_state() */
	return nt36xxx_check_reset_state(ts, NT36XXX_STATE_INIT);
}

static int _nt36xxx_boot_download_firmware(struct nt36xxx_ts *ts)
{
	int ret = -EIO, retry;
	u8 val[2];

	/* this also avoid suspend with delay-queue active */
	if (!(ts->status & NT36XXX_STATUS_PREPARE_FIRMWARE))
		return -EIO;

	/* The stock driver repeats the whole upload and boot pass 3 times. */
	for (retry = 0; retry < 3; retry++) {
		ret = nt36xxx_boot_firmware_once(ts);
		if (!ret || (ts->status & NT36XXX_STATUS_SUSPEND))
			break;
		dev_warn(ts->dev, "firmware boot attempt %d failed: %d\n",
			 retry + 1, ret);
	}
	if (ret)
		return ret;

	/*
	 * nvt_get_fw_info(): data[0] = fw_ver, data[1] = ~fw_ver. The stock
	 * driver retries three times and then continues with defaults, so a
	 * broken info block is reported but is not a download failure.
	 */
	for (retry = 0; retry < 3; retry++) {
		ret = regmap_raw_read(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR] |
				      NT36XXX_EVT_FWINFO, val, 2);
		if (ret)
			return ret;
		if ((u8)(val[0] + val[1]) == 0xff)
			break;
		usleep_range(10000, 11000);
	}

	if (retry == 3)
		dev_warn(ts->dev, "FW info is broken: fw_ver=0x%02x, bar=0x%02x\n",
			 val[0], val[1]);
	else
		dev_info(ts->dev, "Touch IC fw 0x%02x loaded ok\n", val[0]);

	/* Stock nvt_get_fw_info() ends here: frames are read from this page. */
	ret = nt36xxx_bus_set_page(ts->dev, ts->mmap[MMAP_EVENT_BUF_ADDR]);
	if (ret)
		return ret;
	ts->bad_frames = 0;

	ts->status |= NT36XXX_STATUS_DOWNLOAD_COMPLETE;

	return 0;
}

static void nt36xxx_download_firmware(struct work_struct *work) {
	struct nt36xxx_ts *ts = container_of(work, struct nt36xxx_ts, work.work);
	int ret;

	mutex_lock(&ts->lock);
	ret = _nt36xxx_boot_prepare_firmware(ts);
	mutex_unlock(&ts->lock);
	if (ret)
		dev_err(ts->dev, "firmware %s unavailable: %d\n", ts->fw_name, ret);

	if (!(ts->status & NT36XXX_STATUS_PREPARE_FIRMWARE) ||
	    (ts->status & NT36XXX_STATUS_DOWNLOAD_COMPLETE))
		goto exit;

	/*
	 * The driver has no runtime-PM callbacks and the A22x touch supplies
	 * belong to the panel, so no runtime-PM transition is needed here.
	 * Wait for a running report thread before resetting the MCU.
	 */
	disable_irq(ts->irq);

	mutex_lock(&ts->lock);

	if (ts->status & NT36XXX_STATUS_SUSPEND)
		goto unlock;

	ret = nt36xxx_eng_reset_idle(ts);
	if (ret) {
		dev_err(ts->dev, "Failed to check chip version step(3), ret=%d\n", ret);
		goto unlock;
	}

	ret = nt36xxx_chip_version_init(ts);
	if (ret) {
		dev_err(ts->dev, "Failed to check chip version step(4), ret=%d\n", ret);
		goto unlock;
	}

	ret = _nt36xxx_boot_download_firmware(ts);
	if (ret)
		dev_err(ts->dev, "firmware download failed: %d\n", ret);
	else
		ts->download_attempts = 0;
unlock:
	mutex_unlock(&ts->lock);
	enable_irq(ts->irq);
exit:
	if (ts->status & (NT36XXX_STATUS_DOWNLOAD_COMPLETE | NT36XXX_STATUS_SUSPEND))
		return;
	if (++ts->download_attempts >= NT36XXX_MAX_RETRIES) {
		dev_err(ts->dev, "giving up after %u firmware attempts\n",
			ts->download_attempts);
		return;
	}
	schedule_delayed_work(&ts->work, msecs_to_jiffies(4000));
}

static void nt36xxx_release_memory(void *data)
{
	struct nt36xxx_ts *ts = data;
	kfree(ts->bin_map);
	kfree(ts->fw_entry.data);
}

static void nt36xxx_disable_regulators(void *data)
{
	struct nt36xxx_ts *ts = data;

	regulator_bulk_disable(NT36XXX_NUM_SUPPLIES, ts->supplies);
}

static int nt36xxx_enable_regulators(struct nt36xxx_ts *ts)
{
	struct device *dev = ts->dev;
	int ret;

	if (drm_is_panel_follower(dev))
		return 0;
	/* The A22x LK keeps the panel supplies enabled and exposes no touch
	 * regulator phandles. Do not turn an absent optional supply into an
	 * endless probe deferral. The board's SPI reset/IRQ are still required. */
	if (!device_property_present(dev, "vdd-supply"))
		return 0;

	/* These supplies are optional, also shared with LCD panel */
	ts->supplies[0].supply = "vdd";
	ts->supplies[1].supply = "vio";
	ts->supplies[2].supply = "vio2";
	ts->supplies[3].supply = "vio3";

	ret = devm_regulator_bulk_get(dev, NT36XXX_NUM_SUPPLIES, ts->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot get supplies: %d\n", ret);
	ts->supplies_acquired = true;

	ret = regulator_bulk_enable(NT36XXX_NUM_SUPPLIES, ts->supplies);
	if (ret)
		return ret;

	usleep_range(10000, 11000);

	ret = devm_add_action_or_reset(dev, nt36xxx_disable_regulators, ts);
	if (ret)
		return ret;

	return 0;
}

static int nt36xxx_input_dev_config(struct nt36xxx_ts *ts, const struct input_id *id)
{
	struct device *dev = ts->dev;
	int ret;

        ts->input = devm_input_allocate_device(dev);
        if (!ts->input)
                return -ENOMEM;

	input_set_drvdata(ts->input, ts);

	ts->input->phys = devm_kasprintf(dev, GFP_KERNEL,
				     "%s/input0", dev_name(dev));
	if (!ts->input->phys)
		return -ENOMEM;

	ts->input->name = "sec_touchscreen";
	ts->input->dev.parent = dev;
	ts->input->id = *id;

	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0,
						 TOUCH_MAX_PRESSURE, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	/* Palm and metal-jig contacts are reported as MT_TOOL_PALM. */
	input_set_abs_params(ts->input, ABS_MT_TOOL_TYPE, 0, MT_TOOL_MAX, 0, 0);

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0,
						 ts->data->abs_x_max - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0,
						 ts->data->abs_y_max - 1, 0, 0);

	touchscreen_parse_properties(ts->input, true, &ts->prop);

	/* The double tap of the wake gesture (panel follower only). */
	if (drm_is_panel_follower(dev))
		input_set_capability(ts->input, EV_KEY, KEY_WAKEUP);

	WARN(ts->prop.max_x < 1, "ts->prop.max_x < 1");

	ret = input_mt_init_slots(ts->input, TOUCH_MAX_FINGER_NUM,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret) {
		dev_err(dev, "Cannot init MT slots (%d)\n", ret);
		return ret;
	}

	ret = input_register_device(ts->input);
	if (ret) {
		dev_err(dev, "Failed to register input device: %d\n",
			ret);
		return ret;
	}

	return 0;
}

static int nt36xxx_of_compatible(struct device *dev)
{
	struct device_node *np = dev->of_node;

	if (!of_device_is_compatible(np, "novatek,NVT-default-spi")) {
		const char *path = "/chosen";
		struct device_node *dt_node;
		const char *bootargs;

		dt_node = of_find_node_by_path(path);
		if (!dt_node) {
			dev_err(dev, "Failed to find device-tree node: %s\n", path);
			return -ENODEV;
		}

		/* Tianma is a panel vendor, not a Novatek chip identity: the
		 * ILI7807S variant may also have a Tianma display. Only attempt
		 * the Novatek SPI protocol when LK selected a Novatek family.
		 */
		/* Missing bootargs must not let a Novatek probe reset a Himax,
		 * Ilitek or FocalTech controller on the shared SPI chip select. */
		if (of_property_read_string(dt_node, "bootargs", &bootargs) ||
		    (!strstr(bootargs, "nt36") && !strstr(bootargs, "NT36") &&
		     !strstr(bootargs, "novatek") && !strstr(bootargs, "NVT"))) {
			of_node_put(dt_node);
			return -ENODEV;
		}

		of_node_put(dt_node);

		dev_info(dev, "Try to probe novatek/tianma panel as specified in chosen/bootargs.");
	}
	return 0;
}

int nt36xxx_probe(struct device *dev, int irq, const struct input_id *id,
			struct regmap *regmap)
{
	const struct nt36xxx_chip_data *chip_data;
	const char *signed_fwname = NULL;
	int ret;

	struct nt36xxx_ts *ts = devm_kzalloc(dev, sizeof(struct nt36xxx_ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	dev_set_drvdata(dev, ts);

	chip_data = of_device_get_match_data(dev);
	if(!chip_data)
		return -EINVAL;
	/* The LK DTBO offers all four chip families on one SPI CS. Check the
	 * selected panel before sending a reset command to Himax/FocalTech. */
	ret = nt36xxx_of_compatible(dev);
	if (ret)
		return ret;

	ts->dev = dev;
	ts->regmap = regmap;
	ts->irq = irq;

	ts->data = chip_data;
	memcpy(ts->mmap_data, chip_data->mmap, sizeof(ts->mmap_data));
	ts->mmap = ts->mmap_data;

	ts->supplies = devm_kcalloc(dev, NT36XXX_NUM_SUPPLIES,
				    sizeof(*ts->supplies), GFP_KERNEL);
	if (!ts->supplies)
		return -ENOMEM;

	ts->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ts->reset_gpio))
		return PTR_ERR(ts->reset_gpio);

	if (ts->reset_gpio)
		gpiod_set_consumer_name(ts->reset_gpio, "nt36xxx_reset");

	ts->irq_gpio = devm_gpiod_get_optional(dev, "irq", GPIOD_IN);
	if (IS_ERR(ts->irq_gpio))
		return PTR_ERR(ts->irq_gpio);

	if (irq <= 0) {
		if (!ts->irq_gpio)
			return dev_err_probe(dev, -EINVAL, "touch IRQ is missing\n");
		ts->irq = gpiod_to_irq(ts->irq_gpio);
		if (ts->irq <=0) {
			dev_err(dev, "either need irq or irq-gpio specified in devicetree node!\n");
			return -EINVAL;
		}

		dev_info(ts->dev, "irq %d", ts->irq);
	}

	if (ts->irq_gpio)
		gpiod_set_consumer_name(ts->irq_gpio, "nt36xxx_irq");

	ret = nt36xxx_enable_regulators(ts);
	if (ret)
		return ret;

	mutex_init(&ts->lock);

	ret = nt36xxx_eng_reset_idle(ts);
        if (ret) {
                dev_err(dev, "Failed to check chip version step(1), ret=%d\n", ret);
                return ret;
        }

	/* Set memory maps for the specific chip version */
	ret = nt36xxx_chip_version_init(ts);
	if (ret) {
		dev_err(dev, "Failed to check chip version step(2), ret=%d\n", ret);
		return ret;
	}

	/* copy the const mmap into drvdata */
	memcpy(ts->mmap_data, ts->data->mmap, sizeof(ts->mmap_data));
	ts->mmap = ts->mmap_data;

	ret = nt36xxx_input_dev_config(ts, ts->data->id);
	if (ret) {
		dev_err(dev, "failed set input device: %d\n", ret);
		return ret;
	}

	/*
	 * The shared A22x touch node carries Himax's falling-edge trigger. The
	 * stock Novatek DT and probe log use 0x2001 (rising | oneshot), so the
	 * match data may name a board-proven trigger; 0 keeps the DT trigger.
	 */
	ret = devm_request_threaded_irq(dev, ts->irq, NULL, nt36xxx_irq_handler,
			IRQF_ONESHOT | ts->data->irq_trigger, dev_name(dev), ts);
	if (ret) {
		dev_err(dev, "request irq failed: %d\n", ret);
		return ret;
	}

	/* init with default name */
	ts->fw_name = ts->data->fw_name;
	/* support overriding fw name */
	of_property_read_string_index(ts->dev->of_node, "firmware-name", 0, &signed_fwname);
	if (signed_fwname)
		ts->fw_name = signed_fwname;

	if (drm_is_panel_follower(dev)) {
		ts->panel_follower.funcs = &nt36xxx_panel_follower_funcs;
		ret = devm_drm_panel_add_follower(dev, &ts->panel_follower);
		if (ret)
			return dev_err_probe(dev, ret, "cannot follow display panel\n");
	}

	/* have to make sure this is first time schedule work, if devm_drm_panel_add_follower
	 * called into internal resume with schedule_delay_work, then block it over there */
	if (ts->fw_name) {
		ts->status |= NT36XXX_STATUS_NEED_FIRMWARE;

		devm_delayed_work_autocancel(dev, &ts->work, nt36xxx_download_firmware);
		schedule_delayed_work(&ts->work, 0);
	}

	dev_info(dev, "probe ok: irq %d, trigger 0x%lx (0 = devicetree), fw %s\n",
		 ts->irq, ts->data->irq_trigger, ts->fw_name ? ts->fw_name : "none");
	return 0;
}

EXPORT_SYMBOL_GPL(nt36xxx_probe);

/* The controller stops reporting: end every contact userspace still holds. */
static void nt36xxx_release_contacts(struct nt36xxx_ts *ts)
{
	int i;

	for (i = 0; i < TOUCH_MAX_FINGER_NUM; i++) {
		input_mt_slot(ts->input, i);
		input_mt_report_slot_inactive(ts->input);
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static int __maybe_unused nt36xxx_internal_pm_suspend(struct device *dev)
{
	struct nt36xxx_ts *ts = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&ts->lock);
	ts->status |= NT36XXX_STATUS_SUSPEND;
	mutex_unlock(&ts->lock);

	cancel_delayed_work_sync(&ts->work);

	/* Vendor nvt_ts_suspend(): 0x11 to EVENT_BUF + 0x50 (host command). */
	mutex_lock(&ts->lock);
	if (ts->status & NT36XXX_STATUS_DOWNLOAD_COMPLETE)
		ret = regmap_write(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR] |
				   NT36XXX_EVT_HOST_CMD, NT36XXX_CMD_ENTER_SLEEP);

	if (ret)
		dev_err(ts->dev, "Cannot enter suspend!\n");
	mutex_unlock(&ts->lock);

	/* Callers have disabled the IRQ and waited for the report thread. */
	nt36xxx_release_contacts(ts);

	return 0;
}

static int __maybe_unused nt36xxx_pm_suspend(struct device *dev)
{
	struct nt36xxx_ts *ts = dev_get_drvdata(dev);
	int ret=0;

	if (drm_is_panel_follower(dev))
		return 0;

	disable_irq(ts->irq);

	/* Send the sleep command while the controller is still powered. */
	ret = nt36xxx_internal_pm_suspend(dev);

	if (ts->supplies_acquired)
		regulator_bulk_disable(NT36XXX_NUM_SUPPLIES, ts->supplies);

	return ret;
}

static int __maybe_unused nt36xxx_internal_pm_resume(struct device *dev)
{
	struct nt36xxx_ts *ts = dev_get_drvdata(dev);

	/* The panel/controller lost SRAM: reload as nvt_ts_resume() does. */
	mutex_lock(&ts->lock);
	ts->status &= ~(NT36XXX_STATUS_SUSPEND | NT36XXX_STATUS_DOWNLOAD_COMPLETE);
	ts->download_attempts = 0;
	ts->wdt_frames = 0;
	mutex_unlock(&ts->lock);

	if (ts->status & NT36XXX_STATUS_NEED_FIRMWARE)
                schedule_delayed_work(&ts->work, 0);

	return 0;
}

static int __maybe_unused nt36xxx_pm_resume(struct device *dev)
{
	struct nt36xxx_ts *ts = dev_get_drvdata(dev);
	int ret=0;

        if (drm_is_panel_follower(dev))
                return 0;

	if (ts->supplies_acquired) {
		ret = regulator_bulk_enable(NT36XXX_NUM_SUPPLIES, ts->supplies);
		if (ret)
			dev_err(dev, "cannot enable supplies: %d\n", ret);
	}

	enable_irq(ts->irq);

	return nt36xxx_internal_pm_resume(dev);
}

EXPORT_GPL_SIMPLE_DEV_PM_OPS(nt36xxx_pm_ops,
			     nt36xxx_pm_suspend,
			     nt36xxx_pm_resume);

/*
 * Vendor nvt_set_aot_mode(): 0x13 to EVENT_BUF + 0x50 instead of deep sleep, once
 * the firmware runs. Called with the interrupt disabled and SUSPEND set.
 */
static int nt36xxx_enter_gesture(struct nt36xxx_ts *ts)
{
	int ret = -EAGAIN;

	cancel_delayed_work_sync(&ts->work);
	mutex_lock(&ts->lock);
	if (ts->status & NT36XXX_STATUS_DOWNLOAD_COMPLETE) {
		ret = regmap_write(ts->regmap, ts->mmap[MMAP_EVENT_BUF_ADDR] |
				   NT36XXX_EVT_HOST_CMD, NT36XXX_CMD_ENTER_GESTURE);
		if (!ret)
			ts->status |= NT36XXX_STATUS_GESTURE;
	}
	mutex_unlock(&ts->lock);
	nt36xxx_release_contacts(ts);
	if (ret)
		dev_info(ts->dev, "no wake gesture this time: %d\n", ret);
	return ret;
}

static int panel_prepared(struct drm_panel_follower *follower)
{
	struct nt36xxx_ts *ts = container_of(follower, struct nt36xxx_ts, panel_follower);

	/* In gesture mode the interrupt stayed on. */
	if ((ts->status & NT36XXX_STATUS_SUSPEND) && !(ts->status & NT36XXX_STATUS_GESTURE))
		enable_irq(ts->irq);
	mutex_lock(&ts->lock);
	ts->status &= ~NT36XXX_STATUS_GESTURE;
	mutex_unlock(&ts->lock);

	/* supposed to clear the flag here, but leave to internal_pm_resume
	 * for greater purpose, then clear flag as:
	 * ts->status &= ~NT36XXX_STATUS_SUSPEND;
	 */
	return nt36xxx_internal_pm_resume(ts->dev);
}

static int panel_unpreparing(struct drm_panel_follower *follower)
{
	struct nt36xxx_ts *ts = container_of(follower, struct nt36xxx_ts, panel_follower);

	mutex_lock(&ts->lock);
	ts->status |= NT36XXX_STATUS_SUSPEND;
	mutex_unlock(&ts->lock);

	disable_irq(ts->irq);

	if (wake_gesture && !nt36xxx_enter_gesture(ts)) {
		enable_irq(ts->irq);
		return 0;
	}
	return nt36xxx_internal_pm_suspend(ts->dev);
}

/*
 * Resumed once the panel is enabled (initialized, out of sleep and on) and suspended
 * before it is disabled, where the vendor driver notifies the touch part: after its
 * display-on, before its display-off (2026-11-20: the A22x Tianma NT36672C panel sends
 * display-on in enable(), after prepare()).
 */
static struct drm_panel_follower_funcs nt36xxx_panel_follower_funcs = {
	.panel_enabled = panel_prepared,
	.panel_disabling = panel_unpreparing,
};

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NT36XXX Touchscreen driver");
MODULE_AUTHOR("AngeloGioacchino Del Regno <kholk11@gmail.com>");
MODULE_AUTHOR("George Chan <gchan9527@gmail.com>");
