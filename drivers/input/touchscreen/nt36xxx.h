/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2010 - 2017 Novatek, Inc.
 * Copyright (C) 2020 AngeloGioacchino Del Regno <kholk11@gmail.com>
 * Copyright (C) 2023-2024 George Chan <gchan9527@gmail.com>
 */

#ifndef NT36XXX_H
#define NT36XXX_H

#define NT36XXX_INPUT_DEVICE_NAME	"Novatek NT36XXX Touch Sensor"
/* Samsung's stock SM-A226B DTBO and successful probe both use 9.6 MHz. */
#define MAX_SPI_FREQ_HZ 10000000

/* FW Param address */
#define NT36XXX_FW_ADDR		0x01

#define NT36XXX_TRANSFER_LEN	(63*1024)

/* due to extra framework layer, the transfer trunk is as small as
 * 128 otherwize dma error happened, all routed to spi_sync()
*/

/* Number of bytes for chip identification */
#define NT36XXX_ID_LEN_MAX	6

/* Touch info */
#define TOUCH_DEFAULT_MAX_WIDTH  1080
#define TOUCH_DEFAULT_MAX_HEIGHT 2246
#define TOUCH_MAX_FINGER_NUM	 10
#define TOUCH_MAX_PRESSURE	 1000

/*
 * Touch frame read from EVENT_BUF. Offsets below are relative to the first
 * returned data byte (the SPI transport consumes the command and the dummy
 * byte); the vendor driver's buf[n] is data[n - 1] here.
 *   data[6*i .. 6*i+5]  slot i: id<<3|status, x[11:4], y[11:4],
 *                       x[3:0]<<4|y[3:0], width, pressure[7:0]
 *   data[61], data[62]  pressure[15:8] of slots 0 and 1
 *   data[64]            two's complement checksum of data[0..63]
 */
#define POINT_DATA_LEN		65
#define POINT_DATA_CHECKSUM_LEN	65
#define NT36XXX_WDT_FRAMES	10

/* Trim ID locations: new chips first, then the pre-NT36672C address. */
#define NT36XXX_CHIP_VER_TRIM_ADDR	0x3F004
#define NT36XXX_CHIP_VER_TRIM_OLD_ADDR	0x1F64E

/* Misc */
#define NT36XXX_NUM_SUPPLIES	 4
#define NT36XXX_MAX_RETRIES	 5
#define NT36XXX_MAX_FW_RST_RETRY 50

enum nt36xxx_chips {
        NT36525_IC = 0x1,
        NT36672A_IC,
        NT36676F_IC,
        NT36772_IC,
        NT36675_IC,
        NT36870_IC,
        NTMAX_IC,
};

enum nt36xxx_cmds {
	NT36XXX_CMD_ENTER_SLEEP = 0x11,
	NT36XXX_CMD_BOOTLOADER_RESET = 0x69,
};

enum nt36xxx_events {
        NT36XXX_EVT_REPORT              = 0x00,
        NT36XXX_EVT_CRC                 = 0x35,
        NT36XXX_EVT_HOST_CMD            = 0x50,
        NT36XXX_EVT_HS_OR_SUBCMD        = 0x51, /* Handshake or subcommand byte */
        NT36XXX_EVT_RESET_COMPLETE      = 0x60,
        NT36XXX_EVT_FWINFO              = 0x78,
        NT36XXX_EVT_READ_PID            = 0x80,
        NT36XXX_EVT_PROJECTID           = 0x9a, /* Excess 0x80 write bit, messed trouble, ignored */
};

enum nt36xxx_fw_state {
        NT36XXX_STATE_INIT = 0xa0,              /* IC Reset */
        NT36XXX_STATE_REK = 0xa1,               /* ReK baseline */
        NT36XXX_STATE_REK_FINISH = 0xa2,        /* Baseline is ready */
        NT36XXX_STATE_NORMAL_RUN = 0xa3,        /* Firmware is running */
        NT36XXX_STATE_MAX = 0xaf
};

struct nt36xxx_ts;

struct nvt_fw_parse_data {
	uint8_t partition;
	uint8_t ilm_dlm_num;
};

struct nvt_ts_bin_map {
	char name[12];
	uint32_t bin_addr;
	uint32_t sram_addr;
	uint32_t size;
	uint32_t crc;
	uint32_t loaded;
};

struct nvt_ts_hw_info {
	uint8_t carrier_system;
	uint8_t hw_crc;
};

struct nt36xxx_abs_object {
	u16 x;
	u16 y;
	u16 z;
	u8 tm;
};

struct nt36xxx_fw_info {
	u8 fw_ver;
	u8 x_num;
	u8 y_num;
	u8 max_buttons;
	u16 abs_x_max;
	u16 abs_y_max;
	u16 nvt_pid;
};

struct nt36xxx_chip_data {
	const u32 *mmap;
	const struct regmap_config *config;

	const char* fw_name;
	unsigned int max_x;
	unsigned int max_y;
	unsigned int abs_x_max;
	unsigned int abs_y_max;
	unsigned int max_button;
	const struct input_id *id;
	/* Expected trim family; 0 accepts any table entry. */
	enum nt36xxx_chips chip;
	/* Board-proven trigger; 0 keeps the devicetree trigger. */
	unsigned long irq_trigger;
};

struct nt36xxx_trim_table {
	u8 id[NT36XXX_ID_LEN_MAX];
	u8 mask[NT36XXX_ID_LEN_MAX];
	enum nt36xxx_chips mapid;
	uint8_t carrier_system;
	uint8_t hw_crc;
};

int nt36xxx_probe(struct device *dev, int irq, const struct input_id *id,
			struct regmap *regmap);

extern const struct dev_pm_ops nt36xxx_pm_ops;
extern const u32 nt36675_memory_maps[];
extern const u32 nt36672a_memory_maps[];
extern const u32 nt36772_memory_maps[];
extern const u32 nt36676f_memory_maps[];
extern const u32 nt36525_memory_maps[];
#endif
