/*
 * Copyright (C) 2012-2019 Samsung Electronics, Inc.
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

#include <linux/jiffies.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include "tz_common.h"
#include "core/deploy_tzar.h"
#include "core/early_tzar.h"
#include "core/iwsock.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/subsystem.h"
#include "teec/iw_messages.h"

#define USERBOOT_WAIT_MS 10000

#define TZAR_FIRMWARE "a22x-tzar.bin"
#define TZAR_MAX_SIZE SZ_16M

static int deploy_result = -EAGAIN;
module_param(deploy_result, int, 0444);
MODULE_PARM_DESC(deploy_result, "IW result: -EAGAIN before start, -EINPROGRESS active, 0 valid SWd reply, -EOPNOTSUPP for separate early-attach path");

enum iw_userboot_cmd_type {
	CMD_USERBOOT_INVALID_ID,
	CMD_USERBOOT_UPLOAD_CONTAINER,
	CMD_USERBOOT_REPLY,
	CMD_USERBOOT_REPLY_UPLOAD_CONTAINER,
	CMD_USERBOOT_MAX
};

struct cmd_userboot_upload_container {
	struct cmd_request base;
	struct shared_buffer_description buf_desc;
} IW_STRUCTURE;

struct cmd_userboot_reply_upload_container {
	struct cmd_reply base;
} IW_STRUCTURE;

static void tzdev_tzar_free(void *ptr)
{
	vfree(ptr);
}

static int tz_deploy_payload(const struct firmware *fw)
{
	int ret;
	size_t size;
	struct sock_desc *sd;
	struct cmd_userboot_upload_container command = {
		.base = { .cmd = CMD_USERBOOT_UPLOAD_CONTAINER, .serial = 1 },
	};
	struct cmd_userboot_reply_upload_container reply = { };
	void *tzar;

	BUILD_BUG_ON(sizeof(command) != 20);
	BUILD_BUG_ON(sizeof(reply) != 16);
	size = fw->size;
	if (!size || size > TZAR_MAX_SIZE) {
		release_firmware(fw);
		return -EINVAL;
	}
	log_info(tzdev_deploy_tzar, "firmware %s payload=%zu\n", TZAR_FIRMWARE, size);

	tzar = vzalloc(PAGE_ALIGN(size));
	if (!tzar) {
		release_firmware(fw);
		log_error(tzdev_deploy_tzar, "Failed to allocate memory for tzar\n");
		return -ENOMEM;
	}

	memcpy(tzar, fw->data, size);
	release_firmware(fw);

	/* The registration owns the buffer until SWd confirms its release. */
	ret = tzdev_mem_register(tzar, PAGE_ALIGN(size), 0, tzdev_tzar_free, tzar);
	if (ret < 0) {
		log_error(tzdev_deploy_tzar, "Failed to register shared memory, error=%d\n", ret);
		goto out_tzar;
	}

	command.buf_desc.id = ret;
	command.buf_desc.size = size;

	sd = tz_iwsock_socket(1, TZ_NON_INTERRUPTIBLE);
	if (IS_ERR(sd)) {
		ret = PTR_ERR(sd);
		log_error(tzdev_deploy_tzar, "Failed to create IW socket, error=%d\n", ret);
		goto out_mem;
	}

	/* A timed-out operation is not replayed. SMC execution itself remains
	 * outside the scheduler's ability to enforce a deadline.
	 */
	sd->timeout_jiffies = msecs_to_jiffies(USERBOOT_WAIT_MS);
	ret = tz_iwsock_connect(sd, USERBOOT_SOCK, 0);
	if (ret < 0) {
		log_error(tzdev_deploy_tzar, "Failed to connect SWd, error=%d\n", ret);
		goto out_sock;
	}

	ret = tz_iwsock_write(sd, &command, sizeof(command), 0);
	if (ret != sizeof(command)) {
		log_error(tzdev_deploy_tzar, "Failed to write socket, error=%d\n", ret);
		ret = ret < 0 ? ret : -EIO;
		goto out_sock;
	}

	ret = tz_iwsock_read(sd, &reply, sizeof(reply), 0);
	if (ret != sizeof(reply)) {
		log_error(tzdev_deploy_tzar, "Failed to read socket, error=%d\n", ret);
		ret = ret < 0 ? ret : -EPROTO;
		goto out_sock;
	}
	log_info(tzdev_deploy_tzar, "userboot reply cmd=%u serial=%u result=0x%x origin=%u\n",
		 reply.base.base.cmd, reply.base.base.serial, reply.base.result, reply.base.origin);
	if (reply.base.base.cmd != CMD_USERBOOT_REPLY_UPLOAD_CONTAINER ||
	    reply.base.base.serial != command.base.serial || reply.base.result) {
		ret = -EPROTO;
		goto out_sock;
	}

	ret = 0;
	log_info(tzdev_deploy_tzar, "Startup tzar deployment done.\n");

out_sock:
	tz_iwsock_release(sd);
out_mem:
	tzdev_mem_release(command.buf_desc.id);
	return ret;
out_tzar:
	vfree(tzar);

	return ret;
}

static int tz_deploy_handler(void *arg)
{
	int ret = tz_deploy_payload(arg);

	WRITE_ONCE(deploy_result, ret);
	return ret;
}

int tzdev_deploy_tzar(void)
{
	struct task_struct *t;
	const struct firmware *fw;
	int ret;

	if (tzdev_early_tzar_handed_off()) {
		/* SMC .14 spawned userboot with the compressed container. A second
		 * IW upload is neither the same format nor a completion query.
		 */
		WRITE_ONCE(deploy_result, -EOPNOTSUPP);
		pr_info("tzdev: early attach selected; no duplicate IW upload; verify secure bootstrap log\n");
		return 0;
	}
	WRITE_ONCE(deploy_result, -EINPROGRESS);
	/* Acquire the bytes before the sysfs start returns and userspace restores
	 * firmware_class.path. The worker owns this reference from then on.
	 */
	ret = request_firmware(&fw, TZAR_FIRMWARE, NULL);
	if (ret) {
		WRITE_ONCE(deploy_result, ret);
		log_error(tzdev_deploy_tzar, "request_firmware failed: %d\n", ret);
		return ret;
	}
	t = kthread_run(tz_deploy_handler, (void *)fw, "tzdev_deploy_tzar");
	if (IS_ERR(t)) {
		release_firmware(fw);
		WRITE_ONCE(deploy_result, PTR_ERR(t));
		log_error(tzdev_deploy_tzar, "Failed to create kernel thread, error=%ld\n", PTR_ERR(t));
		return PTR_ERR(t);
	}
	return 0;
}

int tz_deploy_tzar_init(void)
{
	tzdev_set_nwd_sysconf_flag(SYSCONF_NWD_TZDEV_DEPLOY_TZAR);

	return 0;
}

tzdev_early_initcall(tz_deploy_tzar_init);
