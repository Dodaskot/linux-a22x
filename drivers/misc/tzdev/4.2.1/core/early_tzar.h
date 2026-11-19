/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TZDEV_EARLY_TZAR_H
#define TZDEV_EARLY_TZAR_H
#include <linux/types.h>

int tzdev_early_tzar_bootstrap(void);
bool tzdev_early_tzar_handed_off(void);

#endif
