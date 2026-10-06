/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_MT6833_LPM_H
#define __LINUX_MT6833_LPM_H

#include <linux/kconfig.h>
#include <linux/types.h>

#define MT6833_LPM_SUSPEND_STATE 0x1010100

#if IS_ENABLED(CONFIG_MTK_MT6833_LPM)
bool mt6833_lpm_s2idle_active(void);
void mt6833_lpm_s2idle_enter(u32 state);
void mt6833_lpm_s2idle_exit(u32 state, int result);
#endif

#endif
