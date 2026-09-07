/*
 * WUSB3801X USB Type-C configuration-channel controller, k50sv1_64_bsp.
 *
 * Register map and state names follow the WillSemi reference driver
 * (WUSB3801x rev 2.0, drivers/usb/wusb3801x/wusb3801x.h, 2016).  The
 * board glue (power pin, analog audio switch, ACCDET hand-off, EINT
 * lookup) reproduces the shipped k50sv1_64_bsp kernel (work evidence
 * E-163).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __WUSB3801X_H__
#define __WUSB3801X_H__

#include <linux/types.h>

#define WUSB3801_REG_VERSION_ID		0x01
#define WUSB3801_REG_CONTROL0		0x02
#define WUSB3801_REG_INTERRUPT		0x03
#define WUSB3801_REG_STATUS		0x04
#define WUSB3801_REG_CONTROL1		0x05
#define WUSB3801_REG_TEST_02		0x08
#define WUSB3801_REG_TEST_09		0x0F
#define WUSB3801_REG_TEST_0A		0x10
#define WUSB3801_REG_LAST		0x18

#define WUSB3801_VENDOR_ID		0x06
#define WUSB3801_VENDOR_ID_MASK		0x07
#define WUSB3801_VERSION_ID_MASK	0xF8

/* CONTROL0 */
#define BIT_REG_CTRL0_DIS_ACC		(0x01 << 7)
#define BIT_REG_CTRL0_TRY_SRC		(0x02 << 5)
#define BIT_REG_CTRL0_TRY_SNK		(0x01 << 5)
#define BIT_REG_CTRL0_CUR_DEF		(0x00 << 3)
#define BIT_REG_CTRL0_CUR_1P5		(0x01 << 3)
#define BIT_REG_CTRL0_CUR_3P0		(0x02 << 3)
#define BIT_REG_CTRL0_RLE_SNK		(0x00 << 1)
#define BIT_REG_CTRL0_RLE_SRC		(0x01 << 1)
#define BIT_REG_CTRL0_RLE_DRP		(0x02 << 1)
#define BIT_REG_CTRL0_INT_MSK		(0x01 << 0)

#define WUSB3801_HOST_CUR_MASK		0x18
#define WUSB3801_MODE_MASK		0xE6	/* role-relevant bits */
#define WUSB3801_ROLE_MASK		0x06
#define WUSB3801_INT_MASK		0x01

#define WUSB3801_DRP_ACC		(BIT_REG_CTRL0_RLE_DRP)
#define WUSB3801_DRP			(BIT_REG_CTRL0_RLE_DRP | BIT_REG_CTRL0_DIS_ACC)
#define WUSB3801_SNK_ACC		(BIT_REG_CTRL0_RLE_SNK)
#define WUSB3801_SNK			(BIT_REG_CTRL0_RLE_SNK | BIT_REG_CTRL0_DIS_ACC)
#define WUSB3801_SRC_ACC		(BIT_REG_CTRL0_RLE_SRC)
#define WUSB3801_SRC			(BIT_REG_CTRL0_RLE_SRC | BIT_REG_CTRL0_DIS_ACC)
#define WUSB3801_DRP_PREFER_SRC_ACC	(WUSB3801_DRP_ACC | BIT_REG_CTRL0_TRY_SRC)
#define WUSB3801_DRP_PREFER_SRC		(WUSB3801_DRP | BIT_REG_CTRL0_TRY_SRC)
#define WUSB3801_DRP_PREFER_SNK_ACC	(WUSB3801_DRP_ACC | BIT_REG_CTRL0_TRY_SNK)
#define WUSB3801_DRP_PREFER_SNK		(WUSB3801_DRP | BIT_REG_CTRL0_TRY_SNK)

/* INTERRUPT */
#define WUSB3801_INT_ATTACH		(0x01 << 0)
#define WUSB3801_INT_DETACH		(0x01 << 1)
#define WUSB3801_INT_STS_MASK		0x03

/* STATUS */
#define WUSB3801_VBUS_OK		0x80
#define WUSB3801_BCLVL_MASK		0x60
#define WUSB3801_SNK_0MA		(0x00 << 5)
#define WUSB3801_SNK_DEFAULT		(0x01 << 5)
#define WUSB3801_SNK_1500MA		(0x02 << 5)
#define WUSB3801_SNK_3000MA		(0x03 << 5)
#define WUSB3801_TYPE_MASK		0x1C
#define WUSB3801_TYPE_INVALID		(0x00)
#define WUSB3801_TYPE_SNK		(0x01 << 2)
#define WUSB3801_TYPE_SRC		(0x02 << 2)
#define WUSB3801_TYPE_AUD_ACC		(0x03 << 2)
#define WUSB3801_TYPE_DBG_ACC		(0x04 << 2)
#define WUSB3801_ATTACH			0x1C
#define WUSB3801_POLARITY_CC_MASK	0x03

/* CONTROL1 */
#define BIT_REG_CTRL1_SM_RST		(0x01 << 0)
#define WUSB3801_DISABLED		0x0A

#define WUSB3801_HOST_DEFAULT		0
#define WUSB3801_HOST_1500MA		1
#define WUSB3801_HOST_3000MA		2
#define WUSB3801_TGL_40MS		0
#define WUSB3801_INT_ENABLE		0x00

/* Chip states (reference driver numbering) */
#define WUSB3801_STATE_DISABLED		0x00
#define WUSB3801_STATE_ERROR_RECOVERY	0x01
#define WUSB3801_STATE_UNATTACHED_SNK	0x02
#define WUSB3801_STATE_UNATTACHED_SRC	0x03
#define WUSB3801_STATE_ATTACHED_SNK	0x06
#define WUSB3801_STATE_ATTACHED_SRC	0x07
#define WUSB3801_STATE_AUDIO_ACCESSORY	0x08
#define WUSB3801_STATE_DEBUG_ACCESSORY	0x09

#define WUSB3801_WAKE_LOCK_TIMEOUT_MS	1000

/* ACCDET's USB-C audio hand-off, drivers/misc/mediatek/accdet/mt6755 */
extern void typec_headphone_irq_handler(int plugged);

#endif /* __WUSB3801X_H__ */
