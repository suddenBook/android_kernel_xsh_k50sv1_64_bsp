/*
 * WUSB3801X USB Type-C CC controller, k50sv1_64_bsp port.
 *
 * Based on the WillSemi reference driver for WUSB3801x rev 2.0
 * (Copyright (c) 2016, WillSemi Inc., lhuang@sh-willsemi.com,
 * drivers/usb/wusb3801x/wusb3801x.c, GPL v2).  The board-specific parts
 * reproduce the shipped k50sv1_64_bsp kernel: the chip is enabled through
 * an active-low PWN pin, the interrupt comes from the "mediatek,EINT_TYPEC-
 * eint" node, and an audio accessory drives the BCT4321 analog switch and
 * ACCDET's typec_headphone_irq_handler().  USB host mode is not handled
 * here; the MediaTek USB controller owns it through its ID-pin path.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#define pr_fmt(fmt) "wusb3801x: " fmt

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/wakelock.h>

#include "wusb3801x.h"

struct wusb3801_pdata {
	int pwn_gpio;		/* active low: 0 enables the chip */
	int switch_gpio;	/* BCT4321 analog switch: 1 routes audio */
	u8 init_mode;
	u8 dfp_power;
	u8 dttime;
};

struct wusb3801_chip {
	struct i2c_client *client;
	struct wusb3801_pdata pdata;
	struct wake_lock wlock;
	struct mutex mlock;
	int irq;
	bool irq_wake_enabled;
	int ufp_power;
	u8 mode;
	u8 dev_id;
	u8 type;
	u8 state;
	u8 bc_lvl;
	u8 dfp_power;
	u8 dttime;
	u8 attached;
};

static const char * const wusb3801_state_names[] = {
	[WUSB3801_STATE_DISABLED] = "disabled",
	[WUSB3801_STATE_ERROR_RECOVERY] = "error_recovery",
	[WUSB3801_STATE_UNATTACHED_SNK] = "unattached_snk",
	[WUSB3801_STATE_UNATTACHED_SRC] = "unattached_src",
	[WUSB3801_STATE_ATTACHED_SNK] = "attached_snk",
	[WUSB3801_STATE_ATTACHED_SRC] = "attached_src",
	[WUSB3801_STATE_AUDIO_ACCESSORY] = "audio_accessory",
	[WUSB3801_STATE_DEBUG_ACCESSORY] = "debug_accessory",
};

static const char *wusb3801_state_name(u8 state)
{
	if (state < ARRAY_SIZE(wusb3801_state_names) &&
	    wusb3801_state_names[state])
		return wusb3801_state_names[state];
	return "unknown";
}

static void wusb3801_update_state(struct wusb3801_chip *chip, u8 state)
{
	chip->state = state;
	dev_info(&chip->client->dev, "state: %s\n", wusb3801_state_name(state));
}

/* Board glue ------------------------------------------------------------ */

/* Stock power_on(): PWN is active low, 0 turns the chip on. */
static void wusb3801_power_on(struct wusb3801_chip *chip, int on)
{
	gpio_direction_output(chip->pdata.pwn_gpio, on ? 0 : 1);
}

/* Stock bct4321_switch(): 1 routes the USB-C D+/D- pair to the codec. */
static void wusb3801_audio_switch(struct wusb3801_chip *chip, int audio)
{
	gpio_direction_output(chip->pdata.switch_gpio, audio ? 1 : 0);
}

/* Register helpers ------------------------------------------------------ */

static int wusb3801_write_masked_byte(struct i2c_client *client, u8 addr,
	u8 mask, u8 val)
{
	int rc;

	if (!mask)
		return -EINVAL;
	rc = i2c_smbus_read_byte_data(client, addr);
	if (rc < 0)
		return rc;
	return i2c_smbus_write_byte_data(client, addr,
		((u8)rc & ~mask) | (val & mask));
}

static int wusb3801_read_device_id(struct wusb3801_chip *chip)
{
	struct device *cdev = &chip->client->dev;
	int rc;

	rc = i2c_smbus_read_byte_data(chip->client, WUSB3801_REG_VERSION_ID);
	if (rc < 0)
		return rc;
	if ((rc & WUSB3801_VENDOR_ID_MASK) != WUSB3801_VENDOR_ID) {
		dev_err(cdev, "unexpected version register 0x%02x\n", rc);
		return -ENODEV;
	}
	chip->dev_id = rc;
	dev_info(cdev, "vendor id 0x%02x, version id 0x%02x\n",
		 rc & WUSB3801_VENDOR_ID_MASK,
		 (rc & WUSB3801_VERSION_ID_MASK) >> 3);
	return 0;
}

static int wusb3801_update_status(struct wusb3801_chip *chip)
{
	int rc;

	rc = i2c_smbus_read_byte_data(chip->client, WUSB3801_REG_CONTROL0);
	if (rc < 0) {
		dev_err(&chip->client->dev, "failed to read CONTROL0: %d\n", rc);
		return rc;
	}
	chip->mode = rc & WUSB3801_MODE_MASK;
	chip->dfp_power = (rc & WUSB3801_HOST_CUR_MASK) >> 3;
	chip->dttime = WUSB3801_TGL_40MS;
	return 0;
}

static int wusb3801_check_modes(u8 mode)
{
	switch (mode) {
	case WUSB3801_DRP_ACC:
	case WUSB3801_DRP:
	case WUSB3801_SNK_ACC:
	case WUSB3801_SNK:
	case WUSB3801_SRC_ACC:
	case WUSB3801_SRC:
	case WUSB3801_DRP_PREFER_SRC_ACC:
	case WUSB3801_DRP_PREFER_SRC:
	case WUSB3801_DRP_PREFER_SNK_ACC:
	case WUSB3801_DRP_PREFER_SNK:
		return 0;
	default:
		return -EINVAL;
	}
}

static int wusb3801_set_chip_state(struct wusb3801_chip *chip, u8 state)
{
	int rc;

	if (state > WUSB3801_STATE_UNATTACHED_SRC)
		return -EINVAL;
	rc = i2c_smbus_write_byte_data(chip->client, WUSB3801_REG_CONTROL1,
		state == WUSB3801_STATE_DISABLED ? WUSB3801_DISABLED : 0);
	if (rc < 0)
		dev_err(&chip->client->dev, "failed to write state machine: %d\n", rc);
	return rc;
}

/*
 * Change the role bits with the chip interrupt masked, then acknowledge
 * whatever the mode change raised before unmasking it again.
 */
static int wusb3801_set_mode(struct wusb3801_chip *chip, u8 mode)
{
	struct device *cdev = &chip->client->dev;
	int rc;

	if (mode == chip->mode)
		return 0;

	rc = i2c_smbus_read_byte_data(chip->client, WUSB3801_REG_CONTROL0);
	if (rc < 0)
		return rc;
	rc = (rc & ~WUSB3801_MODE_MASK) | mode | WUSB3801_INT_MASK;
	rc = i2c_smbus_write_byte_data(chip->client, WUSB3801_REG_CONTROL0, rc);
	if (rc < 0) {
		dev_err(cdev, "failed to write mode: %d\n", rc);
		return rc;
	}
	rc = i2c_smbus_read_byte_data(chip->client, WUSB3801_REG_INTERRUPT);
	if (rc < 0)
		return rc;
	rc = i2c_smbus_read_byte_data(chip->client, WUSB3801_REG_CONTROL0);
	if (rc < 0)
		return rc;
	rc = i2c_smbus_write_byte_data(chip->client, WUSB3801_REG_CONTROL0,
		rc & ~WUSB3801_INT_MASK);
	if (rc < 0) {
		dev_err(cdev, "failed to unmask interrupt: %d\n", rc);
		return rc;
	}
	chip->mode = mode;
	dev_dbg(cdev, "mode 0x%02x\n", mode);
	return 0;
}

static int wusb3801_set_dfp_power(struct wusb3801_chip *chip, u8 hcurrent)
{
	int rc;

	if (hcurrent == chip->dfp_power)
		return 0;
	rc = wusb3801_write_masked_byte(chip->client, WUSB3801_REG_CONTROL0,
		WUSB3801_HOST_CUR_MASK, hcurrent << 3);
	if (rc < 0) {
		dev_err(&chip->client->dev, "failed to write host current: %d\n", rc);
		return rc;
	}
	chip->dfp_power = hcurrent;
	return 0;
}

/*
 * A 3 A-capable DRP partner without VBUS is misdetected as a sink by
 * Type-C 1.0/1.1 logic.  Advertise 3 A while unattached and drop to the
 * configured current once a sink is really attached.
 */
static int wusb3801_init_force_dfp_power(struct wusb3801_chip *chip)
{
	int rc;

	rc = wusb3801_write_masked_byte(chip->client, WUSB3801_REG_CONTROL0,
		WUSB3801_HOST_CUR_MASK, WUSB3801_HOST_3000MA << 3);
	if (rc < 0) {
		dev_err(&chip->client->dev, "failed to force host current: %d\n", rc);
		return rc;
	}
	chip->dfp_power = WUSB3801_HOST_3000MA;
	return 0;
}

static int wusb3801_set_toggle_time(struct wusb3801_chip *chip, u8 toggle_time)
{
	if (toggle_time != WUSB3801_TGL_40MS)
		return -EINVAL;
	chip->dttime = WUSB3801_TGL_40MS;
	return 0;
}

/* Attach / detach ------------------------------------------------------- */

static void wusb3801_set_icurrent_max(struct wusb3801_chip *chip, int icurrent)
{
	chip->ufp_power = icurrent;
}

static void wusb3801_detach(struct wusb3801_chip *chip)
{
	struct device *cdev = &chip->client->dev;

	dev_info(cdev, "detach: type 0x%02x, state %s\n", chip->type,
		 wusb3801_state_name(chip->state));

	switch (chip->state) {
	case WUSB3801_STATE_ATTACHED_SRC:
		wusb3801_init_force_dfp_power(chip);
		break;
	case WUSB3801_STATE_ATTACHED_SNK:
		wusb3801_set_icurrent_max(chip, 0);
		break;
	case WUSB3801_STATE_AUDIO_ACCESSORY:
		wusb3801_audio_switch(chip, 0);
		typec_headphone_irq_handler(0);
		break;
	default:
		break;
	}
	chip->type = WUSB3801_TYPE_INVALID;
	chip->bc_lvl = WUSB3801_SNK_0MA;
	chip->ufp_power = 0;
	chip->attached = 0;
	wusb3801_update_state(chip, WUSB3801_STATE_ERROR_RECOVERY);
}

static int wusb3801_reset_device(struct wusb3801_chip *chip)
{
	struct device *cdev = &chip->client->dev;
	int rc;

	rc = wusb3801_update_status(chip);
	if (rc < 0) {
		dev_err(cdev, "failed to read status\n");
		return rc;
	}

	rc = wusb3801_init_force_dfp_power(chip);
	if (rc < 0) {
		dev_err(cdev, "failed to force dfp power\n");
		return rc;
	}
	rc = wusb3801_set_mode(chip, chip->pdata.init_mode);
	if (rc < 0) {
		dev_err(cdev, "failed to set mode\n");
		return rc;
	}
	rc = wusb3801_set_chip_state(chip, WUSB3801_STATE_ERROR_RECOVERY);
	if (rc < 0) {
		dev_err(cdev, "failed to reset state\n");
		return rc;
	}

	wusb3801_detach(chip);

	rc = wusb3801_write_masked_byte(chip->client, WUSB3801_REG_CONTROL0,
		WUSB3801_INT_MASK, WUSB3801_INT_ENABLE);
	if (rc < 0) {
		dev_err(cdev, "failed to unmask interrupt: %d\n", rc);
		return rc;
	}
	dev_info(cdev, "mode 0x%02x, host current %u, toggle %u\n",
		 chip->mode, chip->dfp_power, chip->dttime);
	return 0;
}

static void wusb3801_bclvl_changed(struct wusb3801_chip *chip)
{
	struct device *cdev = &chip->client->dev;
	int rc;

	rc = i2c_smbus_read_byte_data(chip->client, WUSB3801_REG_STATUS);
	if (rc < 0) {
		dev_err(cdev, "failed to read status: %d\n", rc);
		if (wusb3801_reset_device(chip))
			dev_err(cdev, "failed to reset\n");
		return;
	}
	if ((rc & WUSB3801_TYPE_MASK) == WUSB3801_TYPE_SRC) {
		chip->bc_lvl = rc & WUSB3801_BCLVL_MASK;
		wusb3801_set_icurrent_max(chip,
			chip->bc_lvl == WUSB3801_SNK_3000MA ? 3000 :
			chip->bc_lvl == WUSB3801_SNK_1500MA ? 1500 : 0);
		dev_info(cdev, "source current advertisement %d mA\n",
			 chip->ufp_power);
	}
}

static void wusb3801_src_detected(struct wusb3801_chip *chip)
{
	struct device *cdev = &chip->client->dev;

	if ((chip->mode & WUSB3801_ROLE_MASK) == BIT_REG_CTRL0_RLE_SRC) {
		dev_err(cdev, "source partner while in source-only mode\n");
		if (wusb3801_reset_device(chip))
			dev_err(cdev, "failed to reset\n");
		return;
	}
	wusb3801_update_state(chip, WUSB3801_STATE_ATTACHED_SNK);
	chip->type = WUSB3801_TYPE_SRC;
}

static void wusb3801_snk_detected(struct wusb3801_chip *chip)
{
	struct device *cdev = &chip->client->dev;

	if ((chip->mode & WUSB3801_ROLE_MASK) == BIT_REG_CTRL0_RLE_SNK) {
		dev_err(cdev, "sink partner while in sink-only mode\n");
		if (wusb3801_reset_device(chip))
			dev_err(cdev, "failed to reset\n");
		return;
	}
	wusb3801_set_dfp_power(chip, chip->pdata.dfp_power);
	wusb3801_update_state(chip, WUSB3801_STATE_ATTACHED_SRC);
	chip->type = WUSB3801_TYPE_SNK;
}

static void wusb3801_dbg_acc_detected(struct wusb3801_chip *chip)
{
	struct device *cdev = &chip->client->dev;

	if (chip->mode & BIT_REG_CTRL0_DIS_ACC) {
		dev_err(cdev, "accessory while accessories are disabled\n");
		if (wusb3801_reset_device(chip))
			dev_err(cdev, "failed to reset\n");
		return;
	}
	wusb3801_update_state(chip, WUSB3801_STATE_DEBUG_ACCESSORY);
}

static void wusb3801_aud_acc_detected(struct wusb3801_chip *chip)
{
	struct device *cdev = &chip->client->dev;

	if (chip->mode & BIT_REG_CTRL0_DIS_ACC) {
		dev_err(cdev, "accessory while accessories are disabled\n");
		if (wusb3801_reset_device(chip))
			dev_err(cdev, "failed to reset\n");
		return;
	}
	wusb3801_update_state(chip, WUSB3801_STATE_AUDIO_ACCESSORY);
	/* Route the analog pair to the codec, then let ACCDET classify it. */
	wusb3801_audio_switch(chip, 1);
	typec_headphone_irq_handler(1);
}

static void wusb3801_attach(struct wusb3801_chip *chip)
{
	struct device *cdev = &chip->client->dev;
	int rc;
	u8 type;

	rc = i2c_smbus_read_byte_data(chip->client, WUSB3801_REG_STATUS);
	if (rc < 0) {
		dev_err(cdev, "failed to read status: %d\n", rc);
		return;
	}
	type = (rc & WUSB3801_ATTACH) ? rc & WUSB3801_TYPE_MASK :
		WUSB3801_TYPE_INVALID;
	dev_info(cdev, "attach: status 0x%02x, type 0x%02x\n", rc, type);

	if (chip->state != WUSB3801_STATE_ERROR_RECOVERY) {
		if (wusb3801_set_chip_state(chip, WUSB3801_STATE_ERROR_RECOVERY))
			dev_err(cdev, "failed to set error recovery\n");
		wusb3801_detach(chip);
		dev_err(cdev, "attach in state %s\n",
			wusb3801_state_name(chip->state));
		return;
	}

	switch (type) {
	case WUSB3801_TYPE_SRC:
		wusb3801_src_detected(chip);
		break;
	case WUSB3801_TYPE_SNK:
		wusb3801_snk_detected(chip);
		break;
	case WUSB3801_TYPE_DBG_ACC:
		wusb3801_dbg_acc_detected(chip);
		break;
	case WUSB3801_TYPE_AUD_ACC:
		wusb3801_aud_acc_detected(chip);
		break;
	case WUSB3801_TYPE_INVALID:
		wusb3801_detach(chip);
		dev_err(cdev, "attach interrupt without an attached type\n");
		return;
	default:
		if (wusb3801_set_chip_state(chip, WUSB3801_STATE_ERROR_RECOVERY))
			dev_err(cdev, "failed to set error recovery\n");
		wusb3801_detach(chip);
		dev_err(cdev, "unknown type 0x%02x\n", type);
		return;
	}
	/* Rejected roles/accessories leave the state machine in recovery. */
	if (chip->state == WUSB3801_STATE_ERROR_RECOVERY)
		return;
	chip->type = type;
	chip->attached = 1;
}

static irqreturn_t wusb3801_irq_thread(int irq, void *data)
{
	struct wusb3801_chip *chip = data;
	struct device *cdev = &chip->client->dev;
	int rc;
	u8 int_sts;

	/* Keep the system up until the charger has seen the new partner. */
	wake_lock_timeout(&chip->wlock,
		msecs_to_jiffies(WUSB3801_WAKE_LOCK_TIMEOUT_MS));
	mutex_lock(&chip->mlock);
	rc = i2c_smbus_read_byte_data(chip->client, WUSB3801_REG_INTERRUPT);
	if (rc < 0) {
		dev_err(cdev, "failed to read interrupt: %d\n", rc);
		goto out;
	}
	int_sts = rc & WUSB3801_INT_STS_MASK;
	dev_dbg(cdev, "interrupt status 0x%02x\n", int_sts);

	if (int_sts & WUSB3801_INT_DETACH)
		wusb3801_detach(chip);
	if (int_sts & WUSB3801_INT_ATTACH) {
		if (chip->attached)
			wusb3801_bclvl_changed(chip);
		else
			wusb3801_attach(chip);
	}
out:
	mutex_unlock(&chip->mlock);
	return IRQ_HANDLED;
}

/* sysfs ----------------------------------------------------------------- */

static ssize_t fregdump_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct wusb3801_chip *chip = i2c_get_clientdata(to_i2c_client(dev));
	ssize_t ret = 0;
	int rc, reg;

	mutex_lock(&chip->mlock);
	for (reg = WUSB3801_REG_VERSION_ID; reg <= WUSB3801_REG_LAST; reg++) {
		/* Only the IRQ path may consume the read-to-clear event latch. */
		if (reg == WUSB3801_REG_INTERRUPT) {
			ret += scnprintf(buf + ret, PAGE_SIZE - ret,
					 "0x%02x: skipped (read-clear IRQ latch)\n", reg);
			continue;
		}
		rc = i2c_smbus_read_byte_data(chip->client, reg);
		if (rc < 0) {
			mutex_unlock(&chip->mlock);
			return rc;
		}
		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "0x%02x: 0x%02x\n",
				 reg, rc);
	}
	mutex_unlock(&chip->mlock);
	return ret;
}
static DEVICE_ATTR(fregdump, S_IRUGO, fregdump_show, NULL);

static ssize_t fchip_state_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct wusb3801_chip *chip = i2c_get_clientdata(to_i2c_client(dev));

	return scnprintf(buf, PAGE_SIZE, "%s\n", wusb3801_state_name(chip->state));
}
static DEVICE_ATTR(fchip_state, S_IRUGO, fchip_state_show, NULL);

static ssize_t fmode_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct wusb3801_chip *chip = i2c_get_clientdata(to_i2c_client(dev));

	return scnprintf(buf, PAGE_SIZE, "0x%02x\n", chip->mode);
}

static ssize_t fmode_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct wusb3801_chip *chip = i2c_get_clientdata(to_i2c_client(dev));
	u8 mode;
	int rc;

	if (kstrtou8(buf, 0, &mode) || wusb3801_check_modes(mode))
		return -EINVAL;
	mutex_lock(&chip->mlock);
	rc = wusb3801_set_mode(chip, mode);
	mutex_unlock(&chip->mlock);
	return rc < 0 ? rc : size;
}
static DEVICE_ATTR(fmode, S_IRUGO | S_IWUSR, fmode_show, fmode_store);

static ssize_t fhostcur_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct wusb3801_chip *chip = i2c_get_clientdata(to_i2c_client(dev));

	return scnprintf(buf, PAGE_SIZE, "%u\n", chip->dfp_power);
}

static ssize_t fhostcur_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct wusb3801_chip *chip = i2c_get_clientdata(to_i2c_client(dev));
	u8 cur;
	int rc;

	if (kstrtou8(buf, 0, &cur) || cur > WUSB3801_HOST_3000MA)
		return -EINVAL;
	mutex_lock(&chip->mlock);
	rc = wusb3801_set_dfp_power(chip, cur);
	mutex_unlock(&chip->mlock);
	return rc < 0 ? rc : size;
}
static DEVICE_ATTR(fhostcur, S_IRUGO | S_IWUSR, fhostcur_show, fhostcur_store);

static ssize_t fclientcur_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct wusb3801_chip *chip = i2c_get_clientdata(to_i2c_client(dev));

	return scnprintf(buf, PAGE_SIZE, "%d\n", chip->ufp_power);
}
static DEVICE_ATTR(fclientcur, S_IRUGO, fclientcur_show, NULL);

static ssize_t freset_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct wusb3801_chip *chip = i2c_get_clientdata(to_i2c_client(dev));
	int rc;

	mutex_lock(&chip->mlock);
	rc = wusb3801_reset_device(chip);
	mutex_unlock(&chip->mlock);
	return rc < 0 ? rc : size;
}
static DEVICE_ATTR(freset, S_IWUSR, NULL, freset_store);

static struct attribute *wusb3801_attrs[] = {
	&dev_attr_fregdump.attr,
	&dev_attr_fchip_state.attr,
	&dev_attr_fmode.attr,
	&dev_attr_fhostcur.attr,
	&dev_attr_fclientcur.attr,
	&dev_attr_freset.attr,
	NULL,
};

static const struct attribute_group wusb3801_attr_group = {
	.attrs = wusb3801_attrs,
};

/* Probe ----------------------------------------------------------------- */

static int wusb3801_parse_dt(struct wusb3801_chip *chip)
{
	struct device *cdev = &chip->client->dev;
	struct device_node *node = cdev->of_node;
	struct wusb3801_pdata *data = &chip->pdata;
	u32 val = 0;
	int rc;

	if (!node)
		return -ENODEV;

	data->pwn_gpio = of_get_named_gpio(node, "wusb3801,pwn-gpio", 0);
	if (!gpio_is_valid(data->pwn_gpio)) {
		dev_err(cdev, "no usable wusb3801,pwn-gpio\n");
		return data->pwn_gpio < 0 ? data->pwn_gpio : -EINVAL;
	}
	data->switch_gpio = of_get_named_gpio(node, "wusb3801,usb-switch-gpio", 0);
	if (!gpio_is_valid(data->switch_gpio)) {
		dev_err(cdev, "no usable wusb3801,usb-switch-gpio\n");
		return data->switch_gpio < 0 ? data->switch_gpio : -EINVAL;
	}

	rc = of_property_read_u32(node, "wusb3801,init-mode", &val);
	data->init_mode = (u8)val;
	if (rc || wusb3801_check_modes(data->init_mode)) {
		dev_warn(cdev, "init-mode unusable, using DRP with accessories\n");
		data->init_mode = WUSB3801_DRP_PREFER_SNK_ACC;
	}
	rc = of_property_read_u32(node, "wusb3801,host-current", &val);
	data->dfp_power = (u8)val;
	if (rc || data->dfp_power > WUSB3801_HOST_3000MA) {
		dev_warn(cdev, "host-current unusable, using default current\n");
		data->dfp_power = WUSB3801_HOST_DEFAULT;
	}
	rc = of_property_read_u32(node, "wusb3801,drp-toggle-time", &val);
	if (rc || wusb3801_set_toggle_time(chip, (u8)val))
		dev_dbg(cdev, "only the fixed 40 ms/40 ms DRP toggle is supported\n");
	data->dttime = WUSB3801_TGL_40MS;

	dev_dbg(cdev, "init_mode 0x%02x dfp_power %u\n", data->init_mode,
		data->dfp_power);
	return 0;
}

/* The interrupt line is described by a separate EINT node, as on stock. */
static int wusb3801_resolve_irq(struct wusb3801_chip *chip)
{
	struct device_node *node;
	u32 ints[2] = { 0, 0 };
	int irq;

	node = of_find_compatible_node(NULL, NULL, "mediatek,EINT_TYPEC-eint");
	if (!node) {
		dev_err(&chip->client->dev, "no mediatek,EINT_TYPEC-eint node\n");
		return -ENODEV;
	}
	if (!of_property_read_u32_array(node, "debounce", ints, ARRAY_SIZE(ints)))
		gpio_set_debounce(ints[0], ints[1]);
	irq = irq_of_parse_and_map(node, 0);
	of_node_put(node);
	if (irq <= 0) {
		dev_err(&chip->client->dev, "no interrupt on EINT_TYPEC node\n");
		return -EINVAL;
	}
	chip->irq = irq;
	return 0;
}

static int wusb3801_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	struct device *cdev = &client->dev;
	struct wusb3801_chip *chip;
	int rc, i;

	if (!i2c_check_functionality(client->adapter,
			I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA)) {
		dev_err(cdev, "smbus byte/word data not supported\n");
		return -EIO;
	}

	chip = devm_kzalloc(cdev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->client = client;
	i2c_set_clientdata(client, chip);

	rc = wusb3801_parse_dt(chip);
	if (rc)
		return rc;

	rc = devm_gpio_request(cdev, chip->pdata.pwn_gpio, "wusb3801-pwn");
	if (rc)
		return rc;
	rc = devm_gpio_request(cdev, chip->pdata.switch_gpio, "wusb3801-usb-switch");
	if (rc)
		return rc;
	wusb3801_power_on(chip, 1);
	wusb3801_audio_switch(chip, 0);

	for (i = 0; i < 5; i++) {
		rc = wusb3801_read_device_id(chip);
		if (!rc)
			break;
		usleep_range(1000, 2000);
	}
	if (rc) {
		dev_err(cdev, "chip not found: %d\n", rc);
		goto err_power;
	}

	chip->type = WUSB3801_TYPE_INVALID;
	chip->state = WUSB3801_STATE_ERROR_RECOVERY;
	chip->bc_lvl = WUSB3801_SNK_0MA;

	wake_lock_init(&chip->wlock, WAKE_LOCK_SUSPEND, "wusb3801_wake");
	mutex_init(&chip->mlock);

	rc = wusb3801_resolve_irq(chip);
	if (rc)
		goto err_wake;

	rc = wusb3801_reset_device(chip);
	if (rc) {
		dev_err(cdev, "failed to initialize: %d\n", rc);
		goto err_wake;
	}
	rc = i2c_smbus_write_byte_data(client, WUSB3801_REG_CONTROL1,
		BIT_REG_CTRL1_SM_RST);
	if (rc < 0) {
		dev_err(cdev, "failed to reset state machine: %d\n", rc);
		goto err_wake;
	}

	/* ONESHOT keeps the level IRQ masked until the I2C handler completes. */
	rc = request_threaded_irq(chip->irq, NULL, wusb3801_irq_thread,
		IRQF_TRIGGER_LOW | IRQF_ONESHOT, "wusb3801_int_irq", chip);
	if (rc) {
		dev_err(cdev, "failed to request irq %d: %d\n", chip->irq, rc);
		goto err_wake;
	}
	/* Publish writers only after initialization and IRQ setup are complete. */
	rc = sysfs_create_group(&cdev->kobj, &wusb3801_attr_group);
	if (rc)
		goto err_irq;
	rc = enable_irq_wake(chip->irq);
	if (!rc)
		chip->irq_wake_enabled = true;
	else
		dev_warn(cdev, "cannot enable wake IRQ: %d\n", rc);

	dev_info(cdev, "ready, irq %d, mode 0x%02x\n", chip->irq, chip->mode);
	return 0;

err_irq:
	free_irq(chip->irq, chip);
	wusb3801_detach(chip);
err_wake:
	wake_lock_destroy(&chip->wlock);
err_power:
	wusb3801_power_on(chip, 0);
	return rc;
}

static int wusb3801_remove(struct i2c_client *client)
{
	struct wusb3801_chip *chip = i2c_get_clientdata(client);

	sysfs_remove_group(&client->dev.kobj, &wusb3801_attr_group);
	if (chip->irq_wake_enabled)
		disable_irq_wake(chip->irq);
	/* free_irq() also waits for the threaded handler's I2C/audio handoff. */
	free_irq(chip->irq, chip);
	wusb3801_detach(chip);
	wake_lock_destroy(&chip->wlock);
	wusb3801_power_on(chip, 0);
	i2c_set_clientdata(client, NULL);
	return 0;
}

static void wusb3801_shutdown(struct i2c_client *client)
{
	struct wusb3801_chip *chip = i2c_get_clientdata(client);

	if (!chip)
		return;
	/* No queued attach may change the role after the chip is parked. */
	disable_irq(chip->irq);
	mutex_lock(&chip->mlock);
	if (wusb3801_set_mode(chip, WUSB3801_SNK) ||
	    wusb3801_set_chip_state(chip, WUSB3801_STATE_ERROR_RECOVERY))
		dev_err(&client->dev, "failed to park in sink mode\n");
	mutex_unlock(&chip->mlock);
	msleep(5);
}

static const struct i2c_device_id wusb3801_id_table[] = {
	{ "wusb3801x", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, wusb3801_id_table);

static const struct of_device_id wusb3801_of_match[] = {
	{ .compatible = "mediatek,wusb3801x" },
	{ }
};
MODULE_DEVICE_TABLE(of, wusb3801_of_match);

static struct i2c_driver wusb3801_i2c_driver = {
	.driver = {
		.name = "wusb3801x",
		.owner = THIS_MODULE,
		.of_match_table = wusb3801_of_match,
	},
	.probe = wusb3801_probe,
	.remove = wusb3801_remove,
	.shutdown = wusb3801_shutdown,
	.id_table = wusb3801_id_table,
};
module_i2c_driver(wusb3801_i2c_driver);

MODULE_AUTHOR("lhuang@sh-willsemi.com");
MODULE_DESCRIPTION("WUSB3801X USB Type-C CC controller (k50sv1_64_bsp)");
MODULE_LICENSE("GPL v2");
