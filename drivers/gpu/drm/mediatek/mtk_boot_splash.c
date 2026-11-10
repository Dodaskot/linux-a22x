// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot splash for the A22x recovery: a logo and a progress bar drawn straight into
 * the fbdev buffer from the moment it exists, which is before /init runs. Plymouth
 * does the same from userspace but needs a much larger initramfs and starts later.
 *
 * The picture is the one the recovery UI draws at startup. The UI reads
 * boot_splash_percent, writes boot_splash_stop and carries the bar on to 100%.
 *
 * With CONFIG_DRM_MEDIATEK_BOOT_SPLASH_SWEEP (the system kernel) a light runs to and
 * fro along the track instead, as in LineageOS, until a compositor or greeter becomes
 * DRM master, or for SPLASH_SWEEP_LIMIT_MS at most.
 */
#include <linux/delay.h>
#include <linux/math.h>
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
static struct drm_device *splash_drm;
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

#define SPLASH_SWEEP_LIMIT_MS 300000
#define SWEEP_PERIOD_MS 1800
#define SWEEP_LIGHT 150

/* One frame of the moving light: an eased position, a bright core and soft ends. */
static void splash_sweep(struct fb_info *info, unsigned int elapsed_ms)
{
	unsigned int phase = elapsed_ms % (2 * SWEEP_PERIOD_MS), x, y;
	int travel = BAR_W - SWEEP_LIGHT, centre;
	/* 0..1000 and back, smoothstep-eased so the light slows at both ends. */
	int p = phase < SWEEP_PERIOD_MS ? phase * 1000 / SWEEP_PERIOD_MS
					 : (2 * SWEEP_PERIOD_MS - phase) * 1000 / SWEEP_PERIOD_MS;

	p = p * p * (3000 - 2 * p) / 1000000;
	centre = SWEEP_LIGHT / 2 + travel * p / 1000;
	for (y = 0; y < BAR_H; y++) {
		u32 *row = splash_row(info, BAR_Y + y) + BAR_X;
		unsigned int inset = bar_inset[y < BAR_H / 2 ? BAR_H / 2 - 1 - y : y - BAR_H / 2];

		for (x = inset; x < BAR_W - inset; x++) {
			int d = abs((int)x - centre), a;
			u32 r, g, b;

			if (d >= SWEEP_LIGHT / 2) {
				row[x] = BAR_TRACK;
				continue;
			}
			/* 256 at the core, falling smoothly to 0 at the ends. */
			a = 256 - d * d * 256 / (SWEEP_LIGHT * SWEEP_LIGHT / 4);
			r = (((BAR_FILL >> 16) & 0xff) * a + ((BAR_TRACK >> 16) & 0xff) * (256 - a)) >> 8;
			g = (((BAR_FILL >> 8) & 0xff) * a + ((BAR_TRACK >> 8) & 0xff) * (256 - a)) >> 8;
			b = ((BAR_FILL & 0xff) * a + (BAR_TRACK & 0xff) * (256 - a)) >> 8;
			row[x] = r << 16 | g << 8 | b;
		}
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
		if (IS_ENABLED(CONFIG_DRM_MEDIATEK_BOOT_SPLASH_SWEEP)) {
			if (info && elapsed < SPLASH_SWEEP_LIMIT_MS)
				splash_sweep(info, elapsed);
		} else if (info && elapsed < SPLASH_LIMIT_MS) {
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

/*
 * Shutdown and restart: the system writes this once its compositor has gone (see the
 * a22x-shutdown-splash unit), and the logo with the moving light stays until the reset.
 */
static int boot_splash_show_set(const char *value, const struct kernel_param *param)
{
	struct task_struct *task;

	if (!IS_ENABLED(CONFIG_DRM_MEDIATEK_BOOT_SPLASH_SWEEP) || !splash_drm)
		return -ENODEV;
	mutex_lock(&splash_lock);
	if (!splash_task) {
		task = kthread_run(splash_thread, splash_drm, "boot-splash");
		if (!IS_ERR(task))
			splash_task = task;
	}
	mutex_unlock(&splash_lock);
	return 0;
}

static const struct kernel_param_ops boot_splash_show_ops = {
	.set = boot_splash_show_set,
};
module_param_cb(boot_splash_show, &boot_splash_show_ops, NULL, 0200);
MODULE_PARM_DESC(boot_splash_show, "Write anything to show the splash again, for shutdown");

void mtk_boot_splash_master_set(struct drm_device *drm)
{
	if (IS_ENABLED(CONFIG_DRM_MEDIATEK_BOOT_SPLASH_SWEEP))
		boot_splash_stop_set(NULL, NULL);
}

void mtk_boot_splash_start(struct drm_device *drm)
{
	splash_drm = drm;
	struct task_struct *task = kthread_run(splash_thread, drm, "boot-splash");

	if (IS_ERR(task)) {
		drm_info(drm, "boot splash: no thread\n");
		return;
	}
	mutex_lock(&splash_lock);
	splash_task = task;
	mutex_unlock(&splash_lock);
}
