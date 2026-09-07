/*
 * Copyright (C) 2026 k50sv1_64_bsp project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>

static struct i2c_client *tps65132_client;
static DEFINE_MUTEX(tps65132_lock);

int tps65132_write_bytes(u8 reg, u8 value)
{
	struct i2c_client *client;
	u8 data[2] = { reg, value };
	int ret;

	mutex_lock(&tps65132_lock);
	client = tps65132_client;
	if (!client) {
		ret = -ENODEV;
		goto out;
	}

	ret = i2c_master_send(client, data, sizeof(data));
	if (ret == 2)
		ret = 0;
	else if (ret >= 0)
		ret = -EIO;
out:
	mutex_unlock(&tps65132_lock);
	return ret;
}

static int tps65132_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	mutex_lock(&tps65132_lock);
	if (tps65132_client) {
		mutex_unlock(&tps65132_lock);
		return -EBUSY;
	}
	tps65132_client = client;
	mutex_unlock(&tps65132_lock);
	return 0;
}

static int tps65132_remove(struct i2c_client *client)
{
	mutex_lock(&tps65132_lock);
	if (tps65132_client == client)
		tps65132_client = NULL;
	mutex_unlock(&tps65132_lock);
	return 0;
}

static const struct i2c_device_id tps65132_id[] = {
	{ "tps65132", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tps65132_id);

static const struct of_device_id tps65132_of_match[] = {
	{ .compatible = "mediatek,i2c_lcd_bias" },
	{ }
};
MODULE_DEVICE_TABLE(of, tps65132_of_match);

static struct i2c_driver tps65132_driver = {
	.probe = tps65132_probe,
	.remove = tps65132_remove,
	.id_table = tps65132_id,
	.driver = {
		.name = "tps65132",
		.of_match_table = tps65132_of_match,
		.suppress_bind_attrs = true,
	},
};
module_i2c_driver(tps65132_driver);

MODULE_DESCRIPTION("TPS65132-compatible LCD bias driver");
MODULE_LICENSE("GPL v2");
