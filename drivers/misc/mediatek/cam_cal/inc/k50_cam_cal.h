/*
 * Fitted CAM_CAL I2C identity for k50sv1.
 *
 * Stock CONFIG_CUSTOM_KERNEL_CAM_CAL_DRV compiles the legacy
 * s5k2p8_eeprom / ov8858_eeprom files. Those names are the stock
 * CAM_CAL_DRV identity, not the fitted IMX145/GC5025 sensors.
 * Live clients: CAM_CAL_DRV1 on i2c-2 @0x51 and CAM_CAL_DRV2 on
 * i2c-0 @0x51 (8-bit 0xA2). Population/use stays WI-045.
 *
 * Copyright (C) 2026 The LineageOS Project
 */
#ifndef __K50_CAM_CAL_H__
#define __K50_CAM_CAL_H__

#ifdef K50_CAMCAL_HOST_TEST
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
#else
#include <linux/types.h>
#endif

#define K50_CAMCAL_SLAVE_8BIT		0xA2
#define K50_CAMCAL_SLAVE_7BIT		0x51
#define K50_CAMCAL_DRV1_NAME		"CAM_CAL_DRV1"
#define K50_CAMCAL_DRV1_BUS		2
#define K50_CAMCAL_DRV2_NAME		"CAM_CAL_DRV2"
#define K50_CAMCAL_DRV2_BUS		0

static inline bool k50_camcal_slave_ok(u8 write_id)
{
	return write_id == K50_CAMCAL_SLAVE_8BIT;
}

#endif
