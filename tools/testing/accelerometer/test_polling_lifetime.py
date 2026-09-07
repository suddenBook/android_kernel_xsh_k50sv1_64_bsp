#!/usr/bin/env python3
"""Exercise actual accelerometer start/stop functions at controlled race points.

Pthreads model the mutual exclusion and completion contracts of the kernel
spinlock, hrtimer callback, and workqueue. No kernel or device is required.
--revision reproduces the same assertions using earlier production bodies.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile

from test_accelerometer import BASE, HEADERS, function, macro, run_fixture


CASES = (
    "before_gate", "inside_arm", "inflight_read", "inflight_error", "callback",
    "first_start", "nodata_off", "nodata_keep", "nodata_only", "power_error", "cycles",
)


BOUNDARIES = r"""
#include <stddef.h>
#include <sys/types.h>
#include <pthread.h>
#include <time.h>
#define ACC_LOG(...) do {} while (0)
#define ACC_PR_ERR(...) do {} while (0)
#define ACC_INVALID_VALUE -1
#define HRTIMER_MODE_ABS 0
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define smp_mb() do {} while (0)
#define DEFINE_SPINLOCK(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; test_spin_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); test_spin_unlock(lock); } while (0)
#define mutex_lock(lock) pthread_mutex_lock(lock)
#define mutex_unlock(lock) pthread_mutex_unlock(lock)
typedef uint64_t u64;
typedef int64_t ktime_t;
enum hrtimer_restart { HRTIMER_NORESTART };
struct device { int unused; };
struct device_attribute { int unused; };
struct hrtimer { ktime_t deadline; };
struct work_struct { int unused; };
struct acc_data { int x, y, z, status; int64_t timestamp; };
struct acc_context {
    int power, enable, delay;
    long long delay_ns, latency_ns;
    bool is_active_data, is_active_nodata, is_polling_run, is_first_data_after_enable;
    struct {
        int (*enable_nodata)(int);
        int (*batch)(int, int64_t, int64_t);
        bool is_report_input_direct, is_support_batch;
    } acc_ctl;
    struct { int (*get_data)(int *, int *, int *, int *); } acc_data;
    struct acc_data drv_data;
    struct hrtimer hrTimer;
    struct work_struct report;
    void *accel_workqueue;
    pthread_mutex_t acc_op_mutex;
    ktime_t target_ktime;
};
static struct acc_context context;
static struct acc_context *acc_context_obj = &context;
static const int64_t now_ns = 2000000000;
enum test_mode {
    BEFORE_GATE, INSIDE_ARM, INFLIGHT_READ, INFLIGHT_ERROR, CALLBACK,
    FIRST_START, NODATA_OFF, NODATA_KEEP, NODATA_ONLY, POWER_ERROR, CYCLES
};
static enum test_mode mode;
enum thread_role { CONTROL, WORKER, STOPPER, TIMER_CALLBACK };
static _Thread_local enum thread_role role;
static _Thread_local int poll_lock_depth;
static pthread_t worker_thread;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_changed = PTHREAD_COND_INITIALIZER;
static bool work_pending, work_running, worker_exit, timer_armed, callback_running;
static bool worker_paused, release_worker, callback_paused, release_callback;
static bool cancel_entered, cancel_completed, stop_lock_attempt, disable_returned;
static bool hardware_powered;
static int sensor_reads, reports, reads_after_disable, automatic_powerups;
static int power_off_error, power_off_calls;
static ssize_t stop_result;
static void acc_work_func(struct work_struct *work);
enum hrtimer_restart acc_poll(struct hrtimer *timer);

/* All fixture scheduler state is protected by state_lock. */
static void changed(void) { assert(!pthread_cond_broadcast(&state_changed)); }
static void wait_changed(void)
{
    struct timespec until;
    assert(!clock_gettime(CLOCK_REALTIME, &until));
    until.tv_sec += 4;
    assert(!pthread_cond_timedwait(&state_changed, &state_lock, &until));
}
static void wait_flag(bool *flag)
{
    assert(!pthread_mutex_lock(&state_lock));
    while (!*flag) wait_changed();
    assert(!pthread_mutex_unlock(&state_lock));
}
static void wait_idle(void)
{
    assert(!pthread_mutex_lock(&state_lock));
    while (work_pending || work_running) wait_changed();
    assert(!pthread_mutex_unlock(&state_lock));
}
static void pause_worker(void)
{
    assert(!pthread_mutex_lock(&state_lock));
    if (!worker_paused) {
        worker_paused = true;
        changed();
        while (!release_worker) wait_changed();
    }
    assert(!pthread_mutex_unlock(&state_lock));
}
void test_spin_lock(pthread_mutex_t *lock)
{
    if (role == STOPPER) {
        assert(!pthread_mutex_lock(&state_lock));
        stop_lock_attempt = true;
        changed();
        assert(!pthread_mutex_unlock(&state_lock));
    }
    assert(!pthread_mutex_lock(lock));
    ++poll_lock_depth;
}
void test_spin_unlock(pthread_mutex_t *lock)
{
    assert(poll_lock_depth == 1);
    --poll_lock_depth;
    assert(!pthread_mutex_unlock(lock));
}
static int atomic_read(const int *value) { return *value; }
static void atomic_set(int *value, int data) { *value = data; }
static ktime_t ktime_get(void) { return now_ns; }
static int64_t ktime_to_ns(ktime_t when) { return when; }
static int64_t getCurNS(void) { return now_ns; }
static ktime_t ktime_add_ns(ktime_t when, int64_t delay)
{
    if (role == WORKER && mode == BEFORE_GATE) pause_worker();
    return when + delay;
}
static void hrtimer_start(struct hrtimer *timer, ktime_t when, int kind)
{
    assert(kind == HRTIMER_MODE_ABS);
    if (role == WORKER && mode == INSIDE_ARM) pause_worker();
    assert(!pthread_mutex_lock(&state_lock));
    if (mode == FIRST_START && role == CONTROL) {
        /* A timer may fire as soon as it is armed. */
        assert(context.is_polling_run && context.is_first_data_after_enable);
    }
    timer->deadline = when;
    timer_armed = true;
    changed();
    assert(!pthread_mutex_unlock(&state_lock));
}
static void hrtimer_cancel(struct hrtimer *timer)
{
    (void)timer;
    assert(poll_lock_depth == 0);
    assert(!pthread_mutex_lock(&state_lock));
    cancel_entered = true;
    changed();
    /* Match hrtimer_cancel: wait for the handler, then remove its timer. */
    while (callback_running) wait_changed();
    timer_armed = false;
    cancel_completed = true;
    changed();
    assert(!pthread_mutex_unlock(&state_lock));
}
static bool queue_work(void *queue, struct work_struct *work)
{
    (void)queue; (void)work;
    assert(!pthread_mutex_lock(&state_lock));
    if (mode == CALLBACK && role == TIMER_CALLBACK && !callback_paused) {
        callback_paused = true;
        changed();
        while (!release_callback) wait_changed();
    }
    bool queued = !work_pending;
    work_pending = true;
    changed();
    assert(!pthread_mutex_unlock(&state_lock));
    return queued;
}
static void cancel_work_sync(struct work_struct *work)
{
    (void)work;
    assert(poll_lock_depth == 0);
    assert(!pthread_mutex_lock(&state_lock));
    work_pending = false;
    while (work_running) {
        wait_changed();
        work_pending = false;
    }
    assert(!pthread_mutex_unlock(&state_lock));
}
static void *run_worker(void *unused)
{
    (void)unused;
    role = WORKER;
    assert(!pthread_mutex_lock(&state_lock));
    while (!worker_exit) {
        while (!worker_exit && !work_pending) wait_changed();
        if (worker_exit) break;
        work_pending = false;
        work_running = true;
        assert(!pthread_mutex_unlock(&state_lock));
        acc_work_func(&context.report);
        assert(!pthread_mutex_lock(&state_lock));
        work_running = false;
        changed();
    }
    assert(!pthread_mutex_unlock(&state_lock));
    return NULL;
}
static bool expire_timer(void)
{
    assert(!pthread_mutex_lock(&state_lock));
    bool armed = timer_armed;
    if (armed) {
        timer_armed = false;
        callback_running = true;
    }
    assert(!pthread_mutex_unlock(&state_lock));
    if (!armed) return false;
    enum thread_role previous = role;
    role = TIMER_CALLBACK;
    assert(acc_poll(&context.hrTimer) == HRTIMER_NORESTART);
    role = previous;
    assert(!pthread_mutex_lock(&state_lock));
    callback_running = false;
    changed();
    assert(!pthread_mutex_unlock(&state_lock));
    return true;
}
static void *run_callback(void *unused)
{ (void)unused; assert(expire_timer()); return NULL; }
static int enable_nodata(int enabled)
{
    assert(!pthread_mutex_lock(&state_lock));
    if (!enabled) {
        /* Power must remain on until every outstanding sensor read drains. */
        assert(!callback_running && !work_running && !work_pending);
        ++power_off_calls;
        if (power_off_error) {
            assert(!pthread_mutex_unlock(&state_lock));
            return power_off_error;
        }
    }
    hardware_powered = !!enabled;
    assert(!pthread_mutex_unlock(&state_lock));
    return 0;
}
static int batch(int flag, int64_t period, int64_t latency)
{ assert(flag == 0 && period >= 0 && latency >= 0); return 0; }
static int get_data(int *x, int *y, int *z, int *status)
{
    assert(!pthread_mutex_lock(&state_lock));
    ++sensor_reads;
    if (disable_returned) ++reads_after_disable;
    /* MIR3DA's read helper powers the sensor on if it finds it disabled. */
    if (!hardware_powered) { hardware_powered = true; ++automatic_powerups; }
    assert(!pthread_mutex_unlock(&state_lock));
    if (mode == INFLIGHT_READ || mode == INFLIGHT_ERROR) pause_worker();
    if (mode == INFLIGHT_ERROR) return -EIO;
    *x = 2; *y = 3; *z = 4; *status = 2;
    return 0;
}
static int acc_data_report(struct acc_data *data)
{
    assert(data->x == 2 && data->y == 3 && data->z == 4);
    assert(!pthread_mutex_lock(&state_lock));
    assert(++reports < 1000);
    assert(!pthread_mutex_unlock(&state_lock));
    return 0;
}
"""


TESTS = r"""
static int active(bool enabled)
{ return acc_store_active(NULL, NULL, enabled ? "1" : "0", 1); }
static int nodata(bool enabled)
{ return acc_store_enable_nodata(NULL, NULL, enabled ? "1" : "0", 1); }
static void start_data(void)
{
    const char *request = "0,0,1000000,0";
    assert(acc_store_batch(NULL, NULL, request, strlen(request)) == 0);
    assert(active(true) == 0);
    assert(context.power == 1 && context.is_polling_run && timer_armed);
}
static void *run_stop(void *unused)
{
    (void)unused;
    role = STOPPER;
    stop_result = active(false);
    assert(!pthread_mutex_lock(&state_lock));
    disable_returned = true;
    changed();
    assert(!pthread_mutex_unlock(&state_lock));
    return NULL;
}
static void assert_drained(bool powered)
{
    assert(!pthread_mutex_lock(&state_lock));
    assert(!timer_armed && !callback_running && !work_pending && !work_running);
    assert(!context.is_polling_run && hardware_powered == powered);
    assert(!pthread_mutex_unlock(&state_lock));
}
static void assert_no_late_read(void)
{
    bool late_timer = expire_timer();
    wait_idle();
    assert(!pthread_mutex_lock(&state_lock));
    fprintf(stderr, "disable result: late_timer=%d reads_after_return=%d automatic_powerups=%d\n",
            late_timer, reads_after_disable, automatic_powerups);
    assert(!late_timer && reads_after_disable == 0 && automatic_powerups == 0);
    assert(!pthread_mutex_unlock(&state_lock));
    assert_drained(false);
    assert(context.drv_data.x == ACC_INVALID_VALUE);
    assert(context.drv_data.y == ACC_INVALID_VALUE);
    assert(context.drv_data.z == ACC_INVALID_VALUE);
}
int main(int argc, char **argv)
{
    const char *names[] = {
        "before_gate", "inside_arm", "inflight_read", "inflight_error", "callback",
        "first_start", "nodata_off", "nodata_keep", "nodata_only", "power_error", "cycles"
    };
    assert(argc == 2);
    int selected;
    for (selected = 0; selected < (int)(sizeof(names) / sizeof(names[0])); ++selected)
        if (!strcmp(argv[1], names[selected])) break;
    assert(selected < (int)(sizeof(names) / sizeof(names[0])));
    mode = selected;
    context.delay = 200;
    context.delay_ns = context.latency_ns = -1;
    context.acc_ctl.enable_nodata = enable_nodata;
    context.acc_ctl.batch = batch;
    context.acc_data.get_data = get_data;
    assert(!pthread_mutex_init(&context.acc_op_mutex, NULL));
    assert(!pthread_create(&worker_thread, NULL, run_worker, NULL));

    if (mode <= INFLIGHT_ERROR) {
        start_data();
        assert(expire_timer());
        wait_flag(&worker_paused);
        pthread_t stopper;
        assert(!pthread_create(&stopper, NULL, run_stop, NULL));
        if (mode == INSIDE_ARM) {
            assert(!pthread_mutex_lock(&state_lock));
            while (!stop_lock_attempt && !cancel_completed) wait_changed();
            assert(!pthread_mutex_unlock(&state_lock));
        } else {
            wait_flag(&cancel_completed);
        }
        assert(!pthread_mutex_lock(&state_lock));
        release_worker = true;
        changed();
        assert(!pthread_mutex_unlock(&state_lock));
        assert(!pthread_join(stopper, NULL));
        assert(stop_result == 0);
        assert_no_late_read();
    } else if (mode == CALLBACK) {
        start_data();
        pthread_t callback, stopper;
        assert(!pthread_create(&callback, NULL, run_callback, NULL));
        wait_flag(&callback_paused);
        assert(!pthread_create(&stopper, NULL, run_stop, NULL));
        wait_flag(&cancel_entered);
        assert(!pthread_mutex_lock(&state_lock));
        release_callback = true;
        changed();
        assert(!pthread_mutex_unlock(&state_lock));
        assert(!pthread_join(callback, NULL));
        assert(!pthread_join(stopper, NULL));
        assert(stop_result == 0);
        assert_no_late_read();
    } else if (mode == FIRST_START) {
        start_data();
        assert(active(false) == 0);
        assert_drained(false);
    } else if (mode == NODATA_OFF) {
        start_data();
        assert(nodata(false) == 0);
        assert(active(false) == 0);
        disable_returned = true;
        assert_no_late_read();
    } else if (mode == NODATA_KEEP) {
        start_data();
        assert(nodata(true) == 0);
        assert(active(false) == 0);
        assert_drained(true);
        assert(context.power == 1 && context.enable == 1);
        assert(nodata(false) == 0);
        assert_drained(false);
        assert(power_off_calls == 1);
    } else if (mode == NODATA_ONLY) {
        assert(nodata(true) == 0);
        assert(active(false) == 0);
        assert_drained(true);
        assert(context.power == 1 && context.enable == 1);
        assert(nodata(false) == 0);
        assert_drained(false);
    } else if (mode == POWER_ERROR) {
        start_data();
        assert(expire_timer());
        wait_idle();
        power_off_error = -EIO;
        assert(active(false) != 0);
        assert_drained(true);
        assert(context.power == 1);
        power_off_error = 0;
        assert(active(false) == 0);
        assert_drained(false);
        assert(context.power == 0 && power_off_calls == 2);
    } else {
        assert(mode == CYCLES);
        for (int cycle = 0; cycle < 50; ++cycle) {
            start_data();
            assert(expire_timer());
            wait_idle();
            assert(sensor_reads == cycle + 1);
            assert(active(false) == 0);
            assert_drained(false);
            assert(active(false) == 0);
            assert(power_off_calls == cycle + 1);
        }
    }
    assert(!pthread_mutex_lock(&state_lock));
    worker_exit = true;
    changed();
    assert(!pthread_mutex_unlock(&state_lock));
    assert(!pthread_join(worker_thread, NULL));
    assert(!pthread_mutex_destroy(&context.acc_op_mutex));
    printf("PASS: polling lifetime %s\n", argv[1]);
}
"""


def fixture(accel, kernel_header, division_header):
    definitions = "\n".join(macro(kernel_header, name) for name in (
        "min_t", "max_t", "clamp_t", "clamp_val"))
    definitions += "\n" + macro(division_header, "do_div")
    lock = "static DEFINE_SPINLOCK(acc_poll_lock);\n" if "DEFINE_SPINLOCK(acc_poll_lock)" in accel else ""
    stop = "acc_stop_polling" if "static void acc_stop_polling(" in accel else "stopTimer"
    production = "\n".join(function(accel, name) for name in (
        "startTimer", stop, "acc_work_func", "acc_poll", "acc_enable_and_batch",
        "acc_store_enable_nodata", "acc_store_active", "acc_store_batch"))
    return HEADERS + definitions + BOUNDARIES + lock + production + TESTS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--revision", help="Read production bodies from this git revision")
    parser.add_argument("--case", choices=CASES, action="append", dest="cases")
    parser.add_argument("--keep-generated", type=Path)
    args = parser.parse_args()

    def read(relative):
        if args.revision:
            return subprocess.check_output(
                ["git", "show", f"{args.revision}:{relative}"], cwd=args.kernel, text=True)
        return (args.kernel / relative).read_text()

    code = fixture(read(BASE + "accel.c"), read("include/linux/kernel.h"),
                   read("include/asm-generic/div64.h"))
    with tempfile.TemporaryDirectory(prefix="accel-polling-host-") as temporary:
        directory = Path(temporary)
        if args.keep_generated:
            directory = args.keep_generated.resolve()
            directory.mkdir(parents=True, exist_ok=True)
        passed = run_fixture(directory, "polling_lifetime", code,
                             [[case] for case in args.cases or CASES],
                             extra_flags=("-pthread",), timeout=8)
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
