/*
 * Shared k50sv1 LCD/touch bias GPIO declarations.
 *
 * Copyright (C) 2026 k50sv1_64_bsp project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2.
 */

#ifndef _K50_LCM_BIAS_H
#define _K50_LCM_BIAS_H

#include <linux/types.h>

int lcm_enn_setting(bool enable);
int lcm_enp_setting(bool enable);
int lcm_enp2_setting(bool enable);
int tps65132_write_bytes(u8 reg, u8 value);

#endif /* _K50_LCM_BIAS_H */
