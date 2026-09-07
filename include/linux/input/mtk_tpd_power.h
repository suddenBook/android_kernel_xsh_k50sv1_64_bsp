/*
 * Narrow display-to-touch power sequencing interface for MediaTek TPD.
 *
 * Copyright (C) 2026 k50sv1_64_bsp project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2.
 */

#ifndef _LINUX_INPUT_MTK_TPD_POWER_H
#define _LINUX_INPUT_MTK_TPD_POWER_H

#include <linux/errno.h>

#if defined(CONFIG_TOUCHSCREEN_MTK)
int tpd_prepare_suspend(void);
#else
static inline int tpd_prepare_suspend(void)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _LINUX_INPUT_MTK_TPD_POWER_H */
