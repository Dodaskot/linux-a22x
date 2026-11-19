// SPDX-License-Identifier: GPL-2.0
/* One read-only service-readiness handshake, without loading or invoking a TA.
 * This intentionally does not use the vendor TEEC helper's connection retries.
 */
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <tzdev/tzdev.h>

#include "core/early_tzar.h"
#include "core/iwsock.h"
#include "teec/iw_messages.h"

static atomic_t probe_attempted = ATOMIC_INIT(0);
static int root_task_probe_result = -EAGAIN;
module_param(root_task_probe_result, int, 0444);
MODULE_PARM_DESC(root_task_probe_result,
	"One root_task initialize/finalize handshake result, after successful early handoff and tzdev init");

static int bootstrap_probe(void)
{
	struct sock_desc *sd;
	struct cmd_initialize_context init = {
		.base = { .cmd = CMD_INITIALIZE_CONTEXT, .serial = 1 },
	};
	struct cmd_finalize_context fini = {
		.base = { .cmd = CMD_FINALIZE_CONTEXT, .serial = 2 },
	};
	struct cmd_reply_initialize_context init_reply = { };
	struct cmd_reply_finalize_context fini_reply = { };
	int ret;

	BUILD_BUG_ON(sizeof(init) != 8);
	BUILD_BUG_ON(sizeof(init_reply) != 24);
	BUILD_BUG_ON(sizeof(fini) != 8);
	BUILD_BUG_ON(sizeof(fini_reply) != 16);
	sd = tz_iwsock_socket(1, TZ_NON_INTERRUPTIBLE);
	if (IS_ERR(sd))
		return PTR_ERR(sd);
	sd->timeout_jiffies = msecs_to_jiffies(10000);
	ret = tz_iwsock_connect(sd, ROOT_TASK_SOCK, 0);
	if (ret)
		goto out;
	ret = tz_iwsock_write(sd, &init, sizeof(init), 0);
	if (ret != sizeof(init)) {
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}
	ret = tz_iwsock_read(sd, &init_reply, sizeof(init_reply), 0);
	if (ret != sizeof(init_reply)) {
		ret = ret < 0 ? ret : -EPROTO;
		goto out;
	}
	pr_info("tzdev: root_task init reply cmd=%u serial=%u result=0x%x origin=%u\n",
		init_reply.base.base.cmd, init_reply.base.base.serial,
		init_reply.base.result, init_reply.base.origin);
	if (init_reply.base.base.cmd != CMD_REPLY_INITIALIZE_CONTEXT ||
	    init_reply.base.base.serial != init.base.serial || init_reply.base.result) {
		ret = -EPROTO;
		goto out;
	}
	ret = tz_iwsock_write(sd, &fini, sizeof(fini), 0);
	if (ret != sizeof(fini)) {
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}
	ret = tz_iwsock_read(sd, &fini_reply, sizeof(fini_reply), 0);
	if (ret != sizeof(fini_reply)) {
		ret = ret < 0 ? ret : -EPROTO;
		goto out;
	}
	pr_info("tzdev: root_task finalize reply cmd=%u serial=%u result=0x%x origin=%u\n",
		fini_reply.base.base.cmd, fini_reply.base.base.serial,
		fini_reply.base.result, fini_reply.base.origin);
	if (fini_reply.base.base.cmd != CMD_REPLY_FINALIZE_CONTEXT ||
	    fini_reply.base.base.serial != fini.base.serial || fini_reply.base.result) {
		ret = -EPROTO;
		goto out;
	}
	ret = 0;
	pr_info("tzdev: Startup tzar deployment done (early attach; root_task context round-trip verified).\n");
out:
	tz_iwsock_release(sd);
	return ret;
}

static int root_task_probe_set(const char *val, const struct kernel_param *kp)
{
	bool start;
	int ret = kstrtobool(val, &start);

	if (ret || !start)
		return ret ?: -EINVAL;
	if (!tzdev_early_tzar_handed_off() || !tzdev_is_up())
		return -EPERM;
	if (atomic_cmpxchg(&probe_attempted, 0, 1))
		return -EALREADY;
	WRITE_ONCE(root_task_probe_result, -EINPROGRESS);
	ret = bootstrap_probe();
	WRITE_ONCE(root_task_probe_result, ret);
	pr_info("tzdev: root_task probe result=%d; no retry this boot\n", ret);
	return ret;
}

static const struct kernel_param_ops root_task_probe_ops = {
	.set = root_task_probe_set,
};
module_param_cb(root_task_probe, &root_task_probe_ops, NULL, 0200);
