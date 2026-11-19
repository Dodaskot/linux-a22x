/*
 * Copyright (C) 2012-2019, Samsung Electronics Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <tzdev/tzdev.h>

#include "tzdev_internal.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/early_tzar.h"

static struct platform_device *tzdev_pdev;
static atomic_t start_attempted = ATOMIC_INIT(0);

/* Recovery prepares the partition payload in /run before this explicit gate.
 * Never contact SWd from boot-time probe, and never repeat a failed start.
 */
static int tzdev_start_once(const char *val, bool early_bootstrap)
{
	bool start;
	int ret = kstrtobool(val, &start);

	if (ret || !start)
		return ret ?: -EINVAL;
	if (!tzdev_pdev)
		return -ENODEV;
	if (atomic_cmpxchg(&start_attempted, 0, 1))
		return -EALREADY;

	if (early_bootstrap) {
		ret = tzdev_early_tzar_bootstrap();
		if (ret)
			return ret;
	}
	ret = tzdev_run_init_sequence();
	pr_info("tzdev: manual start result=%d early_bootstrap=%u\n", ret, early_bootstrap);
	return ret;
}

static int tzdev_start_set(const char *val, const struct kernel_param *kp)
{
	return tzdev_start_once(val, false);
}

static int tzdev_bootstrap_start_set(const char *val, const struct kernel_param *kp)
{
	return tzdev_start_once(val, true);
}

static const struct kernel_param_ops tzdev_start_ops = {
	.set = tzdev_start_set,
};
module_param_cb(start, &tzdev_start_ops, NULL, 0200);
MODULE_PARM_DESC(start, "Write 1 once after staging a22x-tzar.bin; no boot-time SMC");

static const struct kernel_param_ops tzdev_bootstrap_start_ops = {
	.set = tzdev_bootstrap_start_set,
};
module_param_cb(bootstrap_start, &tzdev_bootstrap_start_ops, NULL, 0200);
MODULE_PARM_DESC(bootstrap_start,
	"Write 1 once on fresh A22x mode2 to hand off exact TZAR before tzdev init; shares start guard");

static int tzdev_probe(struct platform_device *pdev)
{
	log_info(tzdev_platform, "waiting for explicit start after firmware staging\n");

	return 0;
}

static void tzdev_shutdown(struct platform_device *pdev)
{
	(void) pdev;

	if (tzdev_is_up())
		tzdev_run_fini_sequence();

	log_info(tzdev_platform, "tzdev finalization done.\n");
}

struct platform_driver tzdev_pdrv = {
	.probe = tzdev_probe,
	.shutdown = tzdev_shutdown,

	.driver = {
		.name = "tzdev",
		.owner = THIS_MODULE,
	},
};

int tzdev_platform_register(void)
{
	int ret;

	ret = platform_driver_register(&tzdev_pdrv);
	if (ret) {
		log_error(tzdev_platform, "failed to register tzdev platform driver, error=%d\n", ret);
		return ret;
	}

	tzdev_pdev = platform_device_register_resndata(NULL, "tzdev", -1, NULL, 0, NULL, 0);
	if (IS_ERR(tzdev_pdev)) {
		ret = PTR_ERR(tzdev_pdev);
		log_error(tzdev_platform, "failed to register tzdev platform device, error=%d\n", ret);
		return ret;
	}

	return 0;
}

void tzdev_platform_unregister(void)
{
	platform_driver_unregister(&tzdev_pdrv);
}

int tzdev_platform_smc_call(struct tzdev_smc_data *data)
{
	register unsigned long _r0 __asm__(REGISTERS_NAME "0") = data->args[0] | TZDEV_SMC_MAGIC;
	register unsigned long _r1 __asm__(REGISTERS_NAME "1") = data->args[1];
	register unsigned long _r2 __asm__(REGISTERS_NAME "2") = data->args[2];
	register unsigned long _r3 __asm__(REGISTERS_NAME "3") = data->args[3];
	register unsigned long _r4 __asm__(REGISTERS_NAME "4") = data->args[4];
	register unsigned long _r5 __asm__(REGISTERS_NAME "5") = data->args[5];
	register unsigned long _r6 __asm__(REGISTERS_NAME "6") = data->args[6];

	__asm__ __volatile__(ARCH_EXTENSION SMC(0): "+r"(_r0) , "+r" (_r1) , "+r" (_r2),
		"+r" (_r3), "+r" (_r4), "+r" (_r5), "+r" (_r6) : : "memory", PARAM_REGISTERS);

	data->args[0] = _r0;
	data->args[1] = _r1;
	data->args[2] = _r2;
	data->args[3] = _r3;

	return 0;
}

void *tzdev_platform_open(void)
{
	return NULL;
}

void tzdev_platform_release(void *data)
{
	return;
}

long tzdev_platform_ioctl(void *data, unsigned int cmd, unsigned long arg)
{
	(void) data;
	(void) cmd;
	(void) arg;

	return -ENOTTY;
}
