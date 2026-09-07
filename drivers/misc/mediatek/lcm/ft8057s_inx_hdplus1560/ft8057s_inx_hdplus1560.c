/*
 * FT8057S INX 720x1560 panel used by k50sv1_64_bsp.
 *
 * Callback behavior was reconstructed from the shipped AArch64 3.18.119
 * kernel. LK independently corroborates the panel identity, parameter
 * constants and physical rail/reset sequence; its callback ABI and suspend
 * path differ. No donor-panel command sequence is carried over.
 *
 * Copyright (C) 2026 k50sv1_64_bsp project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2.
 */

#include <linux/kernel.h>
#include <linux/input/mtk_tpd_power.h>
#include <linux/string.h>

#include "k50_lcm_bias.h"
#include "lcm_drv.h"

#define FRAME_WIDTH	720
#define FRAME_HEIGHT	1560

#define REGFLAG_DELAY		0xFD
#define REGFLAG_END_OF_TABLE	0xFE

struct lcm_setting_table {
	unsigned int cmd;
	unsigned char count;
	unsigned char para_list[64];
};

static LCM_UTIL_FUNCS lcm_util;
static bool lcm_initialized;

#define SET_RESET_PIN(v)	(lcm_util.set_reset_pin((v)))
#define MDELAY(n)		(lcm_util.mdelay((n)))
#define dsi_set_cmdq_V2(cmd, count, data, force) \
	lcm_util.dsi_set_cmdq_V2((cmd), (count), (data), (force))

static const struct lcm_setting_table init_setting[] = {
	{ 0x11, 0, { 0x00 } },
	{ REGFLAG_DELAY, 150, { 0x00 } },
	{ 0x29, 0, { 0x00 } },
	{ REGFLAG_DELAY, 50, { 0x00 } },
	{ REGFLAG_END_OF_TABLE, 0, { 0x00 } },
};

static const struct lcm_setting_table suspend_setting[] = {
	{ 0x28, 1, { 0x00 } },
	{ REGFLAG_DELAY, 10, { 0x00 } },
	{ 0x10, 1, { 0x00 } },
	{ REGFLAG_DELAY, 120, { 0x00 } },
	{ 0x04, 1, { 0x5A } },
	{ 0x05, 1, { 0x5A } },
	{ REGFLAG_END_OF_TABLE, 0, { 0x00 } },
};

static void push_table(const struct lcm_setting_table *table,
		       unsigned int count)
{
	unsigned char parameters[64];
	unsigned int i;

	for (i = 0; i < count; i++) {
		switch (table[i].cmd) {
		case REGFLAG_DELAY:
			MDELAY(table[i].count);
			break;
		case REGFLAG_END_OF_TABLE:
			break;
		default:
			memcpy(parameters, table[i].para_list, table[i].count);
			dsi_set_cmdq_V2(table[i].cmd, table[i].count,
					 parameters, 1);
			break;
		}
	}
}

static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
	memcpy(&lcm_util, util, sizeof(lcm_util));
}

static void lcm_get_params(LCM_PARAMS *params)
{
	memset(params, 0, sizeof(*params));
	params->type = LCM_TYPE_DSI;
	params->width = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;

	params->dsi.mode = SYNC_PULSE_VDO_MODE;
	lcm_dsi_mode = SYNC_PULSE_VDO_MODE;
	params->dsi.DSI_WMEM_CONTI = 0x3C;
	params->dsi.DSI_RMEM_CONTI = 0x3E;
	params->dsi.LANE_NUM = LCM_THREE_LANE;
	params->dsi.data_format.color_order = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format = LCM_DSI_FORMAT_RGB888;
	params->dsi.intermediat_buffer_num = 2;
	params->dsi.PS = LCM_PACKED_PS_24BIT_RGB888;
	params->dsi.packet_size = 256;

	params->dsi.vertical_sync_active = 2;
	params->dsi.vertical_backporch = 32;
	params->dsi.vertical_frontporch = 280;
	params->dsi.vertical_active_line = FRAME_HEIGHT;
	params->dsi.horizontal_sync_active = 18;
	params->dsi.horizontal_backporch = 40;
	params->dsi.horizontal_frontporch = 40;
	params->dsi.horizontal_active_pixel = FRAME_WIDTH;
	params->dsi.PLL_CLOCK = 420;
}

static void lcm_log_bias_error(const char *operation, int error)
{
	if (error)
		pr_err("ft8057s_inx: %s failed: %d\n", operation, error);
}

static void lcm_disable_bias(bool disable_enp2)
{
	int ret;

	ret = lcm_enn_setting(false);
	lcm_log_bias_error("disable ENN", ret);
	MDELAY(10);
	ret = lcm_enp_setting(false);
	lcm_log_bias_error("disable ENP", ret);
	MDELAY(20);
	if (disable_enp2) {
		ret = lcm_enp2_setting(false);
		lcm_log_bias_error("disable ENP2", ret);
	}
}

static int lcm_enable_bias(void)
{
	bool enn_enabled = false;
	bool enp_enabled = false;
	bool enp2_enabled = false;
	int ret;

	ret = lcm_enp2_setting(true);
	if (ret) {
		lcm_log_bias_error("enable ENP2", ret);
		goto fail;
	}
	enp2_enabled = true;
	ret = lcm_enp_setting(true);
	if (ret) {
		lcm_log_bias_error("enable ENP", ret);
		goto fail;
	}
	enp_enabled = true;
	MDELAY(10);
	ret = lcm_enn_setting(true);
	if (ret) {
		lcm_log_bias_error("enable ENN", ret);
		goto fail;
	}
	enn_enabled = true;
	MDELAY(20);
	ret = tps65132_write_bytes(0x00, 0x12);
	if (ret) {
		lcm_log_bias_error("program TPS65132 register 0", ret);
		goto fail;
	}
	ret = tps65132_write_bytes(0x01, 0x12);
	if (ret) {
		lcm_log_bias_error("program TPS65132 register 1", ret);
		goto fail;
	}

	return 0;

fail:
	SET_RESET_PIN(0);
	if (enn_enabled) {
		lcm_log_bias_error("rollback ENN", lcm_enn_setting(false));
		MDELAY(10);
	}
	if (enp_enabled) {
		lcm_log_bias_error("rollback ENP", lcm_enp_setting(false));
		MDELAY(20);
	}
	if (enp2_enabled)
		lcm_log_bias_error("rollback ENP2", lcm_enp2_setting(false));
	return ret;
}

static void lcm_init(void)
{
	lcm_initialized = false;
	if (lcm_enable_bias())
		return;

	SET_RESET_PIN(1);
	MDELAY(10);
	SET_RESET_PIN(0);
	MDELAY(20);
	SET_RESET_PIN(1);
	MDELAY(120);

	push_table(init_setting, ARRAY_SIZE(init_setting));
	lcm_initialized = true;
}

static void lcm_suspend(void)
{
	int ret;

	ret = tpd_prepare_suspend();
	if (ret && ret != -EOPNOTSUPP)
		pr_err("ft8057s_inx: touch suspend prepare failed: %d\n", ret);

	if (lcm_initialized)
		push_table(suspend_setting, ARRAY_SIZE(suspend_setting));
	else
		pr_warn("ft8057s_inx: skip DCS suspend for uninitialized panel\n");
	lcm_initialized = false;
	lcm_disable_bias(false);
	SET_RESET_PIN(0);
	MDELAY(100);
}

static void lcm_resume(void)
{
	lcm_init();
}

LCM_DRIVER ft8057s_inx_hdplus1560_lcm_drv = {
	.name = "ft8057s_inx_hdplus1560",
	.set_util_funcs = lcm_set_util_funcs,
	.get_params = lcm_get_params,
	.init = lcm_init,
	.suspend = lcm_suspend,
	.resume = lcm_resume,
};
