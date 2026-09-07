/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 */
#ifndef __MTK_PSC5415_H__
#define __MTK_PSC5415_H__

#ifdef PSC5415_HOST_TEST
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#else
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#endif

#define PSC5415_REG_STATUS		0x00
#define PSC5415_REG_CONTROL		0x01
#define PSC5415_REG_OREG		0x02
#define PSC5415_REG_IC_INFO		0x03
#define PSC5415_REG_IBAT		0x04
#define PSC5415_REG_SP_CHARGER		0x05
#define PSC5415_REG_SAFETY		0x06
#define PSC5415_REG_COUNT		7
#define PSC5415_I2C_ADDRESS		0x6a

#define PSC5415_EXPECTED_IC_INFO		0xf0

#define PSC5415_TMR_RST_MASK		0x01
#define PSC5415_TMR_RST_SHIFT		7
#define PSC5415_EN_STAT_MASK		0x01
#define PSC5415_EN_STAT_SHIFT		6
#define PSC5415_STAT_MASK		0x03
#define PSC5415_STAT_SHIFT		4
#define PSC5415_BOOST_MASK		0x01
#define PSC5415_BOOST_SHIFT		3
#define PSC5415_FAULT_MASK		0x07
#define PSC5415_FAULT_SHIFT		0

#define PSC5415_IINLIM_MASK		0x03
#define PSC5415_IINLIM_SHIFT		6
#define PSC5415_VLOW_MASK		0x03
#define PSC5415_VLOW_SHIFT		4
#define PSC5415_TE_MASK			0x01
#define PSC5415_TE_SHIFT		3
#define PSC5415_CE_MASK			0x01
#define PSC5415_CE_SHIFT		2
#define PSC5415_HZ_MASK			0x01
#define PSC5415_HZ_SHIFT		1
#define PSC5415_OPA_MASK		0x01
#define PSC5415_OPA_SHIFT		0

#define PSC5415_OREG_MASK		0x3f
#define PSC5415_OREG_SHIFT		2
#define PSC5415_OTG_PL_MASK		0x01
#define PSC5415_OTG_PL_SHIFT		1
#define PSC5415_OTG_EN_MASK		0x01
#define PSC5415_OTG_EN_SHIFT		0

#define PSC5415_VENDOR_MASK		0x07
#define PSC5415_VENDOR_SHIFT		5
#define PSC5415_PN_MASK			0x03
#define PSC5415_PN_SHIFT		3
#define PSC5415_REV_MASK		0x07
#define PSC5415_REV_SHIFT		0

#define PSC5415_RESET_MASK		0x01
#define PSC5415_RESET_SHIFT		7
#define PSC5415_IOCHARGE_MASK		0x07
#define PSC5415_IOCHARGE_SHIFT		4
#define PSC5415_ITERM_MASK		0x07
#define PSC5415_ITERM_SHIFT		0

#define PSC5415_DIS_VREG_MASK		0x01
#define PSC5415_DIS_VREG_SHIFT		6
#define PSC5415_IO_LEVEL_MASK		0x01
#define PSC5415_IO_LEVEL_SHIFT		5
#define PSC5415_SP_STATUS_MASK		0x01
#define PSC5415_SP_STATUS_SHIFT		4
#define PSC5415_EN_LEVEL_MASK		0x01
#define PSC5415_EN_LEVEL_SHIFT		3
#define PSC5415_VSP_MASK		0x07
#define PSC5415_VSP_SHIFT		0

#define PSC5415_ISAFE_MASK		0x07
#define PSC5415_ISAFE_SHIFT		4
#define PSC5415_VSAFE_MASK		0x0f
#define PSC5415_VSAFE_SHIFT		0

/* Successful stock sequences recovered from the shipped 3.18.119 Image. */
#define PSC5415_STOCK_SAFETY		0x5f
#define PSC5415_STOCK_PROBE_IBAT		0x50
#define PSC5415_STOCK_PROBE_CONTROL	0xf8
#define PSC5415_STOCK_PROBE_OREG		0xb6
#define PSC5415_STOCK_PROBE_SP		0x84
#define PSC5415_STOCK_INIT_OREG		0x2c
#define PSC5415_STOCK_INIT_CONTROL	0xf8
#define PSC5415_STOCK_INIT_SP		0x02
#define PSC5415_STOCK_INIT_IBAT		0x50

/*
 * PSC5415 Rev.06.1 table 3 and PSC5415E Rev.06.5 table 1 agree:
 * codes 2..35 select 4.20 V, 36..44 select 4.35 V, and 45..62 select
 * 4.40 V. These are groups, not 20 mV steps. Keep this board's existing
 * maximum code 44, and do not assume the variant-specific 4.10 V group.
 */
#define PSC5415_CV_MIN_UV			4200000U
#define PSC5415_CV_MAX_UV			4350000U
#define PSC5415_CV_MIN_CODE		35
#define PSC5415_CV_MAX_CODE		44

#define PSC5415_CURRENT_150_MA		15000U
#define PSC5415_CURRENT_500_MA		50000U
#define PSC5415_CURRENT_800_MA		80000U
#define PSC5415_STOCK_DCP_CURRENT	205000U

struct psc5415_io {
	void *context;
	int (*read)(void *context, u8 reg, u8 *value);
	int (*write)(void *context, u8 reg, u8 value);
};

static inline int psc5415_io_valid(const struct psc5415_io *io)
{
	return io && io->read && io->write;
}

static inline int psc5415_transfer_status(int actual, int expected)
{
	if (actual == expected)
		return 0;
	return actual < 0 ? actual : -EIO;
}

static inline int psc5415_update_bits(const struct psc5415_io *io, u8 reg,
	u8 value, u8 mask, u8 shift)
{
	u8 old_value;
	u8 new_value;
	int ret;

	if (!psc5415_io_valid(io) || reg >= PSC5415_REG_COUNT)
		return -EINVAL;

	ret = io->read(io->context, reg, &old_value);
	if (ret)
		return ret;

	new_value = old_value & (u8)~(mask << shift);
	new_value |= (u8)((value & mask) << shift);

	/* REG4's reset bit reads back as one and must not be replayed. */
	if (reg == PSC5415_REG_IBAT &&
	    !(value == 1 && mask == PSC5415_RESET_MASK &&
	      shift == PSC5415_RESET_SHIFT))
		new_value &= (u8)~(PSC5415_RESET_MASK << PSC5415_RESET_SHIFT);

	return io->write(io->context, reg, new_value);
}

static inline int psc5415_apply_probe_sequence(const struct psc5415_io *io,
	u8 *ic_info)
{
	u8 value;
	int ret;

	if (!psc5415_io_valid(io))
		return -EINVAL;

	ret = io->read(io->context, PSC5415_REG_IC_INFO, &value);
	if (ret)
		return ret;
	if (ic_info)
		*ic_info = value;
	if (value != PSC5415_EXPECTED_IC_INFO)
		return -ENODEV;

	/* REG6 is deliberately the first charger-register write. */
	ret = io->write(io->context, PSC5415_REG_SAFETY,
		PSC5415_STOCK_SAFETY);
	if (ret)
		return ret;
	ret = io->write(io->context, PSC5415_REG_IBAT,
		PSC5415_STOCK_PROBE_IBAT);
	if (ret)
		return ret;
	/* Probe must not enable charging before policy initialization. */
	ret = io->write(io->context, PSC5415_REG_CONTROL,
		PSC5415_STOCK_PROBE_CONTROL | (1U << PSC5415_CE_SHIFT));
	if (ret)
		return ret;
	ret = io->write(io->context, PSC5415_REG_OREG,
		PSC5415_STOCK_PROBE_OREG);
	if (ret)
		return ret;
	return io->write(io->context, PSC5415_REG_SP_CHARGER,
		PSC5415_STOCK_PROBE_SP);
}

static inline int psc5415_apply_operational_sequence(
	const struct psc5415_io *io)
{
	int ret;

	if (!psc5415_io_valid(io))
		return -EINVAL;

	ret = io->write(io->context, PSC5415_REG_OREG,
		PSC5415_STOCK_INIT_OREG);
	if (ret)
		return ret;
	/* Keep CE disabled until the policy has applied all charge limits. */
	ret = io->write(io->context, PSC5415_REG_CONTROL,
		PSC5415_STOCK_INIT_CONTROL | (1U << PSC5415_CE_SHIFT));
	if (ret)
		return ret;
	ret = io->write(io->context, PSC5415_REG_SP_CHARGER,
		PSC5415_STOCK_INIT_SP);
	if (ret)
		return ret;
	return io->write(io->context, PSC5415_REG_IBAT,
		PSC5415_STOCK_INIT_IBAT);
}

static inline int psc5415_apply_otg(const struct psc5415_io *io, bool enable)
{
	int ret;

	if (!psc5415_io_valid(io))
		return -EINVAL;

	if (!enable)
		return psc5415_update_bits(io, PSC5415_REG_CONTROL, 0,
			PSC5415_OPA_MASK, PSC5415_OPA_SHIFT);

	ret = psc5415_update_bits(io, PSC5415_REG_CONTROL, 0,
		PSC5415_HZ_MASK, PSC5415_HZ_SHIFT);
	if (ret)
		return ret;

	return psc5415_update_bits(io, PSC5415_REG_CONTROL, 1,
		PSC5415_OPA_MASK, PSC5415_OPA_SHIFT);
}

static inline int psc5415_cv_code(u32 microvolts, u8 *code)
{
	if (!code)
		return -EINVAL;
	if (microvolts < PSC5415_CV_MIN_UV)
		return -ERANGE;

	/* Round down to a supported voltage, capped at this board's 4.35 V. */
	*code = microvolts >= PSC5415_CV_MAX_UV ?
		PSC5415_CV_MAX_CODE : PSC5415_CV_MIN_CODE;
	return 0;
}

/*
 * Preserve the existing USB and normal-DCP presets. IOCHARGE and IO_LEVEL
 * depend on the chip revision and fitted sense resistor, so do not turn an
 * arbitrary thermal battery-current cap into either of these presets.
 */
static inline int psc5415_charge_current_code(u32 requested_current, u8 *code)
{
	if (!code)
		return -EINVAL;
	if (requested_current == PSC5415_CURRENT_500_MA)
		*code = 0;
	else if (requested_current >= PSC5415_STOCK_DCP_CURRENT)
		*code = 7;
	else
		return -ERANGE;
	return 0;
}

static inline int psc5415_input_current_code(u32 requested_current, u8 *code)
{
	if (!code)
		return -EINVAL;
	/* Code zero is 100 mA on PSC5415(A), but 150 mA on PSC5415E. */
	if (requested_current < PSC5415_CURRENT_150_MA)
		return -ERANGE;
	if (requested_current >= PSC5415_CURRENT_800_MA)
		*code = 2;
	else if (requested_current >= PSC5415_CURRENT_500_MA)
		*code = 1;
	else
		*code = 0;
	return 0;
}

#ifndef PSC5415_HOST_TEST
int psc5415_read_byte(u8 reg, u8 *value);
int psc5415_write_byte(u8 reg, u8 value);
int psc5415_read_interface(u8 reg, u8 *value, u8 mask, u8 shift);
int PSC5415_CONfig_interface(u8 reg, u8 value, u8 mask, u8 shift);
int psc5415_reg_config_interface(u8 reg, u8 value);
int psc5415_hw_component_detect(void);
int is_psc5415_exist(void);
int psc5415_operational_init(void);
int psc5415_enable_otg(u32 enable);
int psc5415_dump_register(void);

int psc5415_set_tmr_rst(u32 value);
int psc5415_get_otg_status(u32 *value);
int psc5415_set_en_stat(u32 value);
int psc5415_get_chip_status(u32 *value);
int psc5415_get_boost_status(u32 *value);
int psc5415_get_fault_status(u32 *value);
int psc5415_set_input_charging_current(u32 value);
int psc5415_set_v_low(u32 value);
int psc5415_set_te(u32 value);
int psc5415_set_ce(u32 value);
int psc5415_set_hz_mode(u32 value);
int psc5415_set_opa_mode(u32 value);
int psc5415_set_oreg(u32 value);
int psc5415_set_otg_pl(u32 value);
int psc5415_set_otg_en(u32 value);
int psc5415_get_vender_code(u32 *value);
int psc5415_get_pn(u32 *value);
int psc5415_get_revision(u32 *value);
int psc5415_set_reset(u32 value);
int psc5415_set_iocharge(u32 value);
int psc5415_set_iterm(u32 value);
int psc5415_set_dis_vreg(u32 value);
int psc5415_set_io_level(u32 value);
int psc5415_get_sp_status(u32 *value);
int psc5415_get_en_level(u32 *value);
int psc5415_set_vsp(u32 value);
int psc5415_set_i_safe(u32 value);
int psc5415_set_v_safe(u32 value);
#endif

#endif /* __MTK_PSC5415_H__ */
