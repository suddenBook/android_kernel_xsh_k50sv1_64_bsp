/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Source reconstruction of the fitted PSC5415 charger. The successful
 * register sequences come from the shipped MT6750/MT6755 3.18.119 Image;
 * transport and lifetime failures deliberately fail closed.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 */
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>

#include <mt-plat/charging.h>

#include "psc5415.h"

#define PSC5415_COMBINED_READ_LENGTH	((1 << 8) | 1)

static DEFINE_MUTEX(psc5415_io_lock);
static DEFINE_MUTEX(psc5415_state_lock);
static struct i2c_client *psc5415_client;
static bool psc5415_present;

kal_bool chargin_hw_init_done = KAL_FALSE;

int psc5415_read_byte(u8 reg, u8 *value)
{
	struct i2c_client *client;
	u32 original_ext_flag;
	u8 buffer;
	int ret;

	if (!value || reg >= PSC5415_REG_COUNT)
		return -EINVAL;

	mutex_lock(&psc5415_io_lock);
	client = READ_ONCE(psc5415_client);
	if (!client) {
		ret = -ENODEV;
		goto out_unlock;
	}

	original_ext_flag = client->ext_flag;
	client->ext_flag = (original_ext_flag & I2C_MASK_FLAG) |
		I2C_WR_FLAG | I2C_DIRECTION_FLAG;
	buffer = reg;
	ret = i2c_master_send(client, &buffer, PSC5415_COMBINED_READ_LENGTH);
	client->ext_flag = original_ext_flag;
	ret = psc5415_transfer_status(ret, PSC5415_COMBINED_READ_LENGTH);
	if (ret)
		goto out_unlock;

	*value = buffer;

out_unlock:
	mutex_unlock(&psc5415_io_lock);
	return ret;
}

int psc5415_write_byte(u8 reg, u8 value)
{
	struct i2c_client *client;
	u32 original_ext_flag;
	u8 buffer[2] = { reg, value };
	int ret;

	if (reg >= PSC5415_REG_COUNT)
		return -EINVAL;

	mutex_lock(&psc5415_io_lock);
	client = READ_ONCE(psc5415_client);
	if (!client) {
		ret = -ENODEV;
		goto out_unlock;
	}

	original_ext_flag = client->ext_flag;
	client->ext_flag = (original_ext_flag & I2C_MASK_FLAG) |
		I2C_DIRECTION_FLAG;
	ret = i2c_master_send(client, buffer, sizeof(buffer));
	client->ext_flag = original_ext_flag;
	ret = psc5415_transfer_status(ret, sizeof(buffer));
	if (ret)
		goto out_unlock;

out_unlock:
	mutex_unlock(&psc5415_io_lock);
	return ret;
}

static int psc5415_io_read(void *context, u8 reg, u8 *value)
{
	(void)context;
	return psc5415_read_byte(reg, value);
}

static int psc5415_io_write(void *context, u8 reg, u8 value)
{
	(void)context;
	return psc5415_write_byte(reg, value);
}

static const struct psc5415_io psc5415_bus = {
	.read = psc5415_io_read,
	.write = psc5415_io_write,
};

int psc5415_read_interface(u8 reg, u8 *value, u8 mask, u8 shift)
{
	u8 raw;
	int ret;

	if (!value)
		return -EINVAL;

	ret = psc5415_read_byte(reg, &raw);
	if (ret)
		return ret;

	*value = (raw >> shift) & mask;
	return 0;
}

int PSC5415_CONfig_interface(u8 reg, u8 value, u8 mask, u8 shift)
{
	return psc5415_update_bits(&psc5415_bus, reg, value, mask, shift);
}

int psc5415_reg_config_interface(u8 reg, u8 value)
{
	return psc5415_write_byte(reg, value);
}

int psc5415_hw_component_detect(void)
{
	u8 ic_info;
	int ret;

	ret = psc5415_read_byte(PSC5415_REG_IC_INFO, &ic_info);
	if (ret)
		return ret;

	return ic_info == PSC5415_EXPECTED_IC_INFO ? 0 : -ENODEV;
}

int is_psc5415_exist(void)
{
	return READ_ONCE(psc5415_present) ? 1 : 0;
}

#define PSC5415_SETTER(_name, _reg, _mask, _shift) \
	int _name(u32 value) \
	{ \
		return PSC5415_CONfig_interface((_reg), value, (_mask), (_shift)); \
	}

#define PSC5415_GETTER(_name, _reg, _mask, _shift) \
	int _name(u32 *value) \
	{ \
		u8 field; \
		int ret; \
		if (!value) \
			return -EINVAL; \
		ret = psc5415_read_interface((_reg), &field, (_mask), (_shift)); \
		if (!ret) \
			*value = field; \
		return ret; \
	}

PSC5415_SETTER(psc5415_set_tmr_rst, PSC5415_REG_STATUS,
	PSC5415_TMR_RST_MASK, PSC5415_TMR_RST_SHIFT)
/* Stock's OTG status helper reads the BOOST status bit, not TMR_RST. */
PSC5415_GETTER(psc5415_get_otg_status, PSC5415_REG_STATUS,
	PSC5415_BOOST_MASK, PSC5415_BOOST_SHIFT)
PSC5415_SETTER(psc5415_set_en_stat, PSC5415_REG_STATUS,
	PSC5415_EN_STAT_MASK, PSC5415_EN_STAT_SHIFT)
PSC5415_GETTER(psc5415_get_chip_status, PSC5415_REG_STATUS,
	PSC5415_STAT_MASK, PSC5415_STAT_SHIFT)
PSC5415_GETTER(psc5415_get_boost_status, PSC5415_REG_STATUS,
	PSC5415_BOOST_MASK, PSC5415_BOOST_SHIFT)
PSC5415_GETTER(psc5415_get_fault_status, PSC5415_REG_STATUS,
	PSC5415_FAULT_MASK, PSC5415_FAULT_SHIFT)
PSC5415_SETTER(psc5415_set_input_charging_current, PSC5415_REG_CONTROL,
	PSC5415_IINLIM_MASK, PSC5415_IINLIM_SHIFT)
PSC5415_SETTER(psc5415_set_v_low, PSC5415_REG_CONTROL,
	PSC5415_VLOW_MASK, PSC5415_VLOW_SHIFT)
PSC5415_SETTER(psc5415_set_te, PSC5415_REG_CONTROL,
	PSC5415_TE_MASK, PSC5415_TE_SHIFT)
PSC5415_SETTER(psc5415_set_ce, PSC5415_REG_CONTROL,
	PSC5415_CE_MASK, PSC5415_CE_SHIFT)
PSC5415_SETTER(psc5415_set_hz_mode, PSC5415_REG_CONTROL,
	PSC5415_HZ_MASK, PSC5415_HZ_SHIFT)
PSC5415_SETTER(psc5415_set_opa_mode, PSC5415_REG_CONTROL,
	PSC5415_OPA_MASK, PSC5415_OPA_SHIFT)
PSC5415_SETTER(psc5415_set_oreg, PSC5415_REG_OREG,
	PSC5415_OREG_MASK, PSC5415_OREG_SHIFT)
PSC5415_SETTER(psc5415_set_otg_pl, PSC5415_REG_OREG,
	PSC5415_OTG_PL_MASK, PSC5415_OTG_PL_SHIFT)
PSC5415_SETTER(psc5415_set_otg_en, PSC5415_REG_OREG,
	PSC5415_OTG_EN_MASK, PSC5415_OTG_EN_SHIFT)
PSC5415_GETTER(psc5415_get_vender_code, PSC5415_REG_IC_INFO,
	PSC5415_VENDOR_MASK, PSC5415_VENDOR_SHIFT)
PSC5415_GETTER(psc5415_get_pn, PSC5415_REG_IC_INFO,
	PSC5415_PN_MASK, PSC5415_PN_SHIFT)
PSC5415_GETTER(psc5415_get_revision, PSC5415_REG_IC_INFO,
	PSC5415_REV_MASK, PSC5415_REV_SHIFT)
PSC5415_SETTER(psc5415_set_reset, PSC5415_REG_IBAT,
	PSC5415_RESET_MASK, PSC5415_RESET_SHIFT)
PSC5415_SETTER(psc5415_set_iocharge, PSC5415_REG_IBAT,
	PSC5415_IOCHARGE_MASK, PSC5415_IOCHARGE_SHIFT)
PSC5415_SETTER(psc5415_set_iterm, PSC5415_REG_IBAT,
	PSC5415_ITERM_MASK, PSC5415_ITERM_SHIFT)
PSC5415_SETTER(psc5415_set_dis_vreg, PSC5415_REG_SP_CHARGER,
	PSC5415_DIS_VREG_MASK, PSC5415_DIS_VREG_SHIFT)
PSC5415_SETTER(psc5415_set_io_level, PSC5415_REG_SP_CHARGER,
	PSC5415_IO_LEVEL_MASK, PSC5415_IO_LEVEL_SHIFT)
PSC5415_GETTER(psc5415_get_sp_status, PSC5415_REG_SP_CHARGER,
	PSC5415_SP_STATUS_MASK, PSC5415_SP_STATUS_SHIFT)
PSC5415_GETTER(psc5415_get_en_level, PSC5415_REG_SP_CHARGER,
	PSC5415_EN_LEVEL_MASK, PSC5415_EN_LEVEL_SHIFT)
PSC5415_SETTER(psc5415_set_vsp, PSC5415_REG_SP_CHARGER,
	PSC5415_VSP_MASK, PSC5415_VSP_SHIFT)
PSC5415_SETTER(psc5415_set_i_safe, PSC5415_REG_SAFETY,
	PSC5415_ISAFE_MASK, PSC5415_ISAFE_SHIFT)
PSC5415_SETTER(psc5415_set_v_safe, PSC5415_REG_SAFETY,
	PSC5415_VSAFE_MASK, PSC5415_VSAFE_SHIFT)

int psc5415_dump_register(void)
{
	u8 values[PSC5415_REG_COUNT];
	int reg;
	int ret;

	for (reg = 0; reg < PSC5415_REG_COUNT; reg++) {
		ret = psc5415_read_byte(reg, &values[reg]);
		if (ret)
			return ret;
	}

	battery_log(BAT_LOG_CRTI,
		"[psc5415] [0x0]=0x%x [0x1]=0x%x [0x2]=0x%x "
		"[0x3]=0x%x [0x4]=0x%x [0x5]=0x%x [0x6]=0x%x "
		"psc5415_dump_register end\n",
		values[0], values[1], values[2], values[3],
		values[4], values[5], values[6]);
	return 0;
}

int psc5415_operational_init(void)
{
	int ret;

	mutex_lock(&psc5415_state_lock);
	if (!psc5415_client || !psc5415_present) {
		ret = -ENODEV;
		goto out_unlock;
	}

	ret = psc5415_apply_operational_sequence(&psc5415_bus);
	if (!ret)
		ret = psc5415_dump_register();

out_unlock:
	/* Keep probe readiness so the battery worker can retry transient errors. */
	mutex_unlock(&psc5415_state_lock);
	return ret;
}

int psc5415_enable_otg(u32 enable)
{
	return psc5415_apply_otg(&psc5415_bus, !!enable);
}

static int psc5415_driver_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	u8 ic_info = 0;
	int ret;

	(void)id;
	mutex_lock(&psc5415_state_lock);
	if (client->addr != PSC5415_I2C_ADDRESS) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (psc5415_client) {
		ret = -EBUSY;
		goto out_unlock;
	}

	WRITE_ONCE(chargin_hw_init_done, KAL_FALSE);
	WRITE_ONCE(psc5415_present, false);
	WRITE_ONCE(psc5415_client, client);

	ret = psc5415_apply_probe_sequence(&psc5415_bus, &ic_info);
	if (ret) {
		dev_err(&client->dev,
			"probe sequence failed at IC_INFO=0x%02x: %d\n",
			ic_info, ret);
		goto out_clear_client;
	}

	ret = psc5415_dump_register();
	if (ret) {
		dev_err(&client->dev, "initial register dump failed: %d\n", ret);
		goto out_clear_client;
	}

	WRITE_ONCE(psc5415_present, true);
	WRITE_ONCE(chargin_hw_init_done, KAL_TRUE);
	dev_info(&client->dev, "PSC5415 detected (IC_INFO=0x%02x)\n", ic_info);
	mutex_unlock(&psc5415_state_lock);
	return 0;

out_clear_client:
	WRITE_ONCE(psc5415_client, NULL);
out_unlock:
	mutex_unlock(&psc5415_state_lock);
	return ret;
}

static const struct i2c_device_id psc5415_i2c_id[] = {
	{ "switching_charger", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, psc5415_i2c_id);

static const struct of_device_id psc5415_of_match[] = {
	{ .compatible = "mediatek,switching_charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, psc5415_of_match);

static struct i2c_driver psc5415_driver = {
	.driver = {
		.name = "psc5415",
		.of_match_table = psc5415_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = psc5415_driver_probe,
	.id_table = psc5415_i2c_id,
};

static int __init psc5415_init(void)
{
	return i2c_add_driver(&psc5415_driver);
}
subsys_initcall(psc5415_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek PSC5415 switch charger");
