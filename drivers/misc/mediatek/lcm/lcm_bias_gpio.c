/*
 * Copyright (C) 2026 k50sv1_64_bsp project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <linux/pinctrl/consumer.h>

#include "k50_lcm_bias.h"

struct k50_lcm_bias {
	struct pinctrl *pinctrl;
	struct pinctrl_state *enn_high;
	struct pinctrl_state *enn_low;
	struct pinctrl_state *enp_high;
	struct pinctrl_state *enp_low;
	struct pinctrl_state *enp2_high;
	struct pinctrl_state *enp2_low;
};

static struct k50_lcm_bias *k50_bias;
static DEFINE_MUTEX(k50_bias_lock);

enum k50_lcm_bias_line {
	K50_LCM_BIAS_ENN,
	K50_LCM_BIAS_ENP,
	K50_LCM_BIAS_ENP2,
};

static int k50_lcm_bias_select(enum k50_lcm_bias_line line, bool enable)
{
	struct k50_lcm_bias *bias;
	struct pinctrl_state *state;
	int ret;

	mutex_lock(&k50_bias_lock);
	bias = k50_bias;
	if (!bias) {
		ret = -ENODEV;
		goto out;
	}

	switch (line) {
	case K50_LCM_BIAS_ENN:
		state = enable ? bias->enn_high : bias->enn_low;
		break;
	case K50_LCM_BIAS_ENP:
		state = enable ? bias->enp_high : bias->enp_low;
		break;
	case K50_LCM_BIAS_ENP2:
		state = enable ? bias->enp2_high : bias->enp2_low;
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	if (IS_ERR_OR_NULL(state)) {
		ret = -EINVAL;
		goto out;
	}
	ret = pinctrl_select_state(bias->pinctrl, state);
out:
	mutex_unlock(&k50_bias_lock);
	return ret;
}

int lcm_enn_setting(bool enable)
{
	return k50_lcm_bias_select(K50_LCM_BIAS_ENN, enable);
}

int lcm_enp_setting(bool enable)
{
	return k50_lcm_bias_select(K50_LCM_BIAS_ENP, enable);
}

int lcm_enp2_setting(bool enable)
{
	return k50_lcm_bias_select(K50_LCM_BIAS_ENP2, enable);
}

static struct pinctrl_state *k50_lookup_state(struct device *dev,
					      struct pinctrl *pinctrl,
					      const char *name)
{
	struct pinctrl_state *state = pinctrl_lookup_state(pinctrl, name);

	if (IS_ERR(state))
		dev_err(dev, "missing pinctrl state %s: %ld\n",
			name, PTR_ERR(state));
	return state;
}

static int k50_lcm_bias_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct k50_lcm_bias *bias;

	bias = devm_kzalloc(dev, sizeof(*bias), GFP_KERNEL);
	if (!bias)
		return -ENOMEM;

	bias->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(bias->pinctrl))
		return PTR_ERR(bias->pinctrl);

	bias->enn_high = k50_lookup_state(dev, bias->pinctrl, "lcdbiasenn_h");
	bias->enn_low = k50_lookup_state(dev, bias->pinctrl, "lcdbiasenn_l");
	bias->enp_high = k50_lookup_state(dev, bias->pinctrl, "lcdbiasenp_h");
	bias->enp_low = k50_lookup_state(dev, bias->pinctrl, "lcdbiasenp_l");
	bias->enp2_high = k50_lookup_state(dev, bias->pinctrl, "lcdbiasenp2_h");
	bias->enp2_low = k50_lookup_state(dev, bias->pinctrl, "lcdbiasenp2_l");
	if (IS_ERR(bias->enn_high))
		return PTR_ERR(bias->enn_high);
	if (IS_ERR(bias->enn_low))
		return PTR_ERR(bias->enn_low);
	if (IS_ERR(bias->enp_high))
		return PTR_ERR(bias->enp_high);
	if (IS_ERR(bias->enp_low))
		return PTR_ERR(bias->enp_low);
	if (IS_ERR(bias->enp2_high))
		return PTR_ERR(bias->enp2_high);
	if (IS_ERR(bias->enp2_low))
		return PTR_ERR(bias->enp2_low);

	mutex_lock(&k50_bias_lock);
	if (k50_bias) {
		mutex_unlock(&k50_bias_lock);
		return -EBUSY;
	}
	k50_bias = bias;
	mutex_unlock(&k50_bias_lock);
	platform_set_drvdata(pdev, bias);
	return 0;
}

static int k50_lcm_bias_remove(struct platform_device *pdev)
{
	struct k50_lcm_bias *bias = platform_get_drvdata(pdev);

	mutex_lock(&k50_bias_lock);
	if (k50_bias == bias)
		k50_bias = NULL;
	mutex_unlock(&k50_bias_lock);
	return 0;
}

static const struct of_device_id k50_lcm_bias_of_match[] = {
	{ .compatible = "mediatek,lcdbias_gpio_control" },
	{ }
};
MODULE_DEVICE_TABLE(of, k50_lcm_bias_of_match);

static struct platform_driver k50_lcm_bias_driver = {
	.probe = k50_lcm_bias_probe,
	.remove = k50_lcm_bias_remove,
	.driver = {
		.name = "lcm_bias_gpio",
		.of_match_table = k50_lcm_bias_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(k50_lcm_bias_driver);

MODULE_DESCRIPTION("k50sv1 LCD bias GPIO driver");
MODULE_LICENSE("GPL v2");
