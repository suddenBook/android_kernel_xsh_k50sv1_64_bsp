/*
 * GPIO Hall switch for the stylus silo fitted to k50sv1_64_bsp.
 *
 * The shipped kernel exposed a much wider character/proc interface around
 * this one switch.  LineageOS needs only the proven GPIO state, wake pulse,
 * read-only status node and FT8057 C0 handoff.
 *
 * Copyright (C) 2026 The LineageOS Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/input/k50_hall.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>

#include "k50_gpiokey.h"

#define K50_HALL_INPUT_NAME	"HALL_DEV"
#define K50_HALL_CLASS_NAME	"hall_info"
#define K50_HALL_DEVICE_NAME	"hall_info_data"
#define K50_HALL_IRQ_NAME	"k50-gpio-hall"
#define K50_HALL_EINT_COMPAT	"mediatek,hall-dev-eint"

struct k50_hall_data {
	struct platform_device *pdev;
	struct input_dev *input;
	struct class *status_class;
	struct device *status_device;
	struct mutex handoff_lock;
	int irq;
	bool irq_wake_enabled;
};

static DEFINE_MUTEX(k50_hall_state_lock);
static int k50_hall_state;
static bool k50_hall_ready;

int get_hall_status(void)
{
	int state;

	mutex_lock(&k50_hall_state_lock);
	state = k50_hall_ready ? k50_hall_state : -ENODEV;
	mutex_unlock(&k50_hall_state_lock);

	return state;
}

static int k50_hall_read_gpio(void)
{
	int state = gpio_get_value(K50_HALL_GPIO);

	if (state < 0)
		return state;
	return !!state;
}

static int k50_hall_set_irq_type(struct k50_hall_data *hall, int state)
{
	return irq_set_irq_type(hall->irq,
				k50_hall_irq_type_for_state(state));
}

static void k50_hall_report_transition(struct k50_hall_data *hall, int state)
{
	int ret;

	ret = k50_ft8057_set_hall_state(!!state);
	if (ret && ret != -ENODEV && ret != -EAGAIN)
		dev_warn(&hall->pdev->dev,
			 "FT8057 Hall handoff failed: %d\n", ret);

	pm_wakeup_event(&hall->pdev->dev, K50_HALL_WAKE_TIMEOUT_MS);
	input_report_key(hall->input, KEY_WAKEUP, 1);
	input_report_key(hall->input, KEY_WAKEUP, 0);
	input_sync(hall->input);
}

static irqreturn_t k50_hall_irq_thread(int irq, void *data)
{
	struct k50_hall_data *hall = data;
	struct k50_hall_transition transition;
	int current_state;
	int sample;
	int ret;

	msleep(K50_HALL_DEBOUNCE_MS);
	sample = k50_hall_read_gpio();

	mutex_lock(&k50_hall_state_lock);
	current_state = k50_hall_state;
	ret = k50_hall_transition(current_state, sample, &transition);
	if (!ret)
		k50_hall_state = transition.state;
	mutex_unlock(&k50_hall_state_lock);
	if (ret) {
		dev_err_ratelimited(&hall->pdev->dev,
				    "invalid Hall GPIO sample: %d\n", ret);
		return IRQ_HANDLED;
	}

	ret = k50_hall_set_irq_type(hall, transition.state);
	if (ret)
		dev_warn_ratelimited(&hall->pdev->dev,
				     "cannot set Hall IRQ polarity: %d\n", ret);

	if (transition.changed) {
		mutex_lock(&hall->handoff_lock);
		k50_hall_report_transition(hall, transition.state);
		mutex_unlock(&hall->handoff_lock);
	}

	return IRQ_HANDLED;
}

static ssize_t hall_status_show(struct device *dev,
				struct device_attribute *attribute, char *buffer)
{
	int state = get_hall_status();

	if (state < 0)
		return state;
	return scnprintf(buffer, PAGE_SIZE, "%d\n", state);
}
static DEVICE_ATTR_RO(hall_status);

static int k50_hall_select_pinctrl(struct platform_device *pdev)
{
	struct pinctrl_state *state;
	struct pinctrl *pinctrl;
	int ret;

	pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(pinctrl)) {
		ret = PTR_ERR(pinctrl);
		if (ret == -ENODEV || ret == -ENOENT)
			return 0;
		return ret;
	}

	state = pinctrl_lookup_state(pinctrl, "state_eint_as_int");
	if (IS_ERR(state)) {
		ret = PTR_ERR(state);
		if (ret == -ENODEV || ret == -ENOENT) {
			dev_warn(&pdev->dev,
				 "Hall interrupt pinctrl state missing\n");
			return 0;
		}
		return ret;
	}

	ret = pinctrl_select_state(pinctrl, state);
	return ret;
}

static int k50_hall_resolve_irq(void)
{
	struct device_node *eint_node;
	int irq;

	irq = gpio_to_irq(K50_HALL_GPIO);
	if (irq > 0)
		return irq;

	eint_node = of_find_compatible_node(NULL, NULL, K50_HALL_EINT_COMPAT);
	if (!eint_node)
		return irq < 0 ? irq : -ENODEV;

	irq = irq_of_parse_and_map(eint_node, 0);
	of_node_put(eint_node);
	return irq > 0 ? irq : -EINVAL;
}

static int k50_hall_create_status_node(struct k50_hall_data *hall)
{
	int ret;

	hall->status_class = class_create(THIS_MODULE, K50_HALL_CLASS_NAME);
	if (IS_ERR(hall->status_class))
		return PTR_ERR(hall->status_class);

	hall->status_device = device_create(hall->status_class, NULL, 0, hall,
					    K50_HALL_DEVICE_NAME);
	if (IS_ERR(hall->status_device)) {
		ret = PTR_ERR(hall->status_device);
		hall->status_device = NULL;
		class_destroy(hall->status_class);
		hall->status_class = NULL;
		return ret;
	}

	ret = device_create_file(hall->status_device, &dev_attr_hall_status);
	if (ret) {
		device_unregister(hall->status_device);
		hall->status_device = NULL;
		class_destroy(hall->status_class);
		hall->status_class = NULL;
	}
	return ret;
}

static void k50_hall_destroy_status_node(struct k50_hall_data *hall)
{
	if (hall->status_device) {
		device_remove_file(hall->status_device, &dev_attr_hall_status);
		device_unregister(hall->status_device);
		hall->status_device = NULL;
	}
	if (hall->status_class) {
		class_destroy(hall->status_class);
		hall->status_class = NULL;
	}
}

static int k50_hall_probe(struct platform_device *pdev)
{
	struct k50_hall_data *hall;
	struct input_dev *input;
	int state;
	int ret;

	hall = devm_kzalloc(&pdev->dev, sizeof(*hall), GFP_KERNEL);
	if (!hall)
		return -ENOMEM;
	hall->pdev = pdev;
	mutex_init(&hall->handoff_lock);
	platform_set_drvdata(pdev, hall);

	mutex_lock(&k50_hall_state_lock);
	if (k50_hall_ready) {
		mutex_unlock(&k50_hall_state_lock);
		return -EBUSY;
	}
	mutex_unlock(&k50_hall_state_lock);

	ret = devm_gpio_request_one(&pdev->dev, K50_HALL_GPIO, GPIOF_IN,
				    K50_HALL_IRQ_NAME);
	if (ret)
		return ret;

	ret = k50_hall_select_pinctrl(pdev);
	if (ret)
		return ret;

	state = k50_hall_read_gpio();
	if (state < 0)
		return state;

	hall->irq = k50_hall_resolve_irq();
	if (hall->irq < 0)
		return hall->irq;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;
	hall->input = input;
	input->name = K50_HALL_INPUT_NAME;
	input->id.bustype = BUS_HOST;
	input->dev.parent = &pdev->dev;
	input_set_capability(input, EV_KEY, KEY_SLEEP);
	input_set_capability(input, EV_KEY, KEY_WAKEUP);

	ret = input_register_device(input);
	if (ret)
		goto fail_input;

	ret = k50_hall_create_status_node(hall);
	if (ret)
		goto fail_registered_input;

	mutex_lock(&k50_hall_state_lock);
	k50_hall_state = state;
	k50_hall_ready = true;
	mutex_unlock(&k50_hall_state_lock);

	ret = request_threaded_irq(hall->irq, NULL, k50_hall_irq_thread,
				   IRQF_ONESHOT |
				   k50_hall_irq_type_for_state(state),
				   K50_HALL_IRQ_NAME, hall);
	if (ret)
		goto fail_published_state;

	mutex_lock(&hall->handoff_lock);
	state = get_hall_status();
	if (state >= 0) {
		ret = k50_ft8057_set_hall_state(!!state);
		if (ret && ret != -ENODEV && ret != -EAGAIN)
			dev_warn(&pdev->dev,
				 "initial FT8057 Hall handoff failed: %d\n", ret);
	}
	mutex_unlock(&hall->handoff_lock);

	device_init_wakeup(&pdev->dev, true);
	ret = enable_irq_wake(hall->irq);
	if (!ret)
		hall->irq_wake_enabled = true;
	else
		dev_warn(&pdev->dev, "cannot enable Hall wake IRQ: %d\n", ret);

	dev_info(&pdev->dev, "GPIO%d Hall state=%d irq=%d debounce=%dms\n",
		 K50_HALL_GPIO, state, hall->irq, K50_HALL_DEBOUNCE_MS);
	return 0;

fail_published_state:
	mutex_lock(&k50_hall_state_lock);
	k50_hall_ready = false;
	mutex_unlock(&k50_hall_state_lock);
	k50_hall_destroy_status_node(hall);
fail_registered_input:
	input_unregister_device(input);
	hall->input = NULL;
	return ret;

fail_input:
	input_free_device(input);
	hall->input = NULL;
	return ret;
}

static int k50_hall_remove(struct platform_device *pdev)
{
	struct k50_hall_data *hall = platform_get_drvdata(pdev);

	if (!hall)
		return 0;

	if (hall->irq_wake_enabled)
		disable_irq_wake(hall->irq);
	device_init_wakeup(&pdev->dev, false);
	free_irq(hall->irq, hall);

	mutex_lock(&k50_hall_state_lock);
	k50_hall_ready = false;
	mutex_unlock(&k50_hall_state_lock);
	k50_ft8057_set_hall_state(false);

	k50_hall_destroy_status_node(hall);
	if (hall->input) {
		input_unregister_device(hall->input);
		hall->input = NULL;
	}
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static const struct of_device_id k50_hall_of_match[] = {
	{ .compatible = "mediatek,gpiokey" },
	{ }
};
MODULE_DEVICE_TABLE(of, k50_hall_of_match);

static struct platform_driver k50_hall_driver = {
	.probe = k50_hall_probe,
	.remove = k50_hall_remove,
	.driver = {
		.name = "k50-gpio-hall",
		.owner = THIS_MODULE,
		.of_match_table = k50_hall_of_match,
	},
};

module_platform_driver(k50_hall_driver);

MODULE_AUTHOR("The LineageOS Project");
MODULE_DESCRIPTION("k50sv1 GPIO7 Hall switch");
MODULE_LICENSE("GPL v2");
