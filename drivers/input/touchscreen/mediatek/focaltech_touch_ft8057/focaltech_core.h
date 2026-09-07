/*
 * Minimal FocalTech V3.4 interface for the k50sv1 FT8057 controller.
 *
 * The protocol structure follows the GPLv2 FocalTech V3.4 driver.  Board
 * constants and chip IDs come from the shipped k50sv1 kernel and DTBO.
 *
 * Copyright (c) 2012-2020 FocalTech Systems, Ltd.
 * Copyright (C) 2026 k50sv1_64_bsp project
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2.
 */

#ifndef _K50_FOCALTECH_CORE_H
#define _K50_FOCALTECH_CORE_H

#include <linux/dma-mapping.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include "tpd.h"

#define FTS_DRIVER_NAME		"fts_ts"
#define FTS_DRIVER_VERSION	"Focaltech V3.4 20211214"

#define FTS_I2C_SLAVE_ADDR	0x38
#define FTS_I2C_RETRIES		3
#define FTS_BUS_BUFFER_SIZE	4096

#define FTS_X_MAX		720
#define FTS_Y_MAX		1560
#define FTS_MAX_POINTS		10
#define FTS_ONE_TOUCH_LEN	6
#define FTS_TOUCH_DATA_LEN	(FTS_MAX_POINTS * FTS_ONE_TOUCH_LEN + 2)

#define FTS_REG_CHIP_ID		0xA3
#define FTS_REG_CHIP_ID2		0x9F
#define FTS_REG_POWER_MODE	0xA5
#define FTS_REG_POWER_SLEEP	0x03
#define FTS_REG_FW_VERSION	0xA6
#define FTS_REG_PEN_MODE		0xC0

#define FTS_TOUCH_DOWN		0
#define FTS_TOUCH_UP		1
#define FTS_TOUCH_CONTACT	2
#define FTS_MAX_ID		0x0A

#define FTS_INFO(fmt, args...) \
	pr_info("[FTS_TS/I]%s:" fmt "\n", __func__, ##args)
#define FTS_ERROR(fmt, args...) \
	pr_err("[FTS_TS/E]%s:" fmt "\n", __func__, ##args)

struct fts_chip_id {
	u8 type;
	u8 chip_idh;
	u8 chip_idl;
	u8 rom_idh;
	u8 rom_idl;
	u8 pb_idh;
	u8 pb_idl;
	u8 bl_idh;
	u8 bl_idl;
};

struct fts_cid_info {
	u8 type;
	u16 ids[8];
};

struct fts_event {
	u16 x;
	u16 y;
	u8 pressure;
	u8 flag;
	u8 id;
	u8 area;
};

enum fts_pm_state {
	FTS_PM_ACTIVE,
	FTS_PM_PREPARING,
	FTS_PM_PREPARED,
	FTS_PM_PREPARED_IO_ERROR,
	FTS_PM_POWERED_DOWN,
	FTS_PM_RESUMING,
	FTS_PM_FAULT,
};

struct fts_ts_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct mutex bus_lock;
	struct mutex report_lock;
	struct mutex pm_lock;
	struct fts_chip_id chip;
	struct fts_cid_info cid;
	struct fts_event events[FTS_MAX_POINTS];
	u8 touch_buf[FTS_TOUCH_DATA_LEN];
	u8 *bus_tx_buf;
	u8 *bus_rx_buf;
	dma_addr_t bus_tx_dma;
	dma_addr_t bus_rx_dma;
	unsigned short original_addr;
	unsigned int irq;
	unsigned int max_points;
	unsigned long active_slots;
	enum fts_pm_state pm_state;
	int prepare_error;
	bool irq_requested;
	bool irq_disabled;
	bool suspended;
};

extern struct fts_ts_data *fts_data;

int fts_bus_init(struct fts_ts_data *ts);
void fts_bus_exit(struct fts_ts_data *ts);
int fts_read(struct fts_ts_data *ts, const u8 *write_buf, int write_len,
	     u8 *read_buf, int read_len);
int fts_write(struct fts_ts_data *ts, const u8 *write_buf, int write_len);
int fts_read_reg(struct fts_ts_data *ts, u8 reg, u8 *value);
int fts_write_reg(struct fts_ts_data *ts, u8 reg, u8 value);

int fts_check_cid(struct fts_ts_data *ts, u8 id_h);
int fts_enter_pen_mode(struct fts_ts_data *ts, bool enable);
int k50_ft8057_set_hall_state(bool state);

#endif /* _K50_FOCALTECH_CORE_H */
