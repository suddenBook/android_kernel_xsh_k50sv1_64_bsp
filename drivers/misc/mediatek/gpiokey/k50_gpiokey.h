/*
 * Pure state helpers for the fitted k50sv1 Hall path.
 *
 * Copyright (C) 2026 The LineageOS Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 */
#ifndef __K50_GPIOKEY_H__
#define __K50_GPIOKEY_H__

#ifdef K50_HALL_HOST_TEST
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
#ifndef IRQ_TYPE_LEVEL_HIGH
#define IRQ_TYPE_LEVEL_HIGH 0x00000004
#endif
#ifndef IRQ_TYPE_LEVEL_LOW
#define IRQ_TYPE_LEVEL_LOW 0x00000008
#endif
#else
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#endif

#define K50_HALL_GPIO			7
#define K50_HALL_EINT			7
#define K50_HALL_LINUX_IRQ_STOCK	295
#define K50_HALL_DEBOUNCE_MS		7
#define K50_HALL_WAKE_TIMEOUT_MS	1000

struct k50_hall_transition {
	bool changed;
	int state;
	u32 next_irq_type;
};

static inline u32 k50_hall_irq_type_for_state(int state)
{
	return state ? IRQ_TYPE_LEVEL_LOW : IRQ_TYPE_LEVEL_HIGH;
}

static inline int k50_hall_transition(int current_state, int sample,
	struct k50_hall_transition *transition)
{
	if (!transition || (current_state != 0 && current_state != 1))
		return -EINVAL;
	if (sample < 0)
		return sample;
	if (sample != 0 && sample != 1)
		return -EINVAL;

	transition->changed = sample != current_state;
	transition->state = sample;
	transition->next_irq_type = k50_hall_irq_type_for_state(sample);
	return 0;
}

#endif /* __K50_GPIOKEY_H__ */
