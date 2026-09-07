/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Android-Q charging-control adapter for the fitted PSC5415. Successful
 * register behavior follows the shipped MT6750/MT6755 3.18.119 kernel.
 * CV requests round down to the documented voltage groups, with the
 * board's existing 4.35 V maximum retained.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/reboot.h>
#include <linux/types.h>

#include <mach/mt_pmic.h>
#include <mt-plat/battery_common.h>
#include <mt-plat/charging.h>
#include <mt-plat/mt_boot.h>
#include <mt-plat/upmu_common.h>

#include "psc5415.h"

#define STATUS_OK		0
#define STATUS_FAIL		1
#define STATUS_UNSUPPORTED	-1

static DEFINE_MUTEX(psc5415_command_lock);

static const u32 psc5415_hv_threshold[] = {
	BATTERY_VOLT_04_200000_V, BATTERY_VOLT_04_250000_V,
	BATTERY_VOLT_04_300000_V, BATTERY_VOLT_04_350000_V,
	BATTERY_VOLT_04_400000_V, BATTERY_VOLT_04_450000_V,
	BATTERY_VOLT_04_500000_V, BATTERY_VOLT_04_550000_V,
	BATTERY_VOLT_04_600000_V, BATTERY_VOLT_06_000000_V,
	BATTERY_VOLT_06_500000_V, BATTERY_VOLT_07_000000_V,
	BATTERY_VOLT_07_500000_V, BATTERY_VOLT_08_500000_V,
	BATTERY_VOLT_09_500000_V, BATTERY_VOLT_10_500000_V,
};

static u32 psc5415_find_closest_level(const u32 *values, u32 count,
	u32 requested)
{
	u32 index;

	for (index = count - 1; index != 0; index--)
		if (values[index] <= requested)
			return values[index];

	return values[0];
}

static u32 psc5415_parameter_to_index(const u32 *values, u32 count,
	u32 value)
{
	u32 index;

	for (index = 0; index < count; index++)
		if (values[index] == value)
			return index;

	return 0;
}

static int charging_hw_init(void *data)
{
	int ret = psc5415_operational_init();

	if (ret)
		battery_log(BAT_LOG_CRTI,
			"[psc5415] operational initialization failed: %d\n", ret);
	return ret ? STATUS_FAIL : STATUS_OK;
}

static int charging_dump_register(void *data)
{
	return psc5415_dump_register() ? STATUS_FAIL : STATUS_OK;
}

static int charging_enable(void *data)
{
	u32 enable = *(u32 *)data;
	u32 boost;
	int ret;

	if (!enable)
		return psc5415_set_ce(1) ? STATUS_FAIL : STATUS_OK;

	/* Prepare the operating mode before allowing battery charging. */
	ret = psc5415_set_hz_mode(0);
	if (ret)
		return STATUS_FAIL;
	ret = psc5415_get_otg_status(&boost);
	if (ret)
		return STATUS_FAIL;
	if (!boost && psc5415_set_opa_mode(0))
		return STATUS_FAIL;

	return psc5415_set_ce(0) ? STATUS_FAIL : STATUS_OK;
}

static int charging_set_cv_voltage(void *data)
{
	u32 microvolts = *(u32 *)data;
	u8 code;
	int ret;

	ret = psc5415_cv_code(microvolts, &code);
	if (ret) {
		battery_log(BAT_LOG_CRTI,
			"[psc5415] unsupported CV request %u uV: %d\n",
			microvolts, ret);
		return STATUS_FAIL;
	}

	return psc5415_set_oreg(code) ? STATUS_FAIL : STATUS_OK;
}

static int charging_get_current(void *data)
{
	/* IINLIM is an input limit, not a calibrated battery-current reading. */
	*(u32 *)data = 0;
	return STATUS_UNSUPPORTED;
}

static int charging_set_current(void *data)
{
	u32 requested_current = *(u32 *)data;
	u8 code;
	int ret;

	ret = psc5415_charge_current_code(requested_current, &code);
	if (ret) {
		battery_log(BAT_LOG_CRTI,
			"[psc5415] unsupported battery-current limit %u (0.01 mA): %d\n",
			requested_current, ret);
		return STATUS_FAIL;
	}
	return psc5415_set_iocharge(code) ? STATUS_FAIL : STATUS_OK;
}

static int charging_set_input_current(void *data)
{
	u32 requested_current = *(u32 *)data;
	u8 code;
	int ret;

	if (requested_current == CHARGE_CURRENT_MAX)
		code = PSC5415_IINLIM_MASK;
	else {
		ret = psc5415_input_current_code(requested_current, &code);
		if (ret) {
			battery_log(BAT_LOG_CRTI,
				"[psc5415] unsupported input-current limit %u (0.01 mA): %d\n",
				requested_current, ret);
			return STATUS_FAIL;
		}
	}
	return psc5415_set_input_charging_current(code) ?
		STATUS_FAIL : STATUS_OK;
}

static int charging_get_charging_status(void *data)
{
	u32 status;
	int ret;

	ret = psc5415_get_chip_status(&status);
	if (ret)
		return STATUS_FAIL;

	*(kal_bool *)data = status == 2 ? KAL_TRUE : KAL_FALSE;
	return STATUS_OK;
}

static int charging_reset_watch_dog_timer(void *data)
{
	return psc5415_set_tmr_rst(1) ? STATUS_FAIL : STATUS_OK;
}

static int charging_set_hv_threshold(void *data)
{
	u32 voltage = *(u32 *)data;
	u32 closest;
	u32 index;

	closest = psc5415_find_closest_level(psc5415_hv_threshold,
		ARRAY_SIZE(psc5415_hv_threshold), voltage);
	index = psc5415_parameter_to_index(psc5415_hv_threshold,
		ARRAY_SIZE(psc5415_hv_threshold), closest);
	pmic_set_register_value(PMIC_RG_VCDT_HV_VTH, index);
	return STATUS_OK;
}

static int charging_get_hv_status(void *data)
{
	*(kal_bool *)data = pmic_get_register_value(PMIC_RGS_VCDT_HV_DET);
	return STATUS_OK;
}

static int charging_get_battery_status(void *data)
{
	u32 baton_enabled;

	baton_enabled = pmic_get_register_value(PMIC_BATON_TDET_EN);
	if (!baton_enabled) {
		*(kal_bool *)data = KAL_FALSE;
		return STATUS_OK;
	}

	pmic_set_register_value(PMIC_BATON_TDET_EN, 1);
	pmic_set_register_value(PMIC_RG_BATON_EN, 1);
	*(kal_bool *)data = pmic_get_register_value(PMIC_RGS_BATON_UNDET);
	return STATUS_OK;
}

static int charging_get_charger_det_status(void *data)
{
	*(kal_bool *)data = pmic_get_register_value(PMIC_RGS_CHRDET);
	return STATUS_OK;
}

static int charging_get_charger_type(void *data)
{
	*(CHARGER_TYPE *)data = hw_charging_get_charger_type();
	return STATUS_OK;
}

static int charging_set_platform_reset(void *data)
{
	kernel_restart("battery service reboot system");
	return STATUS_OK;
}

static int charging_get_platform_boot_mode(void *data)
{
	*(u32 *)data = get_boot_mode();
	return STATUS_OK;
}

static int charging_set_power_off(void *data)
{
	kernel_power_off();
	return STATUS_OK;
}

static int charging_get_power_source(void *data)
{
	return STATUS_UNSUPPORTED;
}

static int charging_get_csdac_full_flag(void *data)
{
	return STATUS_UNSUPPORTED;
}

static int charging_set_ta_current_pattern(void *data)
{
	return STATUS_UNSUPPORTED;
}

static int charging_set_error_state(void *data)
{
	return STATUS_UNSUPPORTED;
}

/*
 * Generic xHCI/WUSB mapping. It uses I2C-controlled OPA mode and deliberately
 * leaves the board-specific OTG pin disabled. HZ=0 followed by OPA=1 enables
 * boost; clearing OPA disables it.
 */
static int charging_enable_otg(void *data)
{
	u32 enable = *(u32 *)data;

	return psc5415_enable_otg(enable) ? STATUS_FAIL : STATUS_OK;
}

typedef int (*psc5415_charging_handler)(void *data);

static psc5415_charging_handler const
psc5415_charging_func[CHARGING_CMD_NUMBER] = {
	[CHARGING_CMD_INIT] = charging_hw_init,
	[CHARGING_CMD_DUMP_REGISTER] = charging_dump_register,
	[CHARGING_CMD_ENABLE] = charging_enable,
	[CHARGING_CMD_SET_CV_VOLTAGE] = charging_set_cv_voltage,
	[CHARGING_CMD_GET_CURRENT] = charging_get_current,
	[CHARGING_CMD_SET_CURRENT] = charging_set_current,
	[CHARGING_CMD_SET_INPUT_CURRENT] = charging_set_input_current,
	[CHARGING_CMD_GET_CHARGING_STATUS] = charging_get_charging_status,
	[CHARGING_CMD_RESET_WATCH_DOG_TIMER] = charging_reset_watch_dog_timer,
	[CHARGING_CMD_SET_HV_THRESHOLD] = charging_set_hv_threshold,
	[CHARGING_CMD_GET_HV_STATUS] = charging_get_hv_status,
	[CHARGING_CMD_GET_BATTERY_STATUS] = charging_get_battery_status,
	[CHARGING_CMD_GET_CHARGER_DET_STATUS] = charging_get_charger_det_status,
	[CHARGING_CMD_GET_CHARGER_TYPE] = charging_get_charger_type,
	[CHARGING_CMD_SET_PLATFORM_RESET] = charging_set_platform_reset,
	[CHARGING_CMD_GET_PLATFORM_BOOT_MODE] = charging_get_platform_boot_mode,
	[CHARGING_CMD_SET_POWER_OFF] = charging_set_power_off,
	[CHARGING_CMD_GET_POWER_SOURCE] = charging_get_power_source,
	[CHARGING_CMD_GET_CSDAC_FALL_FLAG] = charging_get_csdac_full_flag,
	[CHARGING_CMD_SET_TA_CURRENT_PATTERN] = charging_set_ta_current_pattern,
	[CHARGING_CMD_SET_ERROR_STATE] = charging_set_error_state,
	/* Stock left this unsupported; added for generic xHCI/WUSB OTG control. */
	[CHARGING_CMD_ENABLE_OTG] = charging_enable_otg,
};

signed int chr_control_interface(CHARGING_CTRL_CMD cmd, void *data)
{
	psc5415_charging_handler handler;
	int ret;

	BUILD_BUG_ON(CHARGING_CMD_NUMBER != 63);
	if ((unsigned int)cmd >= CHARGING_CMD_NUMBER)
		return STATUS_UNSUPPORTED;

	handler = psc5415_charging_func[cmd];
	if (!handler)
		return STATUS_UNSUPPORTED;

	/* Serialize complete multi-register commands, not just I2C transfers. */
	mutex_lock(&psc5415_command_lock);
	ret = handler(data);
	if (ret && (cmd == CHARGING_CMD_INIT ||
		    cmd == CHARGING_CMD_SET_CV_VOLTAGE ||
		    cmd == CHARGING_CMD_SET_CURRENT ||
		    cmd == CHARGING_CMD_SET_INPUT_CURRENT ||
		    cmd == CHARGING_CMD_ENABLE)) {
		int disable_ret = psc5415_set_ce(1);

		if (disable_ret)
			battery_log(BAT_LOG_CRTI,
				"[psc5415] command %u failed (%d), disable failed (%d)\n",
				cmd, ret, disable_ret);
	}
	mutex_unlock(&psc5415_command_lock);

	return ret;
}
