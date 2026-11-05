// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Galaxy A22 5G restart markers consumed by the stock bootloader.
 *
 * Samsung sec_reboot() sets these words to prevent a normal restart with
 * VBUS present from becoming a power-off charging boot. The boot mode itself
 * is not stored here: NORMAL_BOOT=0 is the later bootloader atag,boot value.
 */
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/string.h>

#include "a22x-reboot-policy.h"

/* ram_console + off_linux + last_reboot_reason.is_power_reset */
#define A22X_REBOOT_FLAGS	(0x11d000 + 0x140 + 0x3f4)

static void __iomem *a22x_reboot_flags;

static int a22x_reboot_notify(struct notifier_block *nb,
			      unsigned long mode, void *data)
{
	u32 marker = readl(a22x_reboot_flags);
	u32 reason = readl(a22x_reboot_flags + sizeof(u32));
	u32 next_reason;

	if (!a22x_reboot_reason(data, marker, reason, &next_reason))
		return NOTIFY_DONE;

	writel(A22X_RESET_MAGIC, a22x_reboot_flags);
	writel(next_reason, a22x_reboot_flags + sizeof(u32));
	/* Flush posted writes before handing reset to PSCI or the watchdog. */
	marker = readl(a22x_reboot_flags);
	reason = readl(a22x_reboot_flags + sizeof(u32));
	if (marker != A22X_RESET_MAGIC || reason != next_reason)
		pr_err("a22x-reboot: SRAM readback %08x/%08x, expected %08x/%08x\n",
		       marker, reason, A22X_RESET_MAGIC, next_reason);
	else
		pr_info("a22x-reboot: SRAM restart markers %08x/%08x\n",
			marker, reason);

	/* This handler supplies the reason; the next handler performs reset. */
	return NOTIFY_DONE;
}

static struct notifier_block a22x_reboot_nb = {
	.notifier_call = a22x_reboot_notify,
	/* Run after shutdown, before PSCI (129) and the MTK watchdog (128). */
	.priority = 192,
};

static int __init a22x_reboot_init(void)
{
	int ret;

	if (!of_machine_is_compatible("samsung,a22x"))
		return 0;

	a22x_reboot_flags = ioremap(A22X_REBOOT_FLAGS, 2 * sizeof(u32));
	if (!a22x_reboot_flags)
		return -ENOMEM;

	/* The restart-only chain is never called for power-off or halt. */
	ret = register_restart_handler(&a22x_reboot_nb);
	if (ret) {
		iounmap(a22x_reboot_flags);
		return ret;
	}

	pr_info("a22x-reboot: Samsung SRAM restart handler registered\n");
	return 0;
}
device_initcall(a22x_reboot_init);
