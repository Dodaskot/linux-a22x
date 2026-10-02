/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Himax HX83112F (zero-flash TDDI) touch, mainline SPI platform glue.
 * The IC/common layers are taken from the Samsung SM-A226B kernel source.
 */
#ifndef HIMAX_PLATFORM_H
#define HIMAX_PLATFORM_H

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/types.h>
#include <linux/kthread.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/spi/spi.h>
#include "himax_ic_core.h"

#define CONFIG_OF_TOUCH

extern struct device *g_device;

#define D(x...) dev_dbg(g_device, "[HXTP] " x)
#define I(x...) dev_info(g_device, "[HXTP] " x)
#define W(x...) dev_warn(g_device, "[HXTP] " x)
#define E(x...) dev_err(g_device, "[HXTP] " x)
#define DIF(x...) \
do { \
	if (debug_flag) \
		dev_info(g_device, "[HXTP][DEBUG] " x); \
} while (0)

#define ENABLE 1
#define DISABLE 0
#define HIMAX_BUS_RETRY_TIMES 10
#define HIMAX_common_NAME "himax-hx83112f"

extern int g_lcm_name;
extern struct himax_ic_data *ic_data;
extern struct himax_ts_data *private_ts;
int himax_chip_common_init(void);
void himax_chip_common_deinit(void);
void himax_ts_work(struct himax_ts_data *ts);
enum hrtimer_restart himax_ts_timer_func(struct hrtimer *timer);

struct himax_platform_data {
	int abs_x_min;
	int abs_x_max;
	int abs_x_fuzz;
	int abs_y_min;
	int abs_y_max;
	int abs_y_fuzz;
	int abs_pressure_min;
	int abs_pressure_max;
	int abs_pressure_fuzz;
	int abs_width_min;
	int abs_width_max;
	int screenWidth;
	int screenHeight;
	uint8_t fw_version;
	uint8_t tw_id;
	uint8_t powerOff3V3;
	uint8_t cable_config[2];
	uint8_t protocol_type;
	int gpio_irq;
	int gpio_reset;
	int gpio_3v3_en;
	int (*power)(int on);
	void (*reset)(void);
	struct himax_virtual_key *virtual_key;
	struct kobject *vk_obj;
	struct kobj_attribute *vk2Use;
	struct himax_config *hx_config;
	int hx_config_size;
};

int himax_bus_read(uint8_t command, uint8_t *data, uint32_t length);
int himax_bus_write(uint8_t command, uint8_t *data, uint32_t length);
void himax_int_enable(int enable);
int himax_ts_register_interrupt(void);
int himax_ts_unregister_interrupt(void);
uint8_t himax_int_gpio_read(int pinnum);
int himax_gpio_power_config(struct himax_platform_data *pdata);
void himax_gpio_power_deconfig(struct himax_platform_data *pdata);

#endif
