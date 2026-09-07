/*
 * Fitted DW9761AF identity for k50sv1 MAINAF.
 *
 * Copyright (C) 2026 The LineageOS Project
 */
#ifndef __K50_DW9761AF_H__
#define __K50_DW9761AF_H__

#ifdef K50_DW9761AF_HOST_TEST
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
#else
#include <linux/types.h>
#endif

#define K50_DW9761AF_DRVNAME		"DW9761AF_DRV"
#define K50_DW9761AF_AFDRV		"DW9761AF"
#define K50_DW9761AF_I2C_WRITE		0x18
#define K50_DW9761AF_I2C_7BIT		0x0c
#define K50_DW9761AF_MACRO		1023
#define K50_DW9761AF_REG_PD		0x02
#define K50_DW9761AF_REG_POS		0x03
#define K50_DW9761AF_REG_MODE		0x06
#define K50_DW9761AF_REG_FREQ		0x07
#define K50_DW9761AF_REG_PRELOAD	0x08
#define K50_DW9761AF_PD_SHUTDOWN	0x01
#define K50_DW9761AF_PD_ACTIVE		0x00
#define K50_DW9761AF_PD_RING		0x02
#define K50_DW9761AF_SAC		0x60
#define K50_DW9761AF_FREQ		0x3e
#define K50_DW9761AF_PRELOAD		0x73

static inline u8 k50_dw9761af_pos_msb(u16 pos)
{
	return (u8)((pos >> 8) & 0x03);
}

static inline u8 k50_dw9761af_pos_lsb(u16 pos)
{
	return (u8)(pos & 0xff);
}

static inline u16 k50_dw9761af_pos_from_bytes(u8 msb, u8 lsb)
{
	return (u16)(((msb & 0x03) << 8) | lsb);
}

static inline bool k50_dw9761af_addr_ok(u8 write_id)
{
	return write_id == K50_DW9761AF_I2C_WRITE;
}

#endif
