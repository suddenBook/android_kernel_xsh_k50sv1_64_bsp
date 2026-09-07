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

#include <kpd.h>
#include <mt-plat/aee.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <mt-plat/upmu_common.h>
#ifdef CONFIG_MT_SND_SOC_6755
#include <mt_soc_afe_control.h>
#endif

#define KPD_DEBUG	KPD_YES

#define KPD_SAY		"kpd: "
#if KPD_DEBUG
#define kpd_print(fmt, arg...)	pr_err(KPD_SAY fmt, ##arg)
#define kpd_info(fmt, arg...)	pr_warn(KPD_SAY fmt, ##arg)
#else
#define kpd_print(fmt, arg...)	do {} while (0)
#define kpd_info(fmt, arg...)	do {} while (0)
#endif

#ifdef CONFIG_KPD_PWRKEY_USE_EINT
static u8 kpd_pwrkey_state = !KPD_PWRKEY_POLARITY;
#endif

static int kpd_show_hw_keycode = 1;
static int kpd_enable_lprst = 1;
static u16 kpd_keymap_state[KPD_NUM_MEMS] = {
	0xffff, 0xffff, 0xffff, 0xffff, 0x00ff
};

static bool kpd_sb_enable;

#ifdef CONFIG_MTK_SMARTBOOK_SUPPORT
static void sb_kpd_release_keys(struct input_dev *dev)
{
	int code;

	for (code = 0; code <= KEY_MAX; code++) {
		if (test_bit(code, dev->keybit)) {
			kpd_print("report release event for sb plug in! keycode:%d\n", code);
			input_report_key(dev, code, 0);
			input_sync(dev);
		}
	}
}

void sb_kpd_enable(void)
{
	kpd_sb_enable = true;
	kpd_print("sb_kpd_enable performed!\n");
	mt_reg_sync_writew(0x0, KP_EN);
	sb_kpd_release_keys(kpd_input_dev);
}

void sb_kpd_disable(void)
{
	kpd_sb_enable = false;
	kpd_print("sb_kpd_disable performed!\n");
	mt_reg_sync_writew(0x1, KP_EN);
}
#else
void sb_kpd_enable(void)
{
	kpd_print("sb_kpd_enable empty function for HAL!\n");
}

void sb_kpd_disable(void)
{
	kpd_print("sb_kpd_disable empty function for HAL!\n");
}
#endif

static void enable_kpd(int enable)
{
	if (enable == 1) {
		mt_reg_sync_writew((u16) (enable), KP_EN);
		kpd_print("KEYPAD is enabled\n");
	} else if (enable == 0) {
		mt_reg_sync_writew((u16) (enable), KP_EN);
		kpd_print("KEYPAD is disabled\n");
	}
}

void kpd_slide_qwerty_init(void)
{
#if KPD_HAS_SLIDE_QWERTY
	bool evdev_flag = false;
	bool power_op = false;
	struct input_handler *handler;
	struct input_handle *handle;

	handle = rcu_dereference(dev->grab);
	if (handle) {
		handler = handle->handler;
		if (strcmp(handler->name, "evdev") == 0)
			return -1;
	} else {
		list_for_each_entry_rcu(handle, &dev->h_list, d_node) {
			handler = handle->handler;
			if (strcmp(handler->name, "evdev") == 0) {
				evdev_flag = true;
				break;
			}
		}
		if (evdev_flag == false)
			return -1;
	}

	power_op = powerOn_slidePin_interface();
	if (!power_op)
		kpd_print(KPD_SAY "Qwerty slide pin interface power on fail\n");
	else
		kpd_print("Qwerty slide pin interface power on success\n");

	mt_eint_set_sens(KPD_SLIDE_EINT, KPD_SLIDE_SENSITIVE);
	mt_eint_set_hw_debounce(KPD_SLIDE_EINT, KPD_SLIDE_DEBOUNCE);
	mt_eint_registration(KPD_SLIDE_EINT, true, KPD_SLIDE_POLARITY,
			     kpd_slide_eint_handler, false);

	power_op = powerOff_slidePin_interface();
	if (!power_op)
		kpd_print(KPD_SAY "Qwerty slide pin interface power off fail\n");
	else
		kpd_print("Qwerty slide pin interface power off success\n");
#endif
}

void kpd_get_keymap_state(u16 state[])
{
	state[0] = *(volatile u16 *)KP_MEM1;
	state[1] = *(volatile u16 *)KP_MEM2;
	state[2] = *(volatile u16 *)KP_MEM3;
	state[3] = *(volatile u16 *)KP_MEM4;
	state[4] = *(volatile u16 *)KP_MEM5;
	kpd_print(KPD_SAY "register = %x %x %x %x %x\n", state[0], state[1], state[2], state[3], state[4]);

}

static void kpd_factory_mode_handler(void)
{
	int i, j;
	bool pressed;
	u16 new_state[KPD_NUM_MEMS], change, mask;
	u16 hw_keycode, linux_keycode;

	for (i = 0; i < KPD_NUM_MEMS - 1; i++)
		kpd_keymap_state[i] = 0xffff;
	if (!kpd_dts_data.kpd_use_extend_type)
		kpd_keymap_state[KPD_NUM_MEMS - 1] = 0x00ff;
	else
		kpd_keymap_state[KPD_NUM_MEMS - 1] = 0xffff;

	kpd_get_keymap_state(new_state);

	for (i = 0; i < KPD_NUM_MEMS; i++) {
		change = new_state[i] ^ kpd_keymap_state[i];
		if (!change)
			continue;

		for (j = 0; j < 16; j++) {
			mask = 1U << j;
			if (!(change & mask))
				continue;

			hw_keycode = (i << 4) + j;
			/* bit is 1: not pressed, 0: pressed */
			pressed = !(new_state[i] & mask);
			if (kpd_show_hw_keycode) {
				kpd_print(KPD_SAY "(%s) factory_mode HW keycode = %u\n",
					  pressed ? "pressed" : "released", hw_keycode);
			}
			BUG_ON(hw_keycode >= KPD_NUM_KEYS);
			linux_keycode = kpd_dts_data.kpd_hw_init_map[hw_keycode];
			if (unlikely(linux_keycode == 0)) {
				kpd_print("Linux keycode = 0\n");
				continue;
			}
			input_report_key(kpd_input_dev, linux_keycode, pressed);
			input_sync(kpd_input_dev);
			kpd_print("factory_mode report Linux keycode = %u\n", linux_keycode);
		}
	}

	memcpy(kpd_keymap_state, new_state, sizeof(new_state));
	kpd_print("save new keymap state\n");
}

/********************************************************************/
void kpd_auto_test_for_factorymode(void)
{
	kpd_print("Enter kpd_auto_test_for_factorymode!\n");

	mdelay(1000);

	kpd_factory_mode_handler();
	kpd_print("begin kpd_auto_test_for_factorymode!\n");
#ifdef CONFIG_MTK_PMIC_CHIP_MT6353
	if (pmic_get_register_value(PMIC_PWRKEY_DEB) == 1) {
#else
	if (pmic_get_register_value(MT6351_PMIC_PWRKEY_DEB) == 1) {
#endif
		kpd_print("power key release\n");
		/*kpd_pwrkey_pmic_handler(1);*/
		/*mdelay(time);*/
		/*kpd_pwrkey_pmic_handler(0);}*/
	} else {
		kpd_print("power key press\n");
		kpd_pwrkey_pmic_handler(1);
		/*mdelay(time);*/
		/*kpd_pwrkey_pmic_handler(0);*/
	}

#ifdef KPD_PMIC_RSTKEY_MAP
#ifdef CONFIG_MTK_PMIC_CHIP_MT6353
	if (pmic_get_register_value(PMIC_HOMEKEY_DEB) == 1) {
#else
	if (pmic_get_register_value(MT6351_PMIC_HOMEKEY_DEB) == 1) {
#endif
		/*kpd_print("home key release\n");*/
		/*kpd_pmic_rstkey_handler(1);*/
		/*mdelay(time);*/
		/*kpd_pmic_rstkey_handler(0);*/
	} else {
		kpd_print("home key press\n");
		kpd_pmic_rstkey_handler(1);
		/*mdelay(time);*/
		/*kpd_pmic_rstkey_handler(0);*/
	}
#endif
}

/********************************************************************/
void long_press_reboot_function_setting(void)
{
	if (kpd_enable_lprst && get_boot_mode() == NORMAL_BOOT) {
		kpd_info("Normal Boot long press reboot selection\n");
#ifdef CONFIG_KPD_PMIC_LPRST_TD
		kpd_info("Enable normal mode LPRST\n");
#ifdef CONFIG_ONEKEY_REBOOT_NORMAL_MODE
#ifdef CONFIG_MTK_PMIC_CHIP_MT6353
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_EN, 0x01);
		pmic_set_register_value(PMIC_RG_HOMEKEY_RST_EN, 0x00);
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_TD, CONFIG_KPD_PMIC_LPRST_TD);
#else
		pmic_set_register_value(MT6351_PMIC_RG_PWRKEY_RST_EN, 0x01);
		pmic_set_register_value(MT6351_PMIC_RG_HOMEKEY_RST_EN, 0x00);
		pmic_set_register_value(MT6351_PMIC_RG_PWRKEY_RST_TD, CONFIG_KPD_PMIC_LPRST_TD);
#endif
#endif

#ifdef CONFIG_TWOKEY_REBOOT_NORMAL_MODE
#ifdef CONFIG_MTK_PMIC_CHIP_MT6353
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_EN, 0x01);
		pmic_set_register_value(PMIC_RG_HOMEKEY_RST_EN, 0x01);
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_TD, CONFIG_KPD_PMIC_LPRST_TD);
#else
		pmic_set_register_value(MT6351_PMIC_RG_PWRKEY_RST_EN, 0x01);
		pmic_set_register_value(MT6351_PMIC_RG_HOMEKEY_RST_EN, 0x01);
		pmic_set_register_value(MT6351_PMIC_RG_PWRKEY_RST_TD, CONFIG_KPD_PMIC_LPRST_TD);
#endif
#endif
#else
		kpd_info("disable normal mode LPRST\n");
#ifdef CONFIG_MTK_PMIC_CHIP_MT6353
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_EN, 0x00);
		pmic_set_register_value(PMIC_RG_HOMEKEY_RST_EN, 0x00);
#else
		pmic_set_register_value(MT6351_PMIC_RG_PWRKEY_RST_EN, 0x00);
		pmic_set_register_value(MT6351_PMIC_RG_HOMEKEY_RST_EN, 0x00);
#endif

#endif
	} else {
		kpd_info("Other Boot Mode long press reboot selection\n");
#ifdef CONFIG_KPD_PMIC_LPRST_TD
		kpd_info("Enable other mode LPRST\n");
#ifdef CONFIG_ONEKEY_REBOOT_OTHER_MODE
#ifdef CONFIG_MTK_PMIC_CHIP_MT6353
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_EN, 0x01);
		pmic_set_register_value(PMIC_RG_HOMEKEY_RST_EN, 0x00);
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_TD, CONFIG_KPD_PMIC_LPRST_TD);
#else
		pmic_set_register_value(MT6351_PMIC_RG_PWRKEY_RST_EN, 0x01);
		pmic_set_register_value(MT6351_PMIC_RG_HOMEKEY_RST_EN, 0x00);
		pmic_set_register_value(MT6351_PMIC_RG_PWRKEY_RST_TD, CONFIG_KPD_PMIC_LPRST_TD);
#endif
#endif

#ifdef CONFIG_TWOKEY_REBOOT_OTHER_MODE
#ifdef CONFIG_MTK_PMIC_CHIP_MT6353
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_EN, 0x01);
		pmic_set_register_value(PMIC_RG_HOMEKEY_RST_EN, 0x01);
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_TD, CONFIG_KPD_PMIC_LPRST_TD);
#else
		pmic_set_register_value(MT6351_PMIC_RG_PWRKEY_RST_EN, 0x01);
		pmic_set_register_value(MT6351_PMIC_RG_HOMEKEY_RST_EN, 0x01);
		pmic_set_register_value(MT6351_PMIC_RG_PWRKEY_RST_TD, CONFIG_KPD_PMIC_LPRST_TD);
#endif
#endif
#else
		kpd_info("disable other mode LPRST\n");
#ifdef CONFIG_MTK_PMIC_CHIP_MT6353
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_EN, 0x00);
		pmic_set_register_value(PMIC_RG_HOMEKEY_RST_EN, 0x00);
#else
		pmic_set_register_value(MT6351_PMIC_RG_PWRKEY_RST_EN, 0x00);
		pmic_set_register_value(MT6351_PMIC_RG_HOMEKEY_RST_EN, 0x00);
#endif
#endif
	}
}

/* FM@suspend */
bool __attribute__ ((weak)) ConditionEnterSuspend(void)
{
	return true;
}

/********************************************************************/
void kpd_wakeup_src_setting(int enable)
{
	int is_fm_radio_playing = 0;

	/* If FM is playing, keep keypad as wakeup source */
	if (ConditionEnterSuspend() == true)
		is_fm_radio_playing = 0;
	else
		is_fm_radio_playing = 1;

	if (is_fm_radio_playing == 0) {
		if (enable == 1) {
			kpd_print("enable kpd work!\n");
			enable_kpd(1);
		} else {
			kpd_print("disable kpd work!\n");
			enable_kpd(0);
		}
	}
}

/********************************************************************/
void kpd_init_keymap(u16 keymap[])
{
	int i = 0;

	if (kpd_dts_data.kpd_use_extend_type)
		kpd_keymap_state[4] = 0xffff;
	for (i = 0; i < KPD_NUM_KEYS; i++) {
		keymap[i] = kpd_dts_data.kpd_hw_init_map[i];
		/*kpd_print(KPD_SAY "keymap[%d] = %d\n", i,keymap[i]);*/
	}
}

void kpd_init_keymap_state(u16 keymap_state[])
{
	int i = 0;

	for (i = 0; i < KPD_NUM_MEMS; i++)
		keymap_state[i] = kpd_keymap_state[i];
	kpd_info("init_keymap_state done: %x %x %x %x %x!\n", keymap_state[0], keymap_state[1], keymap_state[2],
		 keymap_state[3], keymap_state[4]);
}

/********************************************************************/

void kpd_set_debounce(u16 val)
{
	mt_reg_sync_writew((u16) (val & KPD_DEBOUNCE_MASK), KP_DEBOUNCE);
}

/********************************************************************/
/*
 * WI-080 diagnostics and recovery.  Legacy MTK GPIO API, exported by
 * drivers/misc/mediatek/gpio/mt_gpio_core.c; pinctrl-mt6755.c routes its own
 * writes through the same functions.  Values as in mt-plat/mt_gpio.h:
 * GPIO_DIR_IN = 0, GPIO_PULL_ENABLE = 1, GPIO_PULL_UP = 1.
 */
extern int mt_set_gpio_mode(unsigned long pin, unsigned long mode);
extern int mt_get_gpio_mode(unsigned long pin);
extern int mt_set_gpio_dir(unsigned long pin, unsigned long dir);
extern int mt_get_gpio_dir(unsigned long pin);
extern int mt_set_gpio_pull_enable(unsigned long pin, unsigned long enable);
extern int mt_get_gpio_pull_enable(unsigned long pin);
extern int mt_set_gpio_pull_select(unsigned long pin, unsigned long select);
extern int mt_get_gpio_pull_select(unsigned long pin);
extern int mt_get_gpio_in(unsigned long pin);
extern int mt_get_gpio_out(unsigned long pin);

#define KPD_PAD_DIR_IN		0
#define KPD_PAD_PULL_ENABLE	1
#define KPD_PAD_PULL_UP		1

static const unsigned long kpd_kcol_pins[] = { KPD_KCOL0_PIN, KPD_KCOL1_PIN };

static int kpd_pad_snapshot(char *buf, size_t len, const char *name, unsigned long pin)
{
	return scnprintf(buf, len,
			 "%s GPIO%lu: mode=%d dir=%d pullen=%d pullsel=%d din=%d dout=%d\n",
			 name, pin, mt_get_gpio_mode(pin), mt_get_gpio_dir(pin),
			 mt_get_gpio_pull_enable(pin), mt_get_gpio_pull_select(pin),
			 mt_get_gpio_in(pin), mt_get_gpio_out(pin));
}

/* Registers of the block plus the three pads it uses.  Read-only. */
int kpd_hw_snapshot(char *buf, size_t len)
{
	int n = 0;

	n += scnprintf(buf + n, len - n,
		       "KP_STA=%04x MEM=%04x %04x %04x %04x %04x DEBOUNCE=%04x SCAN_TIMING=%04x SEL=%04x EN=%04x\n",
		       *(volatile u16 *)KP_STA, *(volatile u16 *)KP_MEM1, *(volatile u16 *)KP_MEM2,
		       *(volatile u16 *)KP_MEM3, *(volatile u16 *)KP_MEM4, *(volatile u16 *)KP_MEM5,
		       *(volatile u16 *)KP_DEBOUNCE, *(volatile u16 *)KP_SCAN_TIMING,
		       *(volatile u16 *)KP_SEL, *(volatile u16 *)KP_EN);
	n += kpd_pad_snapshot(buf + n, len - n, "KROW0", KPD_KROW0_PIN);
	n += kpd_pad_snapshot(buf + n, len - n, "KCOL0", KPD_KCOL0_PIN);
	n += kpd_pad_snapshot(buf + n, len - n, "KCOL1", KPD_KCOL1_PIN);
	return n;
}

/* True when both column pads are still function-mode inputs with a pull-up. */
bool kpd_kcol_pads_sane(void)
{
	bool ok = true;
	int i;

	for (i = 0; i < ARRAY_SIZE(kpd_kcol_pins); i++) {
		unsigned long pin = kpd_kcol_pins[i];

		if (mt_get_gpio_mode(pin) != KPD_PAD_FUNC_MODE ||
		    mt_get_gpio_dir(pin) != KPD_PAD_DIR_IN ||
		    mt_get_gpio_pull_enable(pin) != KPD_PAD_PULL_ENABLE ||
		    mt_get_gpio_pull_select(pin) != KPD_PAD_PULL_UP) {
			kpd_print("column pad GPIO%lu disturbed: mode=%d dir=%d pullen=%d pullsel=%d\n",
				  pin, mt_get_gpio_mode(pin), mt_get_gpio_dir(pin),
				  mt_get_gpio_pull_enable(pin), mt_get_gpio_pull_select(pin));
			ok = false;
		}
	}
	return ok;
}

void kpd_kcol_pads_reinit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(kpd_kcol_pins); i++) {
		unsigned long pin = kpd_kcol_pins[i];

		mt_set_gpio_mode(pin, KPD_PAD_FUNC_MODE);
		mt_set_gpio_dir(pin, KPD_PAD_DIR_IN);
		mt_set_gpio_pull_select(pin, KPD_PAD_PULL_UP);
		mt_set_gpio_pull_enable(pin, KPD_PAD_PULL_ENABLE);
	}
}

/* Disable and re-enable the block; it re-scans and raises an IRQ for any change. */
void kpd_hw_restart(u16 debounce)
{
	mt_reg_sync_writew(0, KP_EN);
	udelay(100);
	kpd_set_debounce(debounce);
	mt_reg_sync_writew(1, KP_EN);
}

/********************************************************************/
void kpd_pmic_rstkey_hal(unsigned long pressed)
{
	if (kpd_dts_data.kpd_sw_rstkey != 0) {
		if (!kpd_sb_enable) {
			input_report_key(kpd_input_dev, kpd_dts_data.kpd_sw_rstkey, pressed);
			input_sync(kpd_input_dev);
			if (kpd_show_hw_keycode) {
				kpd_print(KPD_SAY "(%s) HW keycode =%d using PMIC\n",
					  pressed ? "pressed" : "released", kpd_dts_data.kpd_sw_rstkey);
			}
		}
	}
}

void kpd_pmic_pwrkey_hal(unsigned long pressed)
{
#ifdef CONFIG_KPD_PWRKEY_USE_PMIC
	if (!kpd_sb_enable) {
		input_report_key(kpd_input_dev, kpd_dts_data.kpd_sw_pwrkey, pressed);
		input_sync(kpd_input_dev);
		if (kpd_show_hw_keycode) {
			kpd_print(KPD_SAY "(%s) HW keycode =%d using PMIC\n",
			       pressed ? "pressed" : "released", kpd_dts_data.kpd_sw_pwrkey);
		}
		/*aee_powerkey_notify_press(pressed);*/
	}
#endif
}

/***********************************************************************/
void kpd_pwrkey_handler_hal(unsigned long data)
{
#ifdef CONFIG_KPD_PWRKEY_USE_EINT
	bool pressed;
	u8 old_state = kpd_pwrkey_state;

	kpd_pwrkey_state = !kpd_pwrkey_state;
	pressed = (kpd_pwrkey_state == !!KPD_PWRKEY_POLARITY);
	if (kpd_show_hw_keycode)
		kpd_print(KPD_SAY "(%s) HW keycode = using EINT\n", pressed ? "pressed" : "released");
	input_report_key(kpd_input_dev, kpd_dts_data.kpd_sw_pwrkey, pressed);
	kpd_print("report Linux keycode = %u\n", kpd_dts_data.kpd_sw_pwrkey);
	input_sync(kpd_input_dev);

	/* for detecting the return to old_state */
	mt_eint_set_polarity(KPD_PWRKEY_EINT, old_state);
	mt_eint_unmask(KPD_PWRKEY_EINT);
#endif
}

#ifdef CONFIG_MTK_MRDUMP_KEY
static int mrdump_eint_state;
static int mrdump_ext_rst_irq;
static irqreturn_t mrdump_rst_eint_handler(int irq, void *data)
{
	/* bool pressed; */

	if (mrdump_eint_state == 0) {
		irq_set_irq_type(mrdump_ext_rst_irq, IRQ_TYPE_LEVEL_HIGH);
		mrdump_eint_state = 1;
	} else {
		irq_set_irq_type(mrdump_ext_rst_irq, IRQ_TYPE_LEVEL_LOW);
		mrdump_eint_state = 0;
	}

	input_report_key(kpd_input_dev, KEY_RESTART, mrdump_eint_state);
	input_sync(kpd_input_dev);

	return IRQ_HANDLED;
}
#endif

/***********************************************************************/
void mt_eint_register(void)
{
#ifdef CONFIG_MTK_MRDUMP_KEY
	int ints[2] = {0, 0};
	int ret;
	struct device_node *node;

	/* register EINT handler for MRDUMP_EXT_RST key */
	node = of_find_compatible_node(NULL, NULL, "mediatek, mrdump_ext_rst-eint");
	if (!node)
		kpd_print("can't find compatible node\n");
	else {
		of_property_read_u32_array(node, "debounce", ints, ARRAY_SIZE(ints));
		gpio_set_debounce(ints[0], ints[1]);

		mrdump_ext_rst_irq = irq_of_parse_and_map(node, 0);
		ret = request_irq(mrdump_ext_rst_irq, mrdump_rst_eint_handler,
				  IRQF_TRIGGER_NONE, "mrdump_ext_rst-eint", NULL);
		if (ret > 0)
			kpd_print("EINT IRQ LINE NOT AVAILABLE\n");
	}
#endif

#ifdef CONFIG_KPD_PWRKEY_USE_EINT
	mt_eint_set_sens(KPD_PWRKEY_EINT, KPD_PWRKEY_SENSITIVE);
	mt_eint_set_hw_debounce(KPD_PWRKEY_EINT, KPD_PWRKEY_DEBOUNCE);
	mt_eint_registration(KPD_PWRKEY_EINT, true, KPD_PWRKEY_POLARITY,
			     kpd_pwrkey_eint_handler, false);
#endif
}

/************************************************************************/
