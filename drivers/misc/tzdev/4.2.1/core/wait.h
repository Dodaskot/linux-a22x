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

#ifndef __TZ_WAIT_H__
#define __TZ_WAIT_H__

#include <linux/freezer.h>
#include <linux/version.h>
#include <linux/wait.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
#include <linux/sched.h>
#else
#include <linux/sched/signal.h>
#endif

/* Keep uninterruptible waits freezable without the removed freezer counters. */
#define wait_event_uninterruptible_freezable_nested(wq, condition) \
 wait_event_state(wq, condition, TASK_UNINTERRUPTIBLE | TASK_FREEZABLE)
#define wait_event_interruptible_nested(wq, condition) \
 wait_event_interruptible(wq, condition)

#endif /* __TZ_WAIT_H__ */
