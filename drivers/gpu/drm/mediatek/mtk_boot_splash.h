/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MTK_BOOT_SPLASH_H
#define MTK_BOOT_SPLASH_H

struct drm_device;

#if IS_ENABLED(CONFIG_DRM_MEDIATEK_BOOT_SPLASH)
void mtk_boot_splash_start(struct drm_device *drm);
#else
static inline void mtk_boot_splash_start(struct drm_device *drm) {}
#endif

#endif
