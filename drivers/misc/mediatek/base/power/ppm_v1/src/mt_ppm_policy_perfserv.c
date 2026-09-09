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


#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/workqueue.h>

#include "mt_ppm_internal.h"


static enum ppm_power_state ppm_perfserv_get_power_state_cb(enum ppm_power_state cur_state);
static void ppm_perfserv_update_limit_cb(enum ppm_power_state new_state);
static void ppm_perfserv_status_change_cb(bool enable);
static void ppm_perfserv_mode_change_cb(enum ppm_mode mode);

/* other members will init by ppm_main */
static struct ppm_policy_data perfserv_policy = {
	.name			= __stringify(PPM_POLICY_PERF_SERV),
	.lock			= __MUTEX_INITIALIZER(perfserv_policy.lock),
	.policy			= PPM_POLICY_PERF_SERV,
	.priority		= PPM_POLICY_PRIO_PERFORMANCE_BASE,
	.get_power_state_cb	= ppm_perfserv_get_power_state_cb,
	.update_limit_cb	= ppm_perfserv_update_limit_cb,
	.status_change_cb	= ppm_perfserv_status_change_cb,
	.mode_change_cb		= ppm_perfserv_mode_change_cb,
};

struct ppm_perfserv_data {
	int min_freq;
	int min_core_num;
	int max_available_freq;
} perfserv_data = {
	.min_freq = -1,
	.min_core_num = -1,
	.max_available_freq = -1,
};

#ifdef CONFIG_MTK_PPM_LCMON_BOOST
/*
 * LCM-on performance pin (k50sv1_64_bsp board policy).
 *
 * The owner's requirement for this handset is the two extremes: panel lit ->
 * every core of both clusters online at its top OPP; panel dark -> one little
 * core during normal MT6755 operation. mt_ppm_main.c also enforces these core
 * limits and the screen-on maximum clocks after ordinary policy arbitration.
 * Power budgets, the final 5A limit, PTPOD calibration, fixed chip segments and
 * the dedicated suspend path retain their existing requirements. The
 * screen-on request is implemented as a second requester of
 * this policy's perf_idx: while the LCM is on, PERF_SERV asks for
 * lcmon.perf_idx (default: the platform maximum, power_tbl[0].perf_idx, which
 * is what /proc/ppm/policy/perfserv_max_perf_idx reports -- 5616 on the
 * MT6755 FY table) and the existing perf_idx path does the rest exactly as it
 * does for a userspace write: ppm_perfserv_get_power_state_cb() moves HICA to
 * the all-cores state through ppm_hica_get_state_by_perf_idx(), and
 * ppm_perfserv_update_limit_cb() turns the power-table row into min = max
 * cluster limits.  The userspace knob keeps working: the effective request is
 * the larger of the two, so a daemon writing perfserv_perf_idx can only raise
 * the pin, never fight it, and there is nothing to oscillate.
 *
 * The screen edge is FB_EVENT_BLANK, delivered by the LCM_OFF policy's FB
 * notifier (mt_ppm_policy_lcm_off.c), which calls ppm_perfserv_lcmon_switch()
 * under the same lock as its own state change, so a single mt_ppm_main() pass
 * applies the pin release and the LCM_OFF core limit together. The pin has to be
 * dropped by its owner: a policy applied later in ppm_main_update_limit() can
 * only intersect an overlapping range (MAX of the minima), so LCM_OFF could
 * never lower a minimum this policy raised.  The panel is already lit when
 * the kernel starts (the bootloader turned it on and the FB driver takes it
 * over without blanking), so lcm_on starts true and the first
 * FB_BLANK_POWERDOWN releases it.
 *
 * The re-evaluation is deferred to a work item rather than run in the FB
 * notifier.  fb_blank() is reached from the FBIOBLANK ioctl with
 * console_lock() held (fbmem.c), and mt_ppm_main() ends in the HPS client
 * callback, which runs the hotplug algorithm synchronously (cpu_up() and
 * cpu_down() in hps_algo_amp()).  Plugging CPUs under console_lock is what
 * MediaTek had to turn printk's console_cpu_notify() into a trylock for; the
 * other PPM users of this notifier only ever lower requests, so none of them
 * plugs a CPU from here.  Deferring keeps the hotplug and DVFS work, and its
 * printk output, out of the composer's blank/unblank ioctl.
 *
 * Tunables, root-only, under /proc/ppm/policy/:
 *   perfserv_lcmon_boost     1/0, default 1
 *   perfserv_lcmon_perf_idx  0 = platform maximum (default), else the perf
 *                            index to pin while the LCM is on
 */
struct ppm_perfserv_lcmon {
	bool enabled;
	bool lcm_on;
	unsigned int perf_idx;	/* 0 = platform maximum */
};

static struct ppm_perfserv_lcmon lcmon = {
	.enabled = true,
	.lcm_on = true,
	.perf_idx = 0,
};

/* what userspace last wrote to perfserv_perf_idx; req.perf_idx holds the effective value */
static unsigned int perfserv_user_perf_idx;

static void ppm_perfserv_lcmon_work_fn(struct work_struct *work)
{
	ppm_task_wakeup();
}
static DECLARE_WORK(lcmon_work, ppm_perfserv_lcmon_work_fn);

static unsigned int ppm_perfserv_lcmon_max_perf_idx(void)
{
	return ppm_get_power_table().power_tbl[0].perf_idx;
}

/* MUST in lock */
static unsigned int ppm_perfserv_lcmon_req_perf_idx(void)
{
	if (!lcmon.enabled || !lcmon.lcm_on)
		return 0;

	return lcmon.perf_idx ? lcmon.perf_idx : ppm_perfserv_lcmon_max_perf_idx();
}
#endif

/* MUST in lock */
bool ppm_perfserv_is_policy_active(void)
{
	if (!perfserv_policy.req.perf_idx
		&& perfserv_data.min_freq == -1
		&& perfserv_data.min_core_num == -1
		&& perfserv_data.max_available_freq == -1)
		return false;
	else
		return true;
}

#ifdef CONFIG_MTK_PPM_LCMON_BOOST
/* MUST in lock: recompute the effective request from the user and the LCM-on requester */
static void ppm_perfserv_lcmon_update_req(void)
{
	unsigned int lcmon_perf_idx;

	if (!perfserv_policy.is_enabled)
		return;

	lcmon_perf_idx = ppm_perfserv_lcmon_req_perf_idx();
	perfserv_policy.req.perf_idx = MAX(perfserv_user_perf_idx, lcmon_perf_idx);
	perfserv_policy.is_activated = ppm_perfserv_is_policy_active();
}

bool ppm_perfserv_lcmon_is_max_boost_active(void)
{
	bool active;

	ppm_lock(&perfserv_policy.lock);
	/* Lower debug indices keep the original performance-request semantics. */
	active = perfserv_policy.is_enabled && lcmon.enabled && lcmon.lcm_on &&
		(!lcmon.perf_idx || lcmon.perf_idx == ppm_perfserv_lcmon_max_perf_idx());
	ppm_unlock(&perfserv_policy.lock);

	return active;
}

void ppm_perfserv_lcmon_switch(bool lcm_on)
{
	unsigned int perf_idx;

	FUNC_ENTER(FUNC_LV_POLICY);

	ppm_lock(&perfserv_policy.lock);

	if (lcmon.lcm_on == lcm_on) {
		ppm_unlock(&perfserv_policy.lock);
		FUNC_EXIT(FUNC_LV_POLICY);
		return;
	}

	lcmon.lcm_on = lcm_on;
	ppm_perfserv_lcmon_update_req();
	perf_idx = perfserv_policy.req.perf_idx;

	ppm_unlock(&perfserv_policy.lock);

	/* one line per screen edge */
	ppm_info("@%s: lcm %s -> perf_idx = %d\n", __func__, lcm_on ? "on" : "off", perf_idx);

	/*
	 * Not ppm_task_wakeup(): see the console_lock note above.  Unbound
	 * rather than a per-CPU pool because the handler may plug CPUs.
	 */
	queue_work(system_unbound_wq, &lcmon_work);

	FUNC_EXIT(FUNC_LV_POLICY);
}
#endif

static enum ppm_power_state ppm_perfserv_get_power_state_cb(enum ppm_power_state cur_state)
{
	/* TODO: check min freq / max available freq */
	if (perfserv_policy.req.perf_idx)
		return ppm_hica_get_state_by_perf_idx(cur_state, perfserv_policy.req.perf_idx);
	else
		return cur_state;
}

static void ppm_perfserv_update_limit_cb(enum ppm_power_state new_state)
{
	int index, i;
	struct ppm_power_tbl_data power_table = ppm_get_power_table();

	FUNC_ENTER(FUNC_LV_POLICY);

	ppm_ver("@%s: perfserv policy update limit for new state = %s\n",
		__func__, ppm_get_power_state_name(new_state));

	/* TODO: add min freq/core check?? */
	if (perfserv_policy.req.perf_idx) {
		ppm_hica_set_default_limit_by_state(new_state, &perfserv_policy);

		/* get limit according to perf idx */
		index = ppm_get_table_idx_by_perf(new_state, perfserv_policy.req.perf_idx);
		if (index != -1) {
			for (i = 0; i < perfserv_policy.req.cluster_num; i++) {
				perfserv_policy.req.limit[i].min_cpu_core =
					power_table.power_tbl[index].cluster_cfg[i].core_num;
				perfserv_policy.req.limit[i].min_cpufreq_idx =
					power_table.power_tbl[index].cluster_cfg[i].opp_lv;

				/* error check */
				if (perfserv_policy.req.limit[i].min_cpufreq_idx == -1)
					perfserv_policy.req.limit[i].max_cpufreq_idx = -1;
			}
		} else {
			struct ppm_power_state_data *state_info;
#ifdef PPM_POWER_TABLE_CALIBRATION
			struct ppm_state_sorted_pwr_tbl_data *tbl;
#else
			const struct ppm_state_sorted_pwr_tbl_data *tbl;
#endif
			state_info = ppm_get_power_state_info();
			tbl = state_info[new_state].perf_sorted_tbl;

			if (perfserv_policy.req.perf_idx >= tbl->sorted_tbl[0].value) {
				/* set min = max */
				for (i = 0; i < perfserv_policy.req.cluster_num; i++) {
					perfserv_policy.req.limit[i].min_cpu_core =
						perfserv_policy.req.limit[i].max_cpu_core;
					perfserv_policy.req.limit[i].min_cpufreq_idx =
						perfserv_policy.req.limit[i].max_cpufreq_idx;
				}
			} else
				ppm_ver("@%s: no need to boost, use state default limit!\n", __func__);
		}
	}

	FUNC_EXIT(FUNC_LV_POLICY);
}

static void ppm_perfserv_status_change_cb(bool enable)
{
	FUNC_ENTER(FUNC_LV_POLICY);

	ppm_ver("@%s: perfserv policy status changed to %d\n", __func__, enable);

#ifdef CONFIG_MTK_PPM_LCMON_BOOST
	/* called with the policy lock held (mt_ppm_interface.c); re-arm the pin when re-enabled */
	if (enable)
		ppm_perfserv_lcmon_update_req();
#endif

	FUNC_EXIT(FUNC_LV_POLICY);
}

static void ppm_perfserv_mode_change_cb(enum ppm_mode mode)
{
	FUNC_ENTER(FUNC_LV_POLICY);

	ppm_ver("@%s: ppm mode changed to %d\n", __func__, mode);

	FUNC_EXIT(FUNC_LV_POLICY);
}

static int ppm_perfserv_perf_idx_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "perf idx = %d\n", perfserv_policy.req.perf_idx);
#ifdef CONFIG_MTK_PPM_LCMON_BOOST
	seq_printf(m, "user = %d, lcmon = %d (boost %s, lcm %s)\n",
		perfserv_user_perf_idx, ppm_perfserv_lcmon_req_perf_idx(),
		lcmon.enabled ? "enabled" : "disabled", lcmon.lcm_on ? "on" : "off");
#endif

	return 0;
}

static ssize_t ppm_perfserv_perf_idx_proc_write(struct file *file, const char __user *buffer,
					size_t count, loff_t *pos)
{
	unsigned int perf_idx;

	char *buf = ppm_copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;

	if (!kstrtouint(buf, 10, &perf_idx)) {
		ppm_info("@%s: get perf_idx = %d\n", __func__, perf_idx);

		ppm_lock(&perfserv_policy.lock);

		if (!perfserv_policy.is_enabled) {
			ppm_err("@%s: perfserv policy is not enabled!\n", __func__);
			ppm_unlock(&perfserv_policy.lock);
			goto out;
		}

#ifdef CONFIG_MTK_PPM_LCMON_BOOST
		perfserv_user_perf_idx = perf_idx;
		ppm_perfserv_lcmon_update_req();
#else
		perfserv_policy.req.perf_idx = perf_idx;
		perfserv_policy.is_activated = ppm_perfserv_is_policy_active();
#endif

		ppm_unlock(&perfserv_policy.lock);
		ppm_task_wakeup();
	} else
		ppm_err("@%s: Invalid input!\n", __func__);

out:
	free_page((unsigned long)buf);
	return count;
}

static int ppm_perfserv_min_perf_idx_proc_show(struct seq_file *m, void *v)
{
	struct ppm_power_tbl_data power_table = ppm_get_power_table();
	unsigned int size = power_table.nr_power_tbl;

	seq_printf(m, "%d\n", power_table.power_tbl[size - 1].perf_idx);

	return 0;
}

static int ppm_perfserv_max_perf_idx_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", ppm_get_power_table().power_tbl[0].perf_idx);

	return 0;
}

#ifdef CONFIG_MTK_PPM_LCMON_BOOST
static int ppm_perfserv_lcmon_boost_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", lcmon.enabled);

	return 0;
}

static ssize_t ppm_perfserv_lcmon_boost_proc_write(struct file *file, const char __user *buffer,
					size_t count, loff_t *pos)
{
	unsigned int enable;

	char *buf = ppm_copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;

	if (!kstrtouint(buf, 10, &enable)) {
		ppm_lock(&perfserv_policy.lock);
		lcmon.enabled = (enable) ? true : false;
		ppm_perfserv_lcmon_update_req();
		ppm_unlock(&perfserv_policy.lock);

		ppm_info("@%s: lcmon boost %s\n", __func__, lcmon.enabled ? "enabled" : "disabled");
		ppm_task_wakeup();
	} else
		ppm_err("echo [0/1] > /proc/ppm/policy/perfserv_lcmon_boost\n");

	free_page((unsigned long)buf);
	return count;
}

static int ppm_perfserv_lcmon_perf_idx_proc_show(struct seq_file *m, void *v)
{
	if (lcmon.perf_idx)
		seq_printf(m, "%d\n", lcmon.perf_idx);
	else
		seq_printf(m, "%d (platform max)\n", ppm_perfserv_lcmon_max_perf_idx());

	return 0;
}

static ssize_t ppm_perfserv_lcmon_perf_idx_proc_write(struct file *file, const char __user *buffer,
					size_t count, loff_t *pos)
{
	unsigned int perf_idx;

	char *buf = ppm_copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;

	if (kstrtouint(buf, 10, &perf_idx)) {
		ppm_err("echo <perf_idx, 0 = platform max> > /proc/ppm/policy/perfserv_lcmon_perf_idx\n");
		goto out;
	}

	if (perf_idx > ppm_perfserv_lcmon_max_perf_idx()) {
		ppm_err("@%s: perf_idx %d is above the power table maximum %d\n",
			__func__, perf_idx, ppm_perfserv_lcmon_max_perf_idx());
		goto out;
	}

	ppm_lock(&perfserv_policy.lock);
	lcmon.perf_idx = perf_idx;
	ppm_perfserv_lcmon_update_req();
	ppm_unlock(&perfserv_policy.lock);

	ppm_info("@%s: lcmon perf_idx = %d%s\n", __func__, perf_idx, perf_idx ? "" : " (platform max)");
	ppm_task_wakeup();

out:
	free_page((unsigned long)buf);
	return count;
}
#endif

PROC_FOPS_RW(perfserv_perf_idx);
PROC_FOPS_RO(perfserv_min_perf_idx);
PROC_FOPS_RO(perfserv_max_perf_idx);
#ifdef CONFIG_MTK_PPM_LCMON_BOOST
PROC_FOPS_RW(perfserv_lcmon_boost);
PROC_FOPS_RW(perfserv_lcmon_perf_idx);
#endif

static int __init ppm_perfserv_policy_init(void)
{
	int i, ret = 0;

	struct pentry {
		const char *name;
		const struct file_operations *fops;
	};

	const struct pentry entries[] = {
		PROC_ENTRY(perfserv_perf_idx),
		PROC_ENTRY(perfserv_min_perf_idx),
		PROC_ENTRY(perfserv_max_perf_idx),
#ifdef CONFIG_MTK_PPM_LCMON_BOOST
		PROC_ENTRY(perfserv_lcmon_boost),
		PROC_ENTRY(perfserv_lcmon_perf_idx),
#endif
	};

	FUNC_ENTER(FUNC_LV_POLICY);

	/* create procfs */
	for (i = 0; i < ARRAY_SIZE(entries); i++) {
		if (!proc_create(entries[i].name, S_IRUGO | S_IWUSR | S_IWGRP, policy_dir, entries[i].fops)) {
			ppm_err("%s(), create /proc/ppm/policy/%s failed\n", __func__, entries[i].name);
			ret = -EINVAL;
			goto out;
		}
	}

	if (ppm_main_register_policy(&perfserv_policy)) {
		ppm_err("@%s: perfserv policy register failed\n", __func__);
		ret = -EINVAL;
		goto out;
	}

#ifdef CONFIG_MTK_PPM_LCMON_BOOST
	/* the panel is lit at boot: start pinned, the first FB_BLANK_POWERDOWN releases it */
	ppm_lock(&perfserv_policy.lock);
	ppm_perfserv_lcmon_update_req();
	ppm_unlock(&perfserv_policy.lock);
	ppm_info("@%s: lcmon boost enabled, perf_idx = %d\n", __func__, perfserv_policy.req.perf_idx);
#endif

	ppm_info("@%s: register %s done!\n", __func__, perfserv_policy.name);

out:
	FUNC_EXIT(FUNC_LV_POLICY);

	return ret;
}

static void __exit ppm_perfserv_policy_exit(void)
{
	FUNC_ENTER(FUNC_LV_POLICY);

#ifdef CONFIG_MTK_PPM_LCMON_BOOST
	cancel_work_sync(&lcmon_work);
#endif
	ppm_main_unregister_policy(&perfserv_policy);

	FUNC_EXIT(FUNC_LV_POLICY);
}

module_init(ppm_perfserv_policy_init);
module_exit(ppm_perfserv_policy_exit);
