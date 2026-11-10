// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot splash for the A22x recovery: a logo and a progress bar drawn straight into
 * the fbdev buffer from the moment it exists, which is before /init runs. Plymouth
 * does the same from userspace but needs a much larger initramfs and starts later.
 *
 * The picture is the one the recovery UI draws at startup. The UI reads
 * boot_splash_percent, writes boot_splash_stop and carries the bar on to 100%.
 */
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include <drm/drm_device.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_print.h>

#include "mtk_boot_splash.h"
#include "mtk_boot_splash_logo.h"

#define SPLASH_WIDTH 1080
#define SPLASH_HEIGHT 2408
#define BAR_X 270
#define BAR_Y 1474
#define BAR_W 540
#define BAR_H 12
#define BAR_TRACK 0x282c34
#define BAR_FILL 0xff9a4b
#define SPLASH_LIMIT_MS 15000

/* Rounded ends: how far each of the six rows from the middle starts inside the bar. */
static const u8 bar_inset[BAR_H / 2] = { 0, 0, 1, 1, 2, 4 };

static DEFINE_MUTEX(splash_lock);
static struct task_struct *splash_task;
static unsigned int boot_splash_percent;
module_param(boot_splash_percent, uint, 0444);
MODULE_PARM_DESC(boot_splash_percent, "How far the boot splash bar has come");

static u32 *splash_row(struct fb_info *info, unsigned int y)
{
	return (u32 *)((u8 *)info->screen_buffer +
		       (size_t)(y + info->var.yoffset) * info->fix.line_length);
}

static void splash_picture(struct fb_info *info)
{
	const u8 *logo = splash_logo;
	unsigned int x, y;

	for (y = 0; y < SPLASH_HEIGHT; y++)
		memset32(splash_row(info, y), SPLASH_BACKGROUND, SPLASH_WIDTH);
	for (y = 0; y < SPLASH_LOGO_H; y++) {
		u32 *row = splash_row(info, SPLASH_LOGO_Y + y) + SPLASH_LOGO_X;

		for (x = 0; x < SPLASH_LOGO_W; x++, logo += 3)
			row[x] = logo[0] << 16 | logo[1] << 8 | logo[2];
	}
}

static void splash_bar(struct fb_info *info, unsigned int percent)
{
	unsigned int filled = BAR_W * percent / 100, x, y;

	for (y = 0; y < BAR_H; y++) {
		u32 *row = splash_row(info, BAR_Y + y) + BAR_X;
		unsigned int inset = bar_inset[y < BAR_H / 2 ? BAR_H / 2 - 1 - y : y - BAR_H / 2];

		for (x = inset; x < BAR_W - inset; x++)
			row[x] = x < filled ? BAR_FILL : BAR_TRACK;
	}
}

/* Boot time is not known in advance, so the bar slows down and never quite arrives. */
static unsigned int splash_progress(unsigned int elapsed_ms)
{
	return 90 - 90 * 2500 / (elapsed_ms + 2500);
}

static int splash_thread(void *data)
{
	struct drm_device *drm = data;
	unsigned long start = jiffies;
	struct fb_info *info = NULL;

	while (!kthread_should_stop()) {
		unsigned int elapsed = jiffies_to_msecs(jiffies - start);

		if (!info) {
			struct drm_fb_helper *helper = drm->fb_helper;

			if (helper && helper->info && helper->info->screen_buffer) {
				info = helper->info;
				if (info->var.bits_per_pixel != 32 || info->var.xres != SPLASH_WIDTH ||
				    info->var.yres < SPLASH_HEIGHT) {
					drm_info(drm, "boot splash: unexpected framebuffer\n");
					break;
				}
				splash_picture(info);
			}
		}
		if (info && elapsed < SPLASH_LIMIT_MS) {
			boot_splash_percent = splash_progress(elapsed);
			splash_bar(info, boot_splash_percent);
		}
		msleep(33);
	}
	while (!kthread_should_stop())
		msleep(100);
	return 0;
}

/* Returning means nothing in the kernel will touch the framebuffer any more. */
static int boot_splash_stop_set(const char *value, const struct kernel_param *param)
{
	mutex_lock(&splash_lock);
	if (splash_task) {
		kthread_stop(splash_task);
		splash_task = NULL;
	}
	mutex_unlock(&splash_lock);
	return 0;
}

static const struct kernel_param_ops boot_splash_stop_ops = {
	.set = boot_splash_stop_set,
};
module_param_cb(boot_splash_stop, &boot_splash_stop_ops, NULL, 0200);
MODULE_PARM_DESC(boot_splash_stop, "Write anything to take the screen from the boot splash");

void mtk_boot_splash_start(struct drm_device *drm)
{
	struct task_struct *task = kthread_run(splash_thread, drm, "boot-splash");

	if (IS_ERR(task)) {
		drm_info(drm, "boot splash: no thread\n");
		return;
	}
	mutex_lock(&splash_lock);
	splash_task = task;
	mutex_unlock(&splash_lock);
}
