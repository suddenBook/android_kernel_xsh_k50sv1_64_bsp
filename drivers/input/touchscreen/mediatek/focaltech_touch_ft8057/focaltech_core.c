/*
 * Minimal FocalTech V3.4 FT8057 driver for k50sv1_64_bsp.
 *
 * This is a source-bringup candidate, not a claim of complete stock parity.
 * It keeps only touch reporting, read-only diagnostics and the board's
 * two-phase display/touch power ordering.  Firmware update, factory test,
 * gesture, proximity, automatic ESD recovery and writable debug interfaces
 * are intentionally absent.
 *
 * Copyright (c) 2012-2020 FocalTech Systems, Ltd.
 * Copyright (C) 2026 k50sv1_64_bsp project
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/slab.h>

#include "focaltech_core.h"
#include "k50_lcm_bias.h"

#define FTS_IDENTIFY_RETRIES	10
#define FTS_IDENTIFY_DELAY_MS	100
#define FTS_BOOT_START_DELAY_MS	12

/* Exact nine-byte rows recovered from the shipped k50sv1 kernel. */
static const struct fts_chip_id fts_chip_ids[] = {
	{ 0x19, 0x86, 0x42, 0x86, 0x42, 0x86, 0xC2, 0x00, 0x00 },
	{ 0x19, 0x86, 0x32, 0x86, 0x32, 0x86, 0xC2, 0x00, 0x00 },
	{ 0x28, 0x80, 0x57, 0x80, 0x57, 0x80, 0xA7, 0x00, 0x00 },
};

static const char * const fts_pm_state_names[] = {
	[FTS_PM_ACTIVE] = "active",
	[FTS_PM_PREPARING] = "preparing",
	[FTS_PM_PREPARED] = "prepared",
	[FTS_PM_PREPARED_IO_ERROR] = "prepared-io-error",
	[FTS_PM_POWERED_DOWN] = "powered-down",
	[FTS_PM_RESUMING] = "resuming",
	[FTS_PM_FAULT] = "fault",
};

struct fts_ts_data *fts_data;
static DEFINE_MUTEX(fts_lifetime_lock);

/* The Hall source will be supplied by its own hardware driver. */
extern int get_hall_status(void) __attribute__((weak));

static bool fts_event_is_down(u8 flag)
{
	return flag == FTS_TOUCH_DOWN || flag == FTS_TOUCH_CONTACT;
}

int fts_check_cid(struct fts_ts_data *ts, u8 id_h)
{
	unsigned int i;
	u8 candidate;

	if (!ts || !ts->cid.type)
		return -ENODATA;

	for (i = 0; i < ARRAY_SIZE(ts->cid.ids); i++) {
		candidate = ts->cid.ids[i] >> 8;
		if (candidate && candidate == id_h)
			return 0;
	}

	return -ENODATA;
}

static int fts_match_chip(struct fts_ts_data *ts, u8 id_h, u8 id_l,
			  bool firmware_valid)
{
	const struct fts_chip_id *chip;
	unsigned int i;
	bool match;

	if (!id_h || !id_l)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(fts_chip_ids); i++) {
		chip = &fts_chip_ids[i];
		if (firmware_valid) {
			match = chip->chip_idh == id_h && chip->chip_idl == id_l;
		} else {
			match = (chip->rom_idh == id_h && chip->rom_idl == id_l) ||
				(chip->pb_idh == id_h && chip->pb_idl == id_l) ||
				(chip->bl_idh == id_h && chip->bl_idl == id_l);
		}
		if (match) {
			ts->chip = *chip;
			return 0;
		}
	}

	return -ENODATA;
}

static int fts_read_boot_id(struct fts_ts_data *ts, u8 *id_h, u8 *id_l)
{
	u8 start_command[2] = { 0x55, 0xAA };
	u8 read_command[4] = { 0x90, 0x00, 0x00, 0x00 };
	u8 chip_id[2] = { 0x00, 0x00 };
	int ret;

	ret = fts_write(ts, start_command, sizeof(start_command));
	if (ret)
		return ret;
	msleep(FTS_BOOT_START_DELAY_MS);

	ret = fts_read(ts, read_command, sizeof(read_command), chip_id,
		       sizeof(chip_id));
	if (ret)
		return ret;
	if (!chip_id[0] || !chip_id[1])
		return -EIO;

	*id_h = chip_id[0];
	*id_l = chip_id[1];
	return 0;
}

static int fts_wait_valid(struct fts_ts_data *ts);
static void fts_reset(int delay_ms);

static int fts_identify(struct fts_ts_data *ts)
{
	u8 id_h = 0;
	u8 id_l = 0;
	int ret = -ENODATA;
	int retry;

	for (retry = 0; retry < FTS_IDENTIFY_RETRIES; retry++) {
		ret = fts_read_reg(ts, FTS_REG_CHIP_ID, &id_h);
		if (!ret)
			ret = fts_read_reg(ts, FTS_REG_CHIP_ID2, &id_l);
		if (!ret && !fts_match_chip(ts, id_h, id_l, true)) {
			FTS_INFO("chip id=0x%02x%02x type=0x%02x",
				 id_h, id_l, ts->chip.type);
			return 0;
		}
		msleep(FTS_IDENTIFY_DELAY_MS);
	}

	FTS_INFO("firmware ID unavailable; trying the boot ID");
	ret = fts_read_boot_id(ts, &id_h, &id_l);
	if (ret)
		return ret;
	ret = fts_match_chip(ts, id_h, id_l, false);
	if (ret)
		return ret;

	FTS_INFO("boot id=0x%02x%02x type=0x%02x",
		 id_h, id_l, ts->chip.type);
	fts_reset(200);
	ret = fts_wait_valid(ts);
	if (ret)
		FTS_ERROR("boot ID matched but application firmware is invalid: %d",
			  ret);
	return ret;
}

static int fts_wait_valid(struct fts_ts_data *ts)
{
	u8 id_h = 0;
	int ret = -EIO;
	int retry;

	for (retry = 0; retry < FTS_IDENTIFY_RETRIES; retry++) {
		ret = fts_read_reg(ts, FTS_REG_CHIP_ID, &id_h);
		if (!ret && (id_h == ts->chip.chip_idh ||
			     !fts_check_cid(ts, id_h))) {
			FTS_INFO("TP ready, device ID=0x%02x", id_h);
			return 0;
		}
		msleep(FTS_IDENTIFY_DELAY_MS);
	}

	return ret ? ret : -EIO;
}

static void fts_reset(int delay_ms)
{
	tpd_gpio_output(GTP_RST_PORT, 0);
	msleep(5);
	tpd_gpio_output(GTP_RST_PORT, 1);
	if (delay_ms)
		msleep(delay_ms);
}

static void fts_irq_disable_sync(struct fts_ts_data *ts)
{
	if (ts->irq_requested && !ts->irq_disabled) {
		disable_irq(ts->irq);
		ts->irq_disabled = true;
	}
}

static void fts_irq_enable(struct fts_ts_data *ts)
{
	if (ts->irq_requested && ts->irq_disabled) {
		ts->irq_disabled = false;
		enable_irq(ts->irq);
	}
}

static void fts_release_all_fingers(struct fts_ts_data *ts)
{
	unsigned int id;

	mutex_lock(&ts->report_lock);
	for (id = 0; id < ts->max_points; id++) {
		input_mt_slot(ts->input_dev, id);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);
	}
	input_report_key(ts->input_dev, BTN_TOUCH, 0);
	input_sync(ts->input_dev);
	ts->active_slots = 0;
	mutex_unlock(&ts->report_lock);
}

static int fts_report_events(struct fts_ts_data *ts, unsigned int event_count,
			     bool snapshot)
{
	unsigned long active = snapshot ? 0 : ts->active_slots;
	unsigned long previous = ts->active_slots;
	unsigned long stale;
	struct fts_event *event;
	unsigned int i;

	mutex_lock(&ts->report_lock);
	for (i = 0; i < event_count; i++) {
		event = &ts->events[i];
		input_mt_slot(ts->input_dev, event->id);
		if (fts_event_is_down(event->flag)) {
			input_mt_report_slot_state(ts->input_dev,
						   MT_TOOL_FINGER, true);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
					 event->area);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_X,
					 event->x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,
					 event->y);
			active |= BIT(event->id);
		} else {
			input_mt_report_slot_state(ts->input_dev,
						   MT_TOOL_FINGER, false);
			active &= ~BIT(event->id);
		}
	}

	if (snapshot) {
		stale = previous & ~active;
		for (i = 0; i < ts->max_points; i++) {
			if (!(stale & BIT(i)))
				continue;
			input_mt_slot(ts->input_dev, i);
			input_mt_report_slot_state(ts->input_dev,
						   MT_TOOL_FINGER, false);
		}
	}

	input_report_key(ts->input_dev, BTN_TOUCH, !!active);
	input_sync(ts->input_dev);
	ts->active_slots = active;
	mutex_unlock(&ts->report_lock);
	return 0;
}

static int fts_parse_touch_packet(struct fts_ts_data *ts)
{
	u8 event_type = ts->touch_buf[1] >> 4;
	u8 firmware_points = ts->touch_buf[1] & 0x0F;
	unsigned int event_count;
	unsigned int limit;
	unsigned int base;
	unsigned int i;
	bool snapshot;
	struct fts_event *event;

	if (firmware_points > ts->max_points)
		return -EIO;

	if (event_type == 0x00) {
		snapshot = true;
		limit = ts->max_points;
	} else if (event_type == 0x02) {
		snapshot = true;
		if (!firmware_points)
			return -EIO;
		limit = firmware_points;
	} else {
		return -EOPNOTSUPP;
	}

	event_count = 0;
	for (i = 0; i < limit; i++) {
		base = 2 + i * FTS_ONE_TOUCH_LEN;
		event = &ts->events[event_count];
		event->id = ts->touch_buf[base + 2] >> 4;
		if (event->id >= FTS_MAX_ID && snapshot)
			break;
		if (event->id >= ts->max_points)
			return -EINVAL;

		event->flag = ts->touch_buf[base] >> 6;
		if (event->flag > FTS_TOUCH_CONTACT)
			return -EINVAL;
		event->x = ((ts->touch_buf[base] & 0x0F) << 8) |
			   ts->touch_buf[base + 1];
		event->y = ((ts->touch_buf[base + 2] & 0x0F) << 8) |
			   ts->touch_buf[base + 3];
		event->pressure = ts->touch_buf[base + 4];
		event->area = ts->touch_buf[base + 5];
		if (!event->pressure)
			event->pressure = 0x3F;
		if (!event->area)
			event->area = 0x09;
		if (event->x > FTS_X_MAX || event->y > FTS_Y_MAX)
			return -ERANGE;
		if (fts_event_is_down(event->flag) && !firmware_points)
			return -EIO;
		event_count++;
	}

	if (!event_count && snapshot && !firmware_points) {
		fts_release_all_fingers(ts);
		return 0;
	}
	if (!event_count)
		return -EIO;

	return fts_report_events(ts, event_count, snapshot);
}

static irqreturn_t fts_irq_thread(int irq, void *device_data)
{
	struct fts_ts_data *ts = device_data;
	u8 address = 0x01;
	int read_length;
	int ret;

	if (READ_ONCE(ts->pm_state) != FTS_PM_ACTIVE)
		return IRQ_HANDLED;

	read_length = 2 + ts->max_points * FTS_ONE_TOUCH_LEN;
	memset(ts->touch_buf, 0xFF, sizeof(ts->touch_buf));
	ret = fts_read(ts, &address, 1, ts->touch_buf, read_length);
	if (!ret)
		ret = fts_parse_touch_packet(ts);
	if (ret)
		dev_err_ratelimited(&ts->client->dev,
				    "FT8057 touch packet failed: %d\n", ret);

	return IRQ_HANDLED;
}

static int fts_input_init(struct fts_ts_data *ts)
{
	struct input_dev *input;
	int ret;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;

	ts->input_dev = input;
	input->name = FTS_DRIVER_NAME;
	input->id.bustype = BUS_I2C;
	input->dev.parent = &ts->client->dev;
	input_set_drvdata(input, ts);

	__set_bit(EV_KEY, input->evbit);
	__set_bit(EV_ABS, input->evbit);
	__set_bit(BTN_TOUCH, input->keybit);
	__set_bit(INPUT_PROP_DIRECT, input->propbit);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, FTS_X_MAX, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, FTS_Y_MAX, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

	ret = input_mt_init_slots(input, ts->max_points, INPUT_MT_DIRECT);
	if (ret)
		goto fail;
	ret = input_register_device(input);
	if (ret)
		goto fail;

	return 0;

fail:
	input_free_device(input);
	ts->input_dev = NULL;
	return ret;
}

static int fts_irq_init(struct fts_ts_data *ts)
{
	struct device_node *node;
	int ret;

	node = of_find_matching_node(NULL, touch_of_match);
	if (!node)
		return -ENODEV;
	ts->irq = irq_of_parse_and_map(node, 0);
	of_node_put(node);
	if (!ts->irq)
		return -EINVAL;

	tpd_gpio_as_int(GTP_INT_PORT);
	ret = request_threaded_irq(ts->irq, NULL, fts_irq_thread,
				   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   FTS_DRIVER_NAME, ts);
	if (ret) {
		irq_dispose_mapping(ts->irq);
		ts->irq = 0;
		return ret;
	}

	ts->client->irq = ts->irq;
	ts->irq_requested = true;
	return 0;
}

static ssize_t driver_info_show(struct device *dev,
				struct device_attribute *attribute, char *buffer)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct fts_ts_data *ts = i2c_get_clientdata(client);
	const char *state = "unknown";
	enum fts_pm_state pm_state;

	if (!ts)
		return -ENODEV;
	pm_state = READ_ONCE(ts->pm_state);
	if (pm_state >= FTS_PM_ACTIVE &&
	    pm_state < ARRAY_SIZE(fts_pm_state_names))
		state = fts_pm_state_names[pm_state];

	return scnprintf(buffer, PAGE_SIZE,
		"Driver Ver: %s\nResolution: (0,0)~(%d,%d)\n"
		"Max Touchs: %u\nIC ID: 0x%02x%02x\n"
		"BUS: I2C, addr: 0x%02x\nPM: %s\n",
		FTS_DRIVER_VERSION, FTS_X_MAX, FTS_Y_MAX, ts->max_points,
		ts->chip.chip_idh, ts->chip.chip_idl, client->addr, state);
}
static DEVICE_ATTR_RO(driver_info);

static ssize_t fw_version_show(struct device *dev,
			       struct device_attribute *attribute, char *buffer)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct fts_ts_data *ts = i2c_get_clientdata(client);
	u8 version = 0;
	int ret;

	if (!ts)
		return -ENODEV;

	mutex_lock(&ts->pm_lock);
	if (ts->pm_state != FTS_PM_ACTIVE) {
		ret = -EAGAIN;
	} else {
		ret = fts_read_reg(ts, FTS_REG_FW_VERSION, &version);
	}
	mutex_unlock(&ts->pm_lock);
	if (ret)
		return ret;

	return scnprintf(buffer, PAGE_SIZE, "0x%02x\n", version);
}
static DEVICE_ATTR_RO(fw_version);

static struct attribute *fts_read_only_attributes[] = {
	&dev_attr_driver_info.attr,
	&dev_attr_fw_version.attr,
	NULL,
};

static const struct attribute_group fts_read_only_group = {
	.attrs = fts_read_only_attributes,
};

int fts_enter_pen_mode(struct fts_ts_data *ts, bool enable)
{
	u8 requested = !!enable;
	u8 readback;
	int ret;

	ret = fts_write_reg(ts, FTS_REG_PEN_MODE, requested);
	if (ret < 0)
		return ret;

	ret = fts_read_reg(ts, FTS_REG_PEN_MODE, &readback);
	if (ret < 0)
		return ret;

	return readback == requested ? 0 : -EIO;
}

int k50_ft8057_set_hall_state(bool state)
{
	struct fts_ts_data *ts;
	int ret;

	mutex_lock(&fts_lifetime_lock);
	ts = fts_data;
	if (!ts) {
		ret = -ENODEV;
		goto out_lifetime;
	}

	mutex_lock(&ts->pm_lock);
	if (ts->pm_state != FTS_PM_ACTIVE)
		ret = -EAGAIN;
	else
		ret = fts_enter_pen_mode(ts, state);
	mutex_unlock(&ts->pm_lock);

out_lifetime:
	mutex_unlock(&fts_lifetime_lock);
	return ret;
}

static int fts_restore_hall_mode(struct fts_ts_data *ts)
{
	int hall_status;

	if (!get_hall_status) {
		FTS_INFO("Hall source unavailable; pen mode not restored");
		return 0;
	}

	hall_status = get_hall_status();
	if (hall_status == -ENODEV)
		return 0;
	if (hall_status < 0)
		return hall_status;
	return fts_enter_pen_mode(ts, !!hall_status);
}

static int fts_prepare_suspend_callback(struct device *device)
{
	struct fts_ts_data *ts;
	int ret;

	mutex_lock(&fts_lifetime_lock);
	ts = fts_data;
	if (!ts)
		goto no_device;

	mutex_lock(&ts->pm_lock);
	switch (ts->pm_state) {
	case FTS_PM_PREPARED:
		ret = 0;
		goto out;
	case FTS_PM_PREPARED_IO_ERROR:
		ret = ts->prepare_error;
		goto out;
	case FTS_PM_POWERED_DOWN:
		ret = 0;
		goto out;
	case FTS_PM_RESUMING:
		ret = -EBUSY;
		goto out;
	case FTS_PM_FAULT:
		ret = -EIO;
		goto out;
	case FTS_PM_PREPARING:
		ret = -EALREADY;
		goto out;
	case FTS_PM_ACTIVE:
		break;
	}

	WRITE_ONCE(ts->pm_state, FTS_PM_PREPARING);
	/* ESD and point-report watchdog work are not compiled in this candidate. */
	fts_irq_disable_sync(ts);
	ret = fts_write_reg(ts, FTS_REG_POWER_MODE, FTS_REG_POWER_SLEEP);
	ts->prepare_error = ret;
	if (ret)
		WRITE_ONCE(ts->pm_state, FTS_PM_PREPARED_IO_ERROR);
	else
		WRITE_ONCE(ts->pm_state, FTS_PM_PREPARED);
out:
	mutex_unlock(&ts->pm_lock);
	mutex_unlock(&fts_lifetime_lock);
	return ret;

no_device:
	mutex_unlock(&fts_lifetime_lock);
	return -ENODEV;
}

static void fts_suspend_callback(struct device *device)
{
	struct fts_ts_data *ts;
	int ret;

	mutex_lock(&fts_lifetime_lock);
	ts = fts_data;
	if (!ts)
		goto no_device;

	mutex_lock(&ts->pm_lock);
	if (ts->pm_state == FTS_PM_POWERED_DOWN)
		goto out;
	if (ts->pm_state == FTS_PM_ACTIVE) {
		FTS_ERROR("phase 2 observed ACTIVE; powering down without late I2C");
		fts_irq_disable_sync(ts);
	}

	fts_release_all_fingers(ts);
	tpd_gpio_output(GTP_RST_PORT, 0);
	msleep(5);
	ret = lcm_enp2_setting(false);
	if (ret) {
		FTS_ERROR("disable ENP2 failed: %d", ret);
		WRITE_ONCE(ts->pm_state, FTS_PM_FAULT);
	} else {
		WRITE_ONCE(ts->pm_state, FTS_PM_POWERED_DOWN);
	}
	ts->suspended = true;
out:
	mutex_unlock(&ts->pm_lock);
	mutex_unlock(&fts_lifetime_lock);
	return;

no_device:
	mutex_unlock(&fts_lifetime_lock);
}

static void fts_resume_callback(struct device *device)
{
	struct fts_ts_data *ts;
	int ret;

	mutex_lock(&fts_lifetime_lock);
	ts = fts_data;
	if (!ts)
		goto no_device;

	mutex_lock(&ts->pm_lock);
	if (ts->pm_state == FTS_PM_ACTIVE)
		goto out;

	WRITE_ONCE(ts->pm_state, FTS_PM_RESUMING);
	ret = lcm_enp2_setting(true);
	if (ret) {
		FTS_ERROR("enable ENP2 failed: %d", ret);
		goto fault;
	}

	fts_reset(200);
	ret = fts_wait_valid(ts);
	if (ret) {
		FTS_ERROR("touch did not become valid after resume: %d", ret);
		goto fault;
	}

	ret = fts_restore_hall_mode(ts);
	if (ret)
		FTS_ERROR("restore Hall pen mode failed: %d", ret);

	ts->prepare_error = 0;
	ts->suspended = false;
	WRITE_ONCE(ts->pm_state, FTS_PM_ACTIVE);
	fts_irq_enable(ts);
	goto out;

fault:
	WRITE_ONCE(ts->pm_state, FTS_PM_FAULT);
out:
	mutex_unlock(&ts->pm_lock);
	mutex_unlock(&fts_lifetime_lock);
	return;

no_device:
	mutex_unlock(&fts_lifetime_lock);
}

static int fts_i2c_probe(struct i2c_client *client,
			 const struct i2c_device_id *device_id)
{
	struct fts_ts_data *ts;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	mutex_lock(&fts_lifetime_lock);
	if (fts_data) {
		ret = -EBUSY;
		goto fail_unlock;
	}

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts) {
		ret = -ENOMEM;
		goto fail_unlock;
	}

	ts->client = client;
	ts->original_addr = client->addr;
	ts->max_points = tpd_dts_data.touch_max_num;
	if (!ts->max_points)
		ts->max_points = 5;
	if (ts->max_points > FTS_MAX_POINTS)
		ts->max_points = FTS_MAX_POINTS;
	mutex_init(&ts->report_lock);
	mutex_init(&ts->pm_lock);
	WRITE_ONCE(ts->pm_state, FTS_PM_ACTIVE);
	i2c_set_clientdata(client, ts);

	if (client->addr != FTS_I2C_SLAVE_ADDR) {
		FTS_INFO("[TPD]Change i2c addr 0x%02x to %x",
			 client->addr, FTS_I2C_SLAVE_ADDR);
		client->addr = FTS_I2C_SLAVE_ADDR;
	}
#ifdef CONFIG_MTK_I2C_EXTENSION
	client->timing = 400;
#endif

	ret = fts_bus_init(ts);
	if (ret)
		goto fail_clear;
	fts_reset(200);
	ret = fts_identify(ts);
	if (ret) {
		FTS_ERROR("not a supported FocalTech IC: %d", ret);
		goto fail_bus;
	}

	ret = fts_input_init(ts);
	if (ret)
		goto fail_bus;
	ret = fts_irq_init(ts);
	if (ret)
		goto fail_input;
	ret = sysfs_create_group(&client->dev.kobj, &fts_read_only_group);
	if (ret)
		goto fail_irq;

	fts_data = ts;
	ret = fts_restore_hall_mode(ts);
	if (ret)
		FTS_ERROR("set initial Hall pen mode failed: %d", ret);
	tpd_load_status = 1;
	FTS_INFO("%s, resolution=%dx%d, max-touch=%u, irq=%u",
		 FTS_DRIVER_VERSION, FTS_X_MAX, FTS_Y_MAX,
		 ts->max_points, ts->irq);
	mutex_unlock(&fts_lifetime_lock);
	return 0;

fail_irq:
	free_irq(ts->irq, ts);
	ts->irq_requested = false;
	irq_dispose_mapping(ts->irq);
	ts->irq = 0;
fail_input:
	input_unregister_device(ts->input_dev);
	ts->input_dev = NULL;
fail_bus:
	fts_bus_exit(ts);
fail_clear:
	client->addr = ts->original_addr;
	i2c_set_clientdata(client, NULL);

fail_unlock:
	mutex_unlock(&fts_lifetime_lock);
	return ret;
}

static int fts_i2c_remove(struct i2c_client *client)
{
	struct fts_ts_data *ts = i2c_get_clientdata(client);

	if (!ts)
		return 0;

	mutex_lock(&fts_lifetime_lock);
	if (fts_data == ts)
		fts_data = NULL;
	tpd_load_status = 0;
	sysfs_remove_group(&client->dev.kobj, &fts_read_only_group);
	mutex_lock(&ts->pm_lock);
	WRITE_ONCE(ts->pm_state, FTS_PM_FAULT);
	fts_irq_disable_sync(ts);
	mutex_unlock(&ts->pm_lock);
	if (ts->irq_requested) {
		free_irq(ts->irq, ts);
		ts->irq_requested = false;
		irq_dispose_mapping(ts->irq);
	}
	if (ts->input_dev)
		input_unregister_device(ts->input_dev);
	fts_bus_exit(ts);
	client->addr = ts->original_addr;
	i2c_set_clientdata(client, NULL);
	mutex_unlock(&fts_lifetime_lock);
	return 0;
}

static const struct i2c_device_id fts_i2c_ids[] = {
	{ "cap_touch", 0 },
	{ FTS_DRIVER_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fts_i2c_ids);

static const struct of_device_id fts_of_match[] = {
	{ .compatible = "mediatek,cap_touch" },
	{ }
};
MODULE_DEVICE_TABLE(of, fts_of_match);

static struct i2c_driver fts_i2c_driver = {
	.probe = fts_i2c_probe,
	.remove = fts_i2c_remove,
	.id_table = fts_i2c_ids,
	.driver = {
		.name = FTS_DRIVER_NAME,
		.of_match_table = fts_of_match,
		.suppress_bind_attrs = true,
	},
};

static int fts_tpd_local_init(void)
{
	return i2c_add_driver(&fts_i2c_driver);
}

static struct tpd_driver_t fts_tpd_driver = {
	.tpd_device_name = FTS_DRIVER_NAME,
	.tpd_local_init = fts_tpd_local_init,
	.suspend = fts_suspend_callback,
	.resume = fts_resume_callback,
};

static int __init fts_tpd_driver_init(void)
{
	int ret;

	FTS_INFO("Driver version: %s", FTS_DRIVER_VERSION);
	tpd_get_dts_info();
	ret = tpd_driver_add(&fts_tpd_driver);
	if (ret) {
		FTS_ERROR("add MTK TPD driver failed: %d", ret);
		return ret > 0 ? -EEXIST : ret;
	}

	ret = tpd_set_prepare_suspend(&fts_tpd_driver,
				      fts_prepare_suspend_callback);
	if (ret) {
		FTS_ERROR("register suspend prepare failed: %d", ret);
		tpd_driver_remove(&fts_tpd_driver);
	}
	return ret;
}

static void __exit fts_tpd_driver_exit(void)
{
	tpd_set_prepare_suspend(&fts_tpd_driver, NULL);
	i2c_del_driver(&fts_i2c_driver);
	tpd_driver_remove(&fts_tpd_driver);
}

module_init(fts_tpd_driver_init);
module_exit(fts_tpd_driver_exit);

MODULE_AUTHOR("FocalTech Driver Team; k50sv1_64_bsp project");
MODULE_DESCRIPTION("Minimal FocalTech FT8057 driver for k50sv1");
MODULE_LICENSE("GPL v2");
