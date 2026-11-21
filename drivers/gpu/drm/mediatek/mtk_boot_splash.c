// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot splash for the A22x recovery: a logo and a progress bar drawn straight into
 * the fbdev buffer from the moment it exists, which is before /init runs. Plymouth
 * does the same from userspace but needs a much larger initramfs and starts later.
 *
 * The picture is the one the recovery UI draws at startup, in one of its built-in themes:
 * /init writes the saved theme to boot_splash_theme early, and the first picture waits up
 * to SPLASH_THEME_WAIT_MS for it (Graphite otherwise). The UI reads boot_splash_percent,
 * writes boot_splash_stop and carries the bar on to 100% in the same theme.
 *
 * The theme's own picture comes first (2026-11-21): the recovery keeps the splash of the theme in
 * use in misc (boot_splash.py, misc_store.py), and it is read here as soon as the eMMC is there,
 * long before the display, then drawn the moment the framebuffer exists, without waiting for
 * /init (the screen stayed black for about 1.4 s between the bootloader and the recovery). Only
 * without a whole record is a built-in theme drawn. boot_splash_source says which: loading, misc
 * or builtin. The record is read and unpacked by a thread of its own from the driver's init, so
 * the picture is ready when the framebuffer comes (read after it, it came 0.13 s late).
 */
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include <linux/zlib.h>

#include <crypto/sha2.h>

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
#define SPLASH_LIMIT_MS 15000
#define SPLASH_THEME_WAIT_MS 400

/* Rounded ends: how far each of the six rows from the middle starts inside the bar. */
static const u8 bar_inset[BAR_H / 2] = { 0, 0, 1, 1, 2, 4 };

static DEFINE_MUTEX(splash_lock);
static struct task_struct *splash_task;
static unsigned int boot_splash_percent;
module_param(boot_splash_percent, uint, 0444);
MODULE_PARM_DESC(boot_splash_percent, "How far the boot splash bar has come");

static unsigned int splash_theme;
static bool splash_theme_known;
static bool splash_theme_changed;

static int boot_splash_theme_set(const char *value, const struct kernel_param *param)
{
	unsigned int n;

	for (n = 0; n < SPLASH_THEMES; n++) {
		if (sysfs_streq(value, splash_theme_names[n])) {
			WRITE_ONCE(splash_theme, n);
			WRITE_ONCE(splash_theme_changed, true);
			WRITE_ONCE(splash_theme_known, true);
			return 0;
		}
	}
	return -EINVAL;
}

static int boot_splash_theme_get(char *buffer, const struct kernel_param *param)
{
	return sysfs_emit(buffer, "%s\n", splash_theme_names[READ_ONCE(splash_theme)]);
}

static const struct kernel_param_ops boot_splash_theme_ops = {
	.set = boot_splash_theme_set,
	.get = boot_splash_theme_get,
};
module_param_cb(boot_splash_theme, &boot_splash_theme_ops, NULL, 0644);
MODULE_PARM_DESC(boot_splash_theme, "Theme of the boot splash: graphite, daylight or aurora");

/*
 * The record in misc of mmcblk0: two slots of 128 KiB at 128 KiB, each a header of 32 bytes
 * ('A22XMSC1', 'splash', generation, length, flags 0) and the SHA-256 of that header and the
 * payload, the payload from 512; the newest whole copy counts. The payload (boot_splash.py):
 * 'A22XSPL1', the frame's size, the bar, its colors, a background grid stretched back bilinearly,
 * and a box of pixels over it (a byte per pixel of the box: 1 where its color follows).
 */
#define MISC_AREA (128 << 10)
#define MISC_SLOT (128 << 10)
#define MISC_SECTOR 512
#define PICTURE_HEAD 80

enum { SOURCE_LOADING, SOURCE_MISC, SOURCE_BUILTIN };
static int splash_source;
#define MISC_WAIT_MS 10000
/* The picture the loader made, until the splash thread takes it (or a stop drops it). */
static DEFINE_MUTEX(misc_lock);
static u32 *misc_frame, misc_track, misc_fill;
static bool misc_dropped;

static int boot_splash_source_get(char *buffer, const struct kernel_param *param)
{
	static const char *const names[] = { "loading", "misc", "builtin" };

	return sysfs_emit(buffer, "%s\n", names[READ_ONCE(splash_source)]);
}

static const struct kernel_param_ops boot_splash_source_ops = {
	.get = boot_splash_source_get,
};
module_param_cb(boot_splash_source, &boot_splash_source_ops, NULL, 0444);
MODULE_PARM_DESC(boot_splash_source, "Where the boot splash picture came from: loading, misc or builtin");

static int misc_match(struct device *dev, const void *data)
{
	struct block_device *bdev = dev_to_bdev(dev);

	return bdev_is_partition(bdev) && bdev->bd_meta_info &&
	       !strcmp((const char *)bdev->bd_meta_info->volname, "misc") &&
	       !strcmp(bdev->bd_disk->disk_name, "mmcblk0");
}

/* The payload of the newest whole copy (kvfree it), NULL without one; -ENODEV: no misc yet. */
static u8 *misc_record(size_t *length, int *error)
{
	struct device *dev = class_find_device(&block_class, NULL, NULL, misc_match);
	u8 *slots, *found = NULL, *payload = NULL;
	struct file *file;
	loff_t pos = MISC_AREA;
	u64 newest = 0;
	u32 size = 0;
	dev_t devt;
	int n;

	*error = -ENODEV;
	if (!dev)
		return NULL;
	devt = dev->devt;
	put_device(dev);
	*error = -EIO;
	file = bdev_file_open_by_dev(devt, BLK_OPEN_READ, NULL, NULL);
	if (IS_ERR(file))
		return NULL;
	slots = kvmalloc(2 * MISC_SLOT, GFP_KERNEL);
	if (slots && kernel_read(file, slots, 2 * MISC_SLOT, &pos) != 2 * MISC_SLOT) {
		kvfree(slots);
		slots = NULL;
	}
	fput(file);
	if (!slots)
		return NULL;
	for (n = 0; n < 2; n++) {
		u8 *slot = slots + n * MISC_SLOT, digest[SHA256_DIGEST_SIZE];
		u64 generation = get_unaligned_le64(slot + 16);
		u32 length_here = get_unaligned_le32(slot + 24);
		struct sha256_ctx ctx;

		if (memcmp(slot, "A22XMSC1", 8) || memcmp(slot + 8, "splash\0\0", 8) ||
		    get_unaligned_le32(slot + 28) || length_here > MISC_SLOT - MISC_SECTOR)
			continue;
		sha256_init(&ctx);
		sha256_update(&ctx, slot, 32);
		sha256_update(&ctx, slot + MISC_SECTOR, length_here);
		sha256_final(&ctx, digest);
		if (memcmp(digest, slot + 32, SHA256_DIGEST_SIZE))
			continue;
		if (!found || generation > newest) {
			found = slot + MISC_SECTOR;
			newest = generation;
			size = length_here;
		}
	}
	*error = -ENOENT;
	if (found) {
		payload = kvmalloc(size, GFP_KERNEL);
		if (payload) {
			memcpy(payload, found, size);
			*length = size;
			*error = 0;
		}
	}
	kvfree(slots);
	return payload;
}

/* For every pixel of SIZE: its grid cell, the next one and the next one's weight of 65536,
 * the cells' centres at ((2n + 1) * CELLS - SIZE) / (2 * SIZE), as boot_splash.py computes it. */
static void splash_taps(unsigned int size, unsigned int cells, u16 *first, u16 *second, u32 *weight)
{
	unsigned int n;

	for (n = 0; n < size; n++) {
		s64 at = div_s64(((s64)(2 * n + 1) * cells - size) * 65536, 2 * size);
		u32 cell = at > 0 ? (u32)(at >> 16) : 0;

		first[n] = second[n] = cell;
		weight[n] = 0;
		if (at > 0 && cell < cells - 1) {
			second[n] = cell + 1;
			weight[n] = at & 0xffff;
		}
	}
}

/* The frame of a payload (vfree it), with the bar's colors; NULL when it is not one. */
static u32 *misc_picture(const u8 *p, size_t length, u32 *track, u32 *fill)
{
	unsigned int gw, gh, bx, by, bw, bh, x, y, set = 0;
	u8 *grid = NULL, *box = NULL;
	u16 *xa = NULL, *xb = NULL, *ya = NULL, *yb = NULL;
	u32 *xw = NULL, *yw = NULL, *frame = NULL;
	u32 grid_len, box_len;
	int got;

	if (length < PICTURE_HEAD || memcmp(p, "A22XSPL1", 8) || get_unaligned_le16(p + 8) != SPLASH_WIDTH ||
	    get_unaligned_le16(p + 10) != SPLASH_HEIGHT)
		return NULL;
	*track = get_unaligned_le32(p + 20);
	*fill = get_unaligned_le32(p + 24);
	gw = get_unaligned_le16(p + 28);
	gh = get_unaligned_le16(p + 30);
	bx = get_unaligned_le16(p + 32);
	by = get_unaligned_le16(p + 34);
	bw = get_unaligned_le16(p + 36);
	bh = get_unaligned_le16(p + 38);
	grid_len = get_unaligned_le32(p + 40);
	box_len = get_unaligned_le32(p + 44);
	if (!gw || gw > SPLASH_WIDTH || !gh || gh > SPLASH_HEIGHT || !bw || !bh || bx + bw > SPLASH_WIDTH ||
	    by + bh > SPLASH_HEIGHT || *track >> 24 || *fill >> 24 ||
	    (u64)PICTURE_HEAD + grid_len + box_len != length)
		return NULL;

	grid = kvmalloc(gw * gh * 3, GFP_KERNEL);
	box = kvmalloc(bw * bh * 4, GFP_KERNEL);
	xa = kmalloc_array(SPLASH_WIDTH, sizeof(*xa), GFP_KERNEL);
	xb = kmalloc_array(SPLASH_WIDTH, sizeof(*xb), GFP_KERNEL);
	xw = kmalloc_array(SPLASH_WIDTH, sizeof(*xw), GFP_KERNEL);
	ya = kmalloc_array(SPLASH_HEIGHT, sizeof(*ya), GFP_KERNEL);
	yb = kmalloc_array(SPLASH_HEIGHT, sizeof(*yb), GFP_KERNEL);
	yw = kmalloc_array(SPLASH_HEIGHT, sizeof(*yw), GFP_KERNEL);
	frame = vmalloc(array3_size(SPLASH_WIDTH, SPLASH_HEIGHT, sizeof(u32)));
	if (!grid || !box || !xa || !xb || !xw || !ya || !yb || !yw || !frame)
		goto refused;
	if (zlib_inflate_blob(grid, gw * gh * 3, p + PICTURE_HEAD, grid_len) != (int)(gw * gh * 3))
		goto refused;
	got = zlib_inflate_blob(box, bw * bh * 4, p + PICTURE_HEAD + grid_len, box_len);
	if (got < (int)(bw * bh))
		goto refused;
	for (x = 0; x < bw * bh; x++) {
		if (box[x] > 1)
			goto refused;
		set += box[x];
	}
	if (got != (int)(bw * bh + 3 * set))
		goto refused;

	splash_taps(SPLASH_WIDTH, gw, xa, xb, xw);
	splash_taps(SPLASH_HEIGHT, gh, ya, yb, yw);
	for (y = 0; y < SPLASH_HEIGHT; y++) {
		const u8 *top = grid + ya[y] * gw * 3, *bottom = grid + yb[y] * gw * 3;
		u32 *row = frame + y * SPLASH_WIDTH;
		u64 fy = yw[y];

		for (x = 0; x < SPLASH_WIDTH; x++) {
			u32 fx = xw[x], color = 0;
			unsigned int k;

			for (k = 0; k < 3; k++) {
				u64 upper = top[xa[x] * 3 + k] * (65536 - fx) + top[xb[x] * 3 + k] * fx;
				u64 lower = bottom[xa[x] * 3 + k] * (65536 - fx) + bottom[xb[x] * 3 + k] * fx;

				color = color << 8 | (u32)((upper * (65536 - fy) + lower * fy + (1ULL << 31)) >> 32);
			}
			row[x] = color;
		}
	}
	{
		const u8 *colors = box + bw * bh;

		for (y = 0; y < bh; y++) {
			u32 *row = frame + (by + y) * SPLASH_WIDTH + bx;

			for (x = 0; x < bw; x++, row++) {
				if (!box[y * bw + x])
					continue;
				*row = colors[0] << 16 | colors[1] << 8 | colors[2];
				colors += 3;
			}
		}
	}
	goto done;
refused:
	vfree(frame);
	frame = NULL;
done:
	kfree(xa);
	kfree(xb);
	kfree(xw);
	kfree(ya);
	kfree(yb);
	kfree(yw);
	kvfree(box);
	kvfree(grid);
	return frame;
}

/* The theme's picture from misc: 0 with FRAME, -ENODEV while misc is not there yet, else none. */
static int misc_splash(u32 **frame, u32 *track, u32 *fill)
{
	size_t length = 0;
	int error;
	u8 *payload = misc_record(&length, &error);

	if (!payload)
		return error;
	*frame = misc_picture(payload, length, track, fill);
	kvfree(payload);
	return *frame ? 0 : -EINVAL;
}

static int misc_loader(void *unused)
{
	unsigned long start = jiffies;
	u32 *frame = NULL, track = 0, fill = 0;
	int error;

	for (;;) {
		error = misc_splash(&frame, &track, &fill);
		if (error != -ENODEV || jiffies_to_msecs(jiffies - start) >= MISC_WAIT_MS)
			break;
		msleep(20);
	}
	mutex_lock(&misc_lock);
	if (!error && !misc_dropped) {
		misc_frame = frame;
		misc_track = track;
		misc_fill = fill;
		frame = NULL;
	}
	mutex_unlock(&misc_lock);
	vfree(frame);
	WRITE_ONCE(splash_source, error ? SOURCE_BUILTIN : SOURCE_MISC);
	if (error)
		pr_info("[drm] boot splash: no picture in misc (%d)\n", error);
	else
		pr_info("[drm] boot splash: the theme's picture from misc, ready\n");
	return 0;
}

void mtk_boot_splash_prefetch(void)
{
	if (IS_ERR(kthread_run(misc_loader, NULL, "boot-splash-misc")))
		WRITE_ONCE(splash_source, SOURCE_BUILTIN);
}

static u32 *splash_row(struct fb_info *info, unsigned int y)
{
	return (u32 *)((u8 *)info->screen_buffer +
		       (size_t)(y + info->var.yoffset) * info->fix.line_length);
}

static void splash_picture(struct fb_info *info, unsigned int theme)
{
	const u8 *logo = splash_logos[theme];
	unsigned int x, y;

	for (y = 0; y < SPLASH_HEIGHT; y++)
		memset32(splash_row(info, y), splash_backgrounds[theme], SPLASH_WIDTH);
	for (y = 0; y < SPLASH_LOGO_H; y++) {
		u32 *row = splash_row(info, SPLASH_LOGO_Y + y) + SPLASH_LOGO_X;

		for (x = 0; x < SPLASH_LOGO_W; x++, logo += 3)
			row[x] = logo[0] << 16 | logo[1] << 8 | logo[2];
	}
}

static void splash_bar(struct fb_info *info, u32 track, u32 fill, unsigned int percent)
{
	unsigned int filled = BAR_W * percent / 100, x, y;

	for (y = 0; y < BAR_H; y++) {
		u32 *row = splash_row(info, BAR_Y + y) + BAR_X;
		unsigned int inset = bar_inset[y < BAR_H / 2 ? BAR_H / 2 - 1 - y : y - BAR_H / 2];

		for (x = inset; x < BAR_W - inset; x++)
			row[x] = x < filled ? fill : track;
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
	unsigned int ready = 0, theme = 0, y;
	struct fb_info *info = NULL;
	u32 *picture = NULL, track = 0, fill = 0;
	bool drawn = false;

	while (!kthread_should_stop()) {
		unsigned int elapsed = jiffies_to_msecs(jiffies - start);

		/* The theme's picture, once the loader has it. */
		if (!drawn && info && READ_ONCE(splash_source) == SOURCE_MISC) {
			mutex_lock(&misc_lock);
			picture = misc_frame;
			misc_frame = NULL;
			track = misc_track;
			fill = misc_fill;
			mutex_unlock(&misc_lock);
			if (picture)
				drm_info(drm, "boot splash: the theme's picture from misc\n");
		}

		if (!info) {
			struct drm_fb_helper *helper = drm->fb_helper;

			if (helper && helper->info && helper->info->screen_buffer) {
				info = helper->info;
				ready = elapsed;
				if (info->var.bits_per_pixel != 32 || info->var.xres != SPLASH_WIDTH ||
				    info->var.yres < SPLASH_HEIGHT) {
					drm_info(drm, "boot splash: unexpected framebuffer\n");
					break;
				}
			}
		}
		/* The theme's picture at once; it is the saved theme, so a name given later changes nothing. */
		if (info && picture) {
			for (y = 0; y < SPLASH_HEIGHT; y++)
				memcpy(splash_row(info, y), picture + y * SPLASH_WIDTH, SPLASH_WIDTH * sizeof(u32));
			vfree(picture);
			picture = NULL;
			drawn = true;
		}
		/* Built-in: the first picture waits a moment for the saved theme; a later one redraws. */
		if (info && READ_ONCE(splash_source) == SOURCE_BUILTIN &&
		    (READ_ONCE(splash_theme_changed) || (!drawn && elapsed - ready >= SPLASH_THEME_WAIT_MS))) {
			WRITE_ONCE(splash_theme_changed, false);
			theme = READ_ONCE(splash_theme);
			track = splash_tracks[theme];
			fill = splash_fills[theme];
			splash_picture(info, theme);
			if (!drawn)
				drm_info(drm, "boot splash: %s theme%s\n", splash_theme_names[theme],
					 READ_ONCE(splash_theme_known) ? "" : " (none given)");
			drawn = true;
		}
		if (drawn && elapsed < SPLASH_LIMIT_MS) {
			boot_splash_percent = splash_progress(elapsed);
			splash_bar(info, track, fill, boot_splash_percent);
		}
		msleep(33);
	}
	vfree(picture);
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
	mutex_lock(&misc_lock);
	misc_dropped = true;
	vfree(misc_frame);
	misc_frame = NULL;
	mutex_unlock(&misc_lock);
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
