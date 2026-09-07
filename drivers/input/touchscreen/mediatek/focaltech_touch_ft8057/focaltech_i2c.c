/*
 * MTK I2C transport for the k50sv1 FT8057 source candidate.
 *
 * Copyright (c) 2012-2020 FocalTech Systems, Ltd.
 * Copyright (C) 2026 k50sv1_64_bsp project
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2.
 */

#include <linux/errno.h>
#include <linux/slab.h>

#include "focaltech_core.h"

static int fts_transfer_result(int result, int expected)
{
	if (result == expected)
		return 0;
	if (result < 0)
		return result;
	return -EIO;
}

#ifdef CONFIG_MTK_I2C_EXTENSION
static int fts_dma_send(struct fts_ts_data *ts, int length)
{
	struct i2c_client *client = ts->client;
	unsigned short saved_addr = client->addr;
	int result = -EIO;
	int retry;

	client->addr = (saved_addr & I2C_MASK_FLAG) | I2C_DMA_FLAG;
	for (retry = 0; retry < FTS_I2C_RETRIES; retry++) {
		result = i2c_master_send(client, (char *)ts->bus_tx_dma, length);
		if (result == length)
			break;
	}
	client->addr = saved_addr;

	return fts_transfer_result(result, length);
}

static int fts_dma_receive(struct fts_ts_data *ts, int length)
{
	struct i2c_client *client = ts->client;
	unsigned short saved_addr = client->addr;
	int result = -EIO;
	int retry;

	memset(ts->bus_rx_buf, 0, length);
	client->addr = (saved_addr & I2C_MASK_FLAG) | I2C_DMA_FLAG;
	for (retry = 0; retry < FTS_I2C_RETRIES; retry++) {
		result = i2c_master_recv(client, (char *)ts->bus_rx_dma, length);
		if (result == length)
			break;
	}
	client->addr = saved_addr;

	return fts_transfer_result(result, length);
}
#else
static int fts_i2c_transfer(struct fts_ts_data *ts, int write_len,
			    int read_len)
{
	struct i2c_msg messages[2];
	int expected;
	int result = -EIO;
	int retry;

	memset(messages, 0, sizeof(messages));
	if (write_len) {
		messages[0].addr = ts->client->addr;
		messages[0].buf = ts->bus_tx_buf;
		messages[0].len = write_len;
	}
	if (read_len) {
		memset(ts->bus_rx_buf, 0, read_len);
		messages[write_len ? 1 : 0].addr = ts->client->addr;
		messages[write_len ? 1 : 0].flags = I2C_M_RD;
		messages[write_len ? 1 : 0].buf = ts->bus_rx_buf;
		messages[write_len ? 1 : 0].len = read_len;
	}

	expected = (write_len && read_len) ? 2 : 1;
	for (retry = 0; retry < FTS_I2C_RETRIES; retry++) {
		result = i2c_transfer(ts->client->adapter,
				      messages,
				      expected);
		if (result == expected)
			break;
	}

	return fts_transfer_result(result, expected);
}
#endif

int fts_read(struct fts_ts_data *ts, const u8 *write_buf, int write_len,
	     u8 *read_buf, int read_len)
{
	int ret;

	if (!ts || !ts->client || !read_buf || read_len <= 0 ||
	    read_len > FTS_BUS_BUFFER_SIZE || write_len < 0 ||
	    write_len > FTS_BUS_BUFFER_SIZE || (write_len && !write_buf))
		return -EINVAL;

	mutex_lock(&ts->bus_lock);
	if (write_len)
		memcpy(ts->bus_tx_buf, write_buf, write_len);

#ifdef CONFIG_MTK_I2C_EXTENSION
	ret = 0;
	if (write_len)
		ret = fts_dma_send(ts, write_len);
	if (!ret)
		ret = fts_dma_receive(ts, read_len);
#else
	ret = fts_i2c_transfer(ts, write_len, read_len);
#endif
	if (!ret)
		memcpy(read_buf, ts->bus_rx_buf, read_len);
	mutex_unlock(&ts->bus_lock);

	return ret;
}

int fts_write(struct fts_ts_data *ts, const u8 *write_buf, int write_len)
{
	int ret;

	if (!ts || !ts->client || !write_buf || write_len <= 0 ||
	    write_len > FTS_BUS_BUFFER_SIZE)
		return -EINVAL;

	mutex_lock(&ts->bus_lock);
	memcpy(ts->bus_tx_buf, write_buf, write_len);
#ifdef CONFIG_MTK_I2C_EXTENSION
	ret = fts_dma_send(ts, write_len);
#else
	ret = fts_i2c_transfer(ts, write_len, 0);
#endif
	mutex_unlock(&ts->bus_lock);

	return ret;
}

int fts_read_reg(struct fts_ts_data *ts, u8 reg, u8 *value)
{
	return fts_read(ts, &reg, 1, value, 1);
}

int fts_write_reg(struct fts_ts_data *ts, u8 reg, u8 value)
{
	u8 buffer[2] = { reg, value };

	return fts_write(ts, buffer, sizeof(buffer));
}

int fts_bus_init(struct fts_ts_data *ts)
{
	int ret;

	if (!ts || !ts->client)
		return -EINVAL;

	mutex_init(&ts->bus_lock);
#ifdef CONFIG_MTK_I2C_EXTENSION
	ret = dma_set_coherent_mask(&ts->client->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ts->bus_tx_buf = dma_alloc_coherent(&ts->client->dev,
					    FTS_BUS_BUFFER_SIZE,
					    &ts->bus_tx_dma, GFP_KERNEL);
	if (!ts->bus_tx_buf)
		return -ENOMEM;

	ts->bus_rx_buf = dma_alloc_coherent(&ts->client->dev,
					    FTS_BUS_BUFFER_SIZE,
					    &ts->bus_rx_dma, GFP_KERNEL);
	if (!ts->bus_rx_buf) {
		dma_free_coherent(&ts->client->dev, FTS_BUS_BUFFER_SIZE,
				  ts->bus_tx_buf, ts->bus_tx_dma);
		ts->bus_tx_buf = NULL;
		return -ENOMEM;
	}
#else
	ret = 0;
	ts->bus_tx_buf = kzalloc(FTS_BUS_BUFFER_SIZE, GFP_KERNEL);
	ts->bus_rx_buf = kzalloc(FTS_BUS_BUFFER_SIZE, GFP_KERNEL);
	if (!ts->bus_tx_buf || !ts->bus_rx_buf) {
		kfree(ts->bus_rx_buf);
		kfree(ts->bus_tx_buf);
		ts->bus_rx_buf = NULL;
		ts->bus_tx_buf = NULL;
		return -ENOMEM;
	}
#endif

	return ret;
}

void fts_bus_exit(struct fts_ts_data *ts)
{
	if (!ts || !ts->client)
		return;

#ifdef CONFIG_MTK_I2C_EXTENSION
	if (ts->bus_rx_buf)
		dma_free_coherent(&ts->client->dev, FTS_BUS_BUFFER_SIZE,
				  ts->bus_rx_buf, ts->bus_rx_dma);
	if (ts->bus_tx_buf)
		dma_free_coherent(&ts->client->dev, FTS_BUS_BUFFER_SIZE,
				  ts->bus_tx_buf, ts->bus_tx_dma);
#else
	kfree(ts->bus_rx_buf);
	kfree(ts->bus_tx_buf);
#endif
	ts->bus_rx_buf = NULL;
	ts->bus_tx_buf = NULL;
}
