/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/leds.h>
#include <linux/reboot.h>
#include <linux/spinlock.h>
#include <mach/mt_pbm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <mt-plat/upmu_common.h>

#include "kd_flashlight.h"
#include "kd_flashlight_type.h"

/* K50's main flash is wired to MT6353 ISINK0/1. */
struct flash_reg {
	unsigned int addr;
	unsigned int value;
	unsigned int mask;
	unsigned int shift;
};

#define FLASH_REG(field, value) \
	{ field##_ADDR, value, field##_MASK, field##_SHIFT }
#define FLASH_WRITE(field, value) \
	flash_write(field##_ADDR, value, field##_MASK, field##_SHIFT)

/* Preserve the stock initialization order, current step and PWM duty. */
static const struct flash_reg flash_init_regs[] = {
	FLASH_REG(PMIC_ISINK_CH0_EN, 0),
	FLASH_REG(PMIC_CLK_DRV_32K_CK_PDN, 0),
	FLASH_REG(PMIC_CLK_DRV_ISINK0_CK_PDN, 0),
	FLASH_REG(PMIC_ISINK_CH0_MODE, 0),
	FLASH_REG(PMIC_ISINK_CH0_STEP, 5),
	FLASH_REG(PMIC_ISINK_DIM0_DUTY, 31),
	FLASH_REG(PMIC_ISINK_DIM0_FSEL, 0),
	FLASH_REG(PMIC_ISINK_CH1_EN, 0),
	FLASH_REG(PMIC_CLK_DRV_32K_CK_PDN, 0),
	FLASH_REG(PMIC_CLK_DRV_ISINK1_CK_PDN, 0),
	FLASH_REG(PMIC_ISINK_CH1_MODE, 0),
	FLASH_REG(PMIC_ISINK_CH1_STEP, 5),
	FLASH_REG(PMIC_ISINK_DIM1_DUTY, 31),
	FLASH_REG(PMIC_ISINK_DIM1_FSEL, 0),
};

/* Ownership/state use flash_lock; PMIC accesses use flash_hw_lock. */
static DEFINE_MUTEX(flash_lock);
static DEFINE_MUTEX(flash_hw_lock);
static bool flash_in_use;
/* LED requests are queued because the 3.18 brightness callback cannot sleep. */
static DEFINE_SPINLOCK(torch_request_lock);
static bool torch_requested;
static bool torch_enabled;
static bool torch_state_unknown;
static unsigned int torch_request_generation;
static unsigned int torch_low_power;
static bool torch_shutdown;
static int torch_result;
static void torch_work_func(struct work_struct *work);
static DECLARE_WORK(torch_work, torch_work_func);
static bool flash_timer_initialized;
static unsigned int flash_timeout_ms = 1000;
static struct hrtimer flash_timer;
static void flash_timeout_work_func(struct work_struct *work);
static DECLARE_WORK(flash_timeout_work, flash_timeout_work_func);

static int flash_write(unsigned int addr, unsigned int value,
		       unsigned int mask, unsigned int shift)
{
	int ret = pmic_config_interface(addr, value, mask, shift);

	/* pmic_set_register_value() discards these PWRAP failures. */
	if (ret) {
		pr_err("k50-flashlight: PMIC register %#x write failed: %d\n",
		       addr, ret);
		return ret < 0 ? ret : -EIO;
	}
	return 0;
}

/* The hardware helpers run under flash_hw_lock. */
static int flash_disable(void)
{
	int ret, next;

	ret = FLASH_WRITE(PMIC_ISINK_CH0_EN, 0);
	/* Always attempt both channels, including after a partial enable. */
	next = FLASH_WRITE(PMIC_ISINK_CH1_EN, 0);
	return ret ? ret : next;
}

static int flash_enable(void)
{
	int ret;

	ret = FLASH_WRITE(PMIC_ISINK_CH0_EN, 1);
	if (!ret)
		ret = FLASH_WRITE(PMIC_ISINK_CH1_EN, 1);
	if (ret)
		flash_disable();
	return ret;
}

static int flash_init(void)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(flash_init_regs); i++) {
		const struct flash_reg *reg = &flash_init_regs[i];

		ret = flash_write(reg->addr, reg->value, reg->mask, reg->shift);
		if (ret) {
			flash_disable();
			return ret;
		}
	}
	return 0;
}

static void flash_timeout_work_func(struct work_struct *work)
{
	mutex_lock(&flash_hw_lock);
	flash_disable();
	mutex_unlock(&flash_hw_lock);
}

static enum hrtimer_restart flash_timeout_callback(struct hrtimer *timer)
{
	schedule_work(&flash_timeout_work);
	return HRTIMER_NORESTART;
}

/* Hold flash_lock, but never flash_hw_lock: the work may be waiting for it. */
static void flash_cancel_timeout(void)
{
	if (flash_timer_initialized)
		hrtimer_cancel(&flash_timer);
	cancel_work_sync(&flash_timeout_work);
}

static int constant_flashlight_ioctl(unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	mutex_lock(&flash_lock);
	if (!flash_in_use) {
		ret = -ENODEV;
		goto out;
	}

	switch (cmd) {
	case FLASH_IOC_SET_TIME_OUT_TIME_MS:
		/* The HAL passes an int; reject negatives and values it cannot hold. */
		if (arg > INT_MAX)
			ret = -EINVAL;
		else
			flash_timeout_ms = arg;
		break;
	case FLASH_IOC_SET_DUTY:
	case FLASH_IOC_SET_STEP:
		/* Stock accepts these without changing the fixed PMIC current. */
		break;
	case FLASH_IOC_SET_ONOFF:
		/* Drain an earlier exposure before changing or rearming the light. */
		flash_cancel_timeout();
		mutex_lock(&flash_hw_lock);
		ret = arg == 1 ? flash_enable() : flash_disable();
		mutex_unlock(&flash_hw_lock);
		if (!ret && arg == 1 && flash_timeout_ms) {
			ktime_t timeout;

			timeout = ktime_set(flash_timeout_ms / 1000,
				(flash_timeout_ms % 1000) * NSEC_PER_MSEC);
			hrtimer_start(&flash_timer, timeout, HRTIMER_MODE_REL);
		}
		break;
	default:
		ret = -EPERM;
		break;
	}
out:
	mutex_unlock(&flash_lock);
	return ret;
}

static int constant_flashlight_open(void *arg)
{
	int ret;

	mutex_lock(&flash_lock);
	if (torch_shutdown || torch_low_power) {
		ret = -EPERM;
		goto out;
	}
	if (flash_in_use || torch_enabled || torch_state_unknown) {
		ret = -EBUSY;
		goto out;
	}
	if (!flash_timer_initialized) {
		hrtimer_init(&flash_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		flash_timer.function = flash_timeout_callback;
		flash_timer_initialized = true;
	}
	flash_cancel_timeout();
	mutex_lock(&flash_hw_lock);
	ret = flash_init();
	mutex_unlock(&flash_hw_lock);
	if (!ret) {
		flash_timeout_ms = 1000;
		flash_in_use = true;
	}
out:
	mutex_unlock(&flash_lock);
	return ret;
}

static int constant_flashlight_release(void *arg)
{
	int ret = 0;

	mutex_lock(&flash_lock);
	if (flash_in_use) {
		flash_cancel_timeout();
		mutex_lock(&flash_hw_lock);
		ret = flash_disable();
		mutex_unlock(&flash_hw_lock);
		flash_in_use = false;
	}
	mutex_unlock(&flash_lock);
	return ret;
}

static FLASHLIGHT_FUNCTION_STRUCT constantFlashlightFunc = {
	constant_flashlight_open,
	constant_flashlight_release,
	constant_flashlight_ioctl
};

MUINT32 constantFlashlightInit(PFLASHLIGHT_FUNCTION_STRUCT *pfFunc)
{
	if (pfFunc)
		*pfFunc = &constantFlashlightFunc;
	return 0;
}
EXPORT_SYMBOL(constantFlashlightInit);

/* The stock PMIC implementation has no interrupt-driven high-current mode. */
ssize_t strobe_VDIrq(void)
{
	return 0;
}
EXPORT_SYMBOL(strobe_VDIrq);

/* flash_lock serializes the two users; flash_hw_lock protects PMIC accesses. */
static int torch_read_hardware(bool *enabled)
{
	unsigned int first, second;
	int ret;

	ret = pmic_read_interface(PMIC_ISINK_CH0_EN_ADDR, &first,
		PMIC_ISINK_CH0_EN_MASK, PMIC_ISINK_CH0_EN_SHIFT);
	if (!ret)
		ret = pmic_read_interface(PMIC_ISINK_CH1_EN_ADDR, &second,
			PMIC_ISINK_CH1_EN_MASK, PMIC_ISINK_CH1_EN_SHIFT);
	if (ret)
		return ret < 0 ? ret : -EIO;
	*enabled = first || second;
	return 0;
}

static void torch_work_func(struct work_struct *work)
{
	unsigned int generation;
	unsigned long flags;
	bool requested, enabled, blocked;
	int ret, operation_ret, read_ret;

	for (;;) {
		spin_lock_irqsave(&torch_request_lock, flags);
		generation = torch_request_generation;
		requested = torch_requested;
		spin_unlock_irqrestore(&torch_request_lock, flags);

		mutex_lock(&flash_lock);
		blocked = torch_low_power || torch_shutdown;
		ret = requested && blocked ? -EPERM : 0;
		if (flash_in_use) {
			if (requested && !ret)
				ret = -EBUSY;
		} else {
			/* Reserve the power budget before enabling the LED, as the HAL does. */
			if (requested && !blocked)
				kicker_pbm_by_flash(true);
			mutex_lock(&flash_hw_lock);
			if (requested && !blocked) {
				operation_ret = torch_enabled ? 0 : flash_init();
				if (!operation_ret)
					operation_ret = flash_enable();
			} else {
				/* A newer ON request cannot cancel a low-power/shutdown OFF. */
				operation_ret = flash_disable();
			}
			if (operation_ret)
				ret = operation_ret;
			read_ret = torch_read_hardware(&enabled);
			if (read_ret) {
				if (!ret)
					ret = read_ret;
				/* Failed readback after enable must attempt a safe shutdown. */
				flash_disable();
				read_ret = torch_read_hardware(&enabled);
			}
			torch_state_unknown = read_ret != 0;
			if (!read_ret)
				torch_enabled = enabled;
			mutex_unlock(&flash_hw_lock);
			/* Keep the budget reserved until OFF is positively confirmed. */
			kicker_pbm_by_flash(torch_enabled || torch_state_unknown);
		}
		torch_result = ret;

		spin_lock_irqsave(&torch_request_lock, flags);
		if (generation == torch_request_generation) {
			/* A rejected request must not reserve the legacy flash forever. */
			if (ret)
				torch_requested = torch_enabled || torch_state_unknown;
			spin_unlock_irqrestore(&torch_request_lock, flags);
			mutex_unlock(&flash_lock);
			return;
		}
		spin_unlock_irqrestore(&torch_request_lock, flags);
		mutex_unlock(&flash_lock);
	}
}

static void torch_brightness_set(struct led_classdev *led,
				enum led_brightness brightness)
{
	unsigned long flags;

	spin_lock_irqsave(&torch_request_lock, flags);
	torch_requested = brightness != LED_OFF;
	++torch_request_generation;
	spin_unlock_irqrestore(&torch_request_lock, flags);
	schedule_work(&torch_work);
}

static enum led_brightness torch_brightness_get(struct led_classdev *led)
{
	return READ_ONCE(torch_enabled) ? 1 : LED_OFF;
}

/*
 * Completed state for userspace: "enabled available errno". Unlike the LED
 * brightness attribute, this waits for asynchronous writes and reports errors.
 * Read failures propagate to userspace instead of returning the requested state.
 */
static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	bool enabled, was_unknown;
	int ret, available, result;

	flush_work(&torch_work);
	mutex_lock(&flash_lock);
	mutex_lock(&flash_hw_lock);
	ret = torch_read_hardware(&enabled);
	mutex_unlock(&flash_hw_lock);
	if (!flash_in_use) {
		was_unknown = torch_state_unknown;
		torch_state_unknown = ret != 0;
		if (!ret) {
			torch_enabled = enabled;
			if (was_unknown)
				kicker_pbm_by_flash(torch_enabled);
		}
	}
	available = !flash_in_use && !torch_low_power && !torch_shutdown;
	result = torch_result;
	enabled = torch_enabled;
	mutex_unlock(&flash_lock);
	if (ret)
		return ret;
	return scnprintf(buf, PAGE_SIZE, "%d %d %d\n", enabled, available, result);
}
static DEVICE_ATTR_RO(status);

static struct attribute *torch_attrs[] = {
	&dev_attr_status.attr,
	NULL,
};
static const struct attribute_group torch_group = {
	.attrs = torch_attrs,
};
static const struct attribute_group *torch_groups[] = {
	&torch_group,
	NULL,
};
static struct led_classdev torch_led = {
	.name = "k50:torch",
	.max_brightness = 1,
	.brightness_set = torch_brightness_set,
	.brightness_get = torch_brightness_get,
	.groups = torch_groups,
};

/* Voltage and capacity callbacks are separate; clearing one cannot clear both. */
void k50_torch_set_low_power(unsigned int source, bool blocked)
{
	mutex_lock(&flash_lock);
	if (blocked)
		torch_low_power |= source;
	else
		torch_low_power &= ~source;
	mutex_unlock(&flash_lock);
	if (blocked)
		torch_brightness_set(&torch_led, LED_OFF);
}
EXPORT_SYMBOL(k50_torch_set_low_power);

static int torch_reboot(struct notifier_block *notifier,
			unsigned long action, void *data)
{
	mutex_lock(&flash_lock);
	torch_shutdown = true;
	mutex_unlock(&flash_lock);
	torch_brightness_set(&torch_led, LED_OFF);
	flush_work(&torch_work);
	/* Also drain the legacy flash timer before the PMIC is shut down. */
	constant_flashlight_release(NULL);
	return NOTIFY_DONE;
}
static struct notifier_block torch_reboot_notifier = {
	.notifier_call = torch_reboot,
};

static int __init constant_flashlight_init(void)
{
	int ret;

	ret = led_classdev_register(NULL, &torch_led);
	if (ret)
		return ret;
	ret = register_reboot_notifier(&torch_reboot_notifier);
	if (ret) {
		led_classdev_unregister(&torch_led);
		cancel_work_sync(&torch_work);
	}
	return ret;
}
module_init(constant_flashlight_init);

static void __exit constant_flashlight_exit(void)
{
	unregister_reboot_notifier(&torch_reboot_notifier);
	torch_reboot(NULL, 0, NULL);
	led_classdev_unregister(&torch_led);
	cancel_work_sync(&torch_work);
}
module_exit(constant_flashlight_exit);

MODULE_DESCRIPTION("K50 MT6353 ISINK main flash and standalone torch");
MODULE_LICENSE("GPL v2");
