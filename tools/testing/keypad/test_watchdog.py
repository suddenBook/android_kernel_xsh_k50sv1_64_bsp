#!/usr/bin/env python3
"""Run the production keypad IRQ/recovery paths with controlled time and scans.

Only kernel scheduling, input reporting and hardware boundaries are replaced.
Pass a scratch directory for the host C build; --revision checks older kpd.c.
"""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


SOURCE = "drivers/input/keyboard/mediatek/kpd.c"
CASES = (
    "other_key_taps", "new_key_during_recovery", "ordinary_release",
    "separate_timeouts", "release_repress", "repress_during_recovery",
    "missed_release", "manual", "manual_repress", "wrap",
    "resume_held", "resume_changed", "remove_pending", "forced_up",
)


def function(source, name):
    match = re.search(rf"(?m)^[\w *]+\b{name}\s*\([^;{{}}]*\)\s*\{{", source)
    if not match:
        raise ValueError(f"Function not found: {name}")
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


PREAMBLE = r"""
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
typedef uint16_t u16;
typedef int irqreturn_t;
typedef int pm_message_t;
#define IRQ_HANDLED 1
#define HZ 100
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, value) ((x) = (value))
#define unlikely(x) (x)
#define BUG_ON(x) assert(!(x))
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)
#define kpd_print(...) do {} while (0)
#define DEFINE_MUTEX(name) int name
struct input_dev { int unused; };
struct wake_lock { int unused; };
struct device_driver { int unused; };
struct platform_device { int unused; };
struct work_struct { void (*fn)(struct work_struct *); };
struct delayed_work {
    struct work_struct work;
    bool pending;
    unsigned long due;
};
struct tasklet_struct {
    void (*fn)(unsigned long);
    bool pending;
    unsigned disabled;
};
#define DECLARE_DELAYED_WORK(name, callback) \
    struct delayed_work name = { .work = { .fn = callback } }
#define DECLARE_TASKLET(name, callback, arg) \
    struct tasklet_struct name = { .fn = callback }
static unsigned long jiffies;
static struct input_dev input;
static struct input_dev *kpd_input_dev = &input;
static struct { unsigned kpd_key_debounce; } kpd_dts_data = { 1024 };
static unsigned kp_irqnr = 1;
static void *kp_base = &input, *kpd_clk = &input, *system_wq;
static struct { struct device_driver driver; } kpd_pdrv;
static int kpd_dev, aee_timer;
static unsigned irq_disabled, hardware_reads, restarts, pad_repairs;
static bool irq_pending, pads_sane = true;
static bool reported[256];
static unsigned presses[256], releases[256];
static void (*settle_action)(void);
static void pump_tasklet(void);
static void run_due_work(void);
static irqreturn_t kpd_irq_handler(int irq, void *dev_id);
static unsigned long msecs_to_jiffies(unsigned ms)
{ return ((unsigned long)ms * HZ + 999) / 1000; }
static void mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(int *lock) { assert(*lock == 1); *lock = 0; }
static bool mod_delayed_work(void *wq, struct delayed_work *work, unsigned long delay)
{
    (void)wq;
    bool pending = work->pending;
    work->pending = true;
    work->due = jiffies + delay;
    return pending;
}
static bool cancel_delayed_work(struct delayed_work *work)
{ bool pending = work->pending; work->pending = false; return pending; }
static void cancel_delayed_work_sync(struct delayed_work *work)
{ cancel_delayed_work(work); }
static void flush_delayed_work(struct delayed_work *work)
{
    if (work->pending) {
        work->pending = false;
        work->work.fn(&work->work);
    }
}
static void tasklet_schedule(struct tasklet_struct *tasklet) { tasklet->pending = true; }
static void tasklet_disable(struct tasklet_struct *tasklet) { ++tasklet->disabled; }
static void tasklet_enable(struct tasklet_struct *tasklet)
{ assert(tasklet->disabled); --tasklet->disabled; }
static void tasklet_kill(struct tasklet_struct *tasklet)
{ assert(!tasklet->disabled); tasklet->pending = false; }
static void disable_irq_nosync(unsigned irq) { (void)irq; ++irq_disabled; }
static void disable_irq(unsigned irq) { disable_irq_nosync(irq); }
static void enable_irq(unsigned irq)
{
    assert(irq_disabled);
    if (!--irq_disabled && irq_pending) {
        irq_pending = false;
        kpd_irq_handler(irq, NULL);
    }
}
static void input_report_key(struct input_dev *dev, unsigned key, int down)
{
    assert(dev && key < 256);
    if (reported[key] == !!down) return;
    reported[key] = !!down;
    if (down) ++presses[key]; else ++releases[key];
}
static void input_sync(struct input_dev *dev) { assert(dev); }
static void kpd_aee_handler(unsigned key, int down) { (void)key; (void)down; }
static void wake_lock_timeout(struct wake_lock *lock, unsigned delay)
{ (void)lock; (void)delay; }
static int kpd_hw_snapshot(char *buf, size_t len)
{ assert(len); buf[0] = 0; return 0; }
static bool kpd_kcol_pads_sane(void) { return pads_sane; }
static void kpd_kcol_pads_reinit(void) { ++pad_repairs; pads_sane = true; }
static void kpd_hw_restart(u16 debounce) { assert(debounce == 1024); ++restarts; }
static void msleep(unsigned ms)
{
    unsigned long ticks = msecs_to_jiffies(ms);
    jiffies += ticks / 2;
    if (settle_action) {
        void (*action)(void) = settle_action;
        settle_action = NULL;
        action();
    }
    jiffies += ticks - ticks / 2;
    pump_tasklet();
}
static void kpd_delete_attr(struct device_driver *driver) { (void)driver; }
static void misc_deregister(int *dev) { (void)dev; }
static void free_irq(unsigned irq, void *dev) { (void)irq; (void)dev; }
static void kpd_unregister_input(void) { kpd_input_dev = NULL; }
static void hrtimer_cancel(int *timer) { (void)timer; }
static void wake_lock_destroy(struct wake_lock *lock) { (void)lock; }
static void irq_dispose_mapping(unsigned irq) { (void)irq; }
static void iounmap(void *base) { (void)base; }
static void clk_disable_unprepare(void *clk) { (void)clk; }
"""


HARDWARE = r"""
static u16 hardware[KPD_NUM_MEMS];
static void kpd_get_keymap_state(u16 state[])
{
    assert(kp_base);
    ++hardware_reads;
    memcpy(state, hardware, sizeof(hardware));
}
"""


TESTS = r"""
static void pump_tasklet(void)
{
    unsigned runs = 0;
    while (kpd_keymap_tasklet.pending && !kpd_keymap_tasklet.disabled) {
        assert(++runs < 10);
        kpd_keymap_tasklet.pending = false;
        kpd_keymap_tasklet.fn(0);
    }
}
static void run_due_work(void)
{
    unsigned runs = 0;
    pump_tasklet();
    while (kpd_stuck_work.pending && time_after_eq(jiffies, kpd_stuck_work.due)) {
        assert(++runs < 10);
        kpd_stuck_work.pending = false;
        kpd_stuck_work.work.fn(&kpd_stuck_work.work);
        pump_tasklet();
    }
}
static void advance(unsigned ms)
{ jiffies += msecs_to_jiffies(ms); run_due_work(); }
static void key(unsigned hw, bool down)
{
    unsigned mem = hw >> 4;
    u16 mask = 1U << (hw & 15);
    assert(hw < KPD_NUM_KEYS);
    assert(!!(hardware[mem] & mask) == down);
    hardware[mem] ^= mask;
    if (irq_disabled) irq_pending = true;
    else kpd_irq_handler(kp_irqnr, NULL);
    pump_tasklet();
}
static void press_other_key(void) { key(0, true); }
static void release_repress(void) { key(1, false); key(1, true); }
static void release_without_irq(void) { hardware[0] |= 2; }
static void initialize(void)
{
    for (unsigned i = 0; i < KPD_NUM_MEMS; ++i)
        hardware[i] = kpd_keymap_state[i] = i == KPD_NUM_MEMS - 1 ? 0xff : 0xffff;
    /* The packaged k50 recovery DT maps these two matrix keys. */
    kpd_keymap[0] = 114;
    kpd_keymap[1] = 115;
    kpd_valid_mask[0] = 3;
    kpd_stopping = false;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    initialize();
    const char *name = argv[1];
    if (!strcmp(name, "other_key_taps") || !strcmp(name, "wrap")) {
        if (!strcmp(name, "wrap")) jiffies = ULONG_MAX - 1000;
        pads_sane = false;
        key(1, true);
        for (unsigned i = 0; i < 3; ++i) {
            advance(5000); key(0, true);
            advance(100); key(0, false);
        }
        advance(4700);
        assert(releases[115] == 1 && !reported[115]);
        assert(restarts == 1 && pad_repairs == 1 && !kpd_stuck_work.pending);
    } else if (!strcmp(name, "new_key_during_recovery")) {
        key(1, true);
        settle_action = press_other_key;
        advance(20000);
        assert(releases[115] == 1 && !reported[115]);
        assert(presses[114] == 1 && releases[114] == 0 && reported[114]);
        assert(kpd_stuck_work.pending);
    } else if (!strcmp(name, "ordinary_release")) {
        key(1, true);
        advance(100); key(1, false);
        advance(20000);
        assert(presses[115] == 1 && releases[115] == 1);
        assert(!restarts && !kpd_stuck_work.pending);
    } else if (!strcmp(name, "separate_timeouts")) {
        key(1, true);
        advance(10000); key(0, true);
        advance(10000);
        assert(!reported[115] && reported[114] && restarts == 1);
        assert(kpd_stuck_work.pending && kpd_stuck_work.due == msecs_to_jiffies(30000));
        jiffies = kpd_stuck_work.due;
        run_due_work();
        assert(!reported[114] && releases[114] == 1 && restarts == 2);
        assert(!kpd_stuck_work.pending);
    } else if (!strcmp(name, "release_repress")) {
        key(1, true);
        advance(19900); release_repress();
        advance(100);
        assert(reported[115] && presses[115] == 2 && releases[115] == 1 && !restarts);
        assert(kpd_stuck_work.due == msecs_to_jiffies(39900));
        advance(19900);
        assert(!reported[115] && releases[115] == 2 && restarts == 1);
    } else if (!strcmp(name, "repress_during_recovery")) {
        key(1, true);
        settle_action = release_repress;
        advance(20000);
        assert(reported[115] && presses[115] == 2 && releases[115] == 1 && restarts == 1);
        assert(kpd_stuck_work.pending && time_after(kpd_stuck_work.due, jiffies));
    } else if (!strcmp(name, "missed_release")) {
        key(1, true);
        settle_action = release_without_irq;
        advance(20000);
        assert(!reported[115] && releases[115] == 1 && !kpd_stuck_work.pending);
    } else if (!strcmp(name, "manual")) {
        key(1, true);
        settle_action = press_other_key;
        assert(kpd_store_recover(NULL, "1", 1) == 1);
        assert(!reported[115] && reported[114] && releases[114] == 0 && restarts == 1);
        key(0, false);
        assert(!kpd_stuck_work.pending);
        /* Retrying the still-low, software-released column must still reset hardware. */
        assert(kpd_store_recover(NULL, "1", 1) == 1);
        assert(restarts == 2 && presses[115] == 1 && releases[115] == 1);
    } else if (!strcmp(name, "manual_repress")) {
        key(1, true);
        settle_action = release_repress;
        assert(kpd_store_recover(NULL, "1", 1) == 1);
        assert(reported[115] && presses[115] == 2 && releases[115] == 1 && restarts == 1);
    } else if (!strcmp(name, "resume_held")) {
        key(1, true);
        advance(10000);
        assert(kpd_pdrv_suspend(NULL, 0) == 0);
        jiffies += msecs_to_jiffies(10000);
        assert(kpd_pdrv_resume(NULL) == 0);
        run_due_work();
        assert(!reported[115] && releases[115] == 1 && restarts == 1);
    } else if (!strcmp(name, "resume_changed")) {
        key(1, true);
        assert(kpd_pdrv_suspend(NULL, 0) == 0);
        hardware[0] |= 2;
        hardware[0] &= ~1U;
        jiffies += msecs_to_jiffies(25000);
        assert(kpd_pdrv_resume(NULL) == 0);
        run_due_work();
        assert(!reported[115] && reported[114] && !restarts);
        assert(kpd_stuck_work.pending && kpd_stuck_work.due == msecs_to_jiffies(45000));
    } else if (!strcmp(name, "remove_pending")) {
        key(1, true);
        assert(kpd_stuck_work.pending);
        assert(kpd_pdrv_remove(NULL) == 0);
        assert(!kpd_stuck_work.pending && !kpd_input_dev && !kp_base);
        unsigned reads = hardware_reads;
        kpd_stuck_work_func(&kpd_stuck_work.work);
        kpd_keymap_handler(0);
        assert(kpd_pdrv_resume(NULL) == 0);
        assert(kpd_store_recover(NULL, "1", 1) == -ENODEV);
        assert(hardware_reads == reads && !kpd_stuck_work.pending && !restarts);
    } else if (!strcmp(name, "forced_up")) {
        key(1, true);
        advance(20000);
        assert(!reported[115] && releases[115] == 1);
        key(0, true); key(0, false);
        assert(presses[115] == 1 && !kpd_stuck_work.pending);
        key(1, false);
        assert(releases[115] == 1);
        key(1, true);
        assert(presses[115] == 2 && reported[115]);
        advance(20000);
        assert(releases[115] == 2 && !reported[115] && restarts == 2);
    } else abort();
    assert(!kpd_mutex && !kpd_keymap_tasklet.disabled);
    printf("PASS %s\n", name);
}
"""


def fixture(source, header):
    constants = "\n".join(re.findall(r"(?m)^#define KPD_(?:NUM_MEMS|MEM5_BITS|NUM_KEYS)\b.*", header))
    start = source.index("static DEFINE_MUTEX(kpd_mutex)")
    end = source.index("#if (defined(CONFIG_ARCH_MT8173)", start)
    globals_ = source[start:end]
    start = source.index("static void kpd_keymap_handler(unsigned long data);")
    end = source.index(";", source.index("static DECLARE_TASKLET(kpd_keymap_tasklet,", start))
    tasklet = source[start:end + 1]
    start = source.index("static bool kpd_any_key_down(")
    last = function(source, "kpd_stuck_work_func")
    core = source[start:source.index(last) + len(last)]
    callbacks = "".join(function(source, name) for name in (
        "kpd_store_recover", "kpd_pdrv_suspend", "kpd_pdrv_resume", "kpd_pdrv_remove"))
    return "\n".join((PREAMBLE, constants, globals_, tasklet, HARDWARE, core, callbacks, TESTS))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--revision")
    parser.add_argument("--case", choices=CASES)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]

    def read(path):
        if args.revision:
            return subprocess.check_output(
                ["git", "show", f"{args.revision}:{path}"], cwd=root, text=True)
        return (root / path).read_text()

    source = fixture(read(SOURCE), read("drivers/input/keyboard/mediatek/mt6755/hal_kpd.h"))
    cases = (args.case,) if args.case else CASES
    args.scratch.mkdir(parents=True, exist_ok=True)
    failures = []
    with tempfile.TemporaryDirectory(prefix="keypad-", dir=args.scratch) as directory:
        unit = Path(directory) / "test.c"
        binary = Path(directory) / "test"
        unit.write_text(source)
        subprocess.run([
            "clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-variable",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            str(unit), "-o", str(binary),
        ], check=True)
        for case in cases:
            result = subprocess.run([str(binary), case], text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
            if result.returncode:
                failures.append(case)
                print(f"FAIL {case}: {result.stdout.strip()}")
            else:
                print(result.stdout, end="")
    print(f"{len(cases) - len(failures)}/{len(cases)} passed; failures={failures}")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
