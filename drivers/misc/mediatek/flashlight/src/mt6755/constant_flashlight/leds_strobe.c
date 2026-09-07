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
#include <linux/errno.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
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

/* Only ioctl/open/release take flash_lock; timeout work takes flash_hw_lock. */
static DEFINE_MUTEX(flash_lock);
static DEFINE_MUTEX(flash_hw_lock);
static bool flash_in_use;
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
	if (flash_in_use) {
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

static void __exit constant_flashlight_exit(void)
{
	constant_flashlight_release(NULL);
}
module_exit(constant_flashlight_exit);

MODULE_DESCRIPTION("K50 MT6353 ISINK main flash");
MODULE_LICENSE("GPL v2");
