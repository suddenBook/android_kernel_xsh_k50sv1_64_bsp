#!/usr/bin/env python3
"""Check real BT open/close functions with deterministic pthread interleavings."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


SOURCE = Path(__file__).resolve().parents[2]

STUBS = r'''
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MTK_WCN_BOOL_FALSE 0
#define MTK_WCN_BOOL_TRUE 1
#define WMTDRV_TYPE_BT 0
#define BT_TASK_INDX 0
#define ON 0xff
#define OFF 0
#define BT_LOG_PRT_ERR(...) ((void)0)
#define BT_LOG_PRT_WARN(...) ((void)0)
#define BT_LOG_PRT_INFO(...) ((void)0)
#define BT_LOG_PRT_DBG(...) ((void)0)
#define DEFINE_MUTEX(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define mutex_lock test_mutex_lock
#define mutex_unlock pthread_mutex_unlock

struct inode { int unused; };
struct file { int unused; };
/* Atomic backing preserves the old check/set race without a host C data race. */
static atomic_bool btonflag;
static atomic_int rstflag, bt_ftrace_flag;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local int thread_id;
static enum power_gate { NO_GATE, GATE_ON, GATE_OFF } gate;
static int inside_gate, release_gate, second_progress, overlapping_power;
static int ready, fail_on, fail_off, fail_first_on;
static int power_on, on_calls, off_calls, logger_state;
static void (*reset_callback)(void), (*event_callback)(void);

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void test_mutex_lock(pthread_mutex_t *lock)
{
    pthread_mutex_lock(&state_lock);
    if (thread_id == 2) {
        second_progress = 1;
        pthread_cond_broadcast(&changed);
    }
    pthread_mutex_unlock(&state_lock);
    pthread_mutex_lock(lock);
}

static void block_first_power_call(enum power_gate selected_gate)
{
    if (gate == selected_gate && thread_id == 1) {
        inside_gate = 1;
        pthread_cond_broadcast(&changed);
        while (!release_gate)
            pthread_cond_wait(&changed, &state_lock);
        inside_gate = 0;
    }
}

static int mtk_wcn_wmt_func_on(int type)
{
    int result;

    pthread_mutex_lock(&state_lock);
    check(type == WMTDRV_TYPE_BT, "wrong function enabled");
    on_calls++;
    if (thread_id == 2) {
        /* Baseline code has no mutex: its second open reaches WMT directly. */
        second_progress = 1;
        if (inside_gate)
            overlapping_power++;
        pthread_cond_broadcast(&changed);
    }
    block_first_power_call(GATE_ON);
    result = !(fail_on || (fail_first_on && thread_id == 1));
    if (result)
        power_on = 1;
    pthread_mutex_unlock(&state_lock);
    return result;
}

static int mtk_wcn_wmt_func_off(int type)
{
    int result;

    pthread_mutex_lock(&state_lock);
    check(type == WMTDRV_TYPE_BT && power_on, "function-off without a powered function");
    off_calls++;
    block_first_power_call(GATE_OFF);
    result = !fail_off;
    if (result)
        power_on = 0;
    pthread_mutex_unlock(&state_lock);
    return result;
}

static int mtk_wcn_stp_is_ready(void) { return ready; }
static void mtk_wcn_stp_set_bluez(int mode) {}
static void BT_event_cb(void) {}
static void bt_cdev_rst_cb(void) {}

static int mtk_wcn_stp_register_event_cb(int type, void (*callback)(void))
{
    pthread_mutex_lock(&state_lock);
    event_callback = callback;
    pthread_mutex_unlock(&state_lock);
    return 0;
}

static int mtk_wcn_wmt_msgcb_reg(int type, void (*callback)(void))
{
    pthread_mutex_lock(&state_lock);
    reset_callback = callback;
    pthread_mutex_unlock(&state_lock);
    return MTK_WCN_BOOL_TRUE;
}

static int mtk_wcn_wmt_msgcb_unreg(int type)
{
    return mtk_wcn_wmt_msgcb_reg(type, NULL);
}

static void bt_state_notify(int state)
{
    pthread_mutex_lock(&state_lock);
    logger_state = state;
    pthread_mutex_unlock(&state_lock);
}
'''

TESTS = r'''
struct call { int id, close, result; };

static void *call_driver(void *opaque)
{
    struct call *call = opaque;

    thread_id = call->id;
    call->result = call->close ? BT_close(NULL, NULL) : BT_open(NULL, NULL);
    return NULL;
}

static void reset_test(void)
{
    thread_id = 0;
    btonflag = rstflag = bt_ftrace_flag = 0;
    gate = NO_GATE;
    inside_gate = release_gate = second_progress = overlapping_power = 0;
    fail_on = fail_off = fail_first_on = 0;
    power_on = on_calls = off_calls = logger_state = 0;
    reset_callback = event_callback = NULL;
    ready = 1;
}

static void check_open(void)
{
    check(btonflag && bt_ftrace_flag && power_on && !rstflag, "open state is inconsistent");
    check(event_callback == BT_event_cb && reset_callback == bt_cdev_rst_cb,
          "open callbacks are inconsistent");
#ifdef CONFIG_MTK_CONNSYS_DEDICATED_LOG_PATH
    check(logger_state == ON, "open logger state is inconsistent");
#endif
}

static void check_closed(int power_remains_on)
{
    check(!btonflag && !bt_ftrace_flag && power_on == power_remains_on,
          "closed state is inconsistent");
    check(!event_callback && !reset_callback, "closed driver retained callbacks");
#ifdef CONFIG_MTK_CONNSYS_DEDICATED_LOG_PATH
    check(logger_state == OFF, "closed logger state is inconsistent");
#endif
}

static void interleave(struct call *first, struct call *second)
{
    pthread_t threads[2];

    check(!pthread_create(&threads[0], NULL, call_driver, first), "first thread creation failed");
    pthread_mutex_lock(&state_lock);
    while (!inside_gate)
        pthread_cond_wait(&changed, &state_lock);
    pthread_mutex_unlock(&state_lock);
    check(!pthread_create(&threads[1], NULL, call_driver, second), "second thread creation failed");
    pthread_mutex_lock(&state_lock);
    /* Wait for either the new mutex attempt or the baseline overlapping WMT call. */
    while (!second_progress)
        pthread_cond_wait(&changed, &state_lock);
    release_gate = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&state_lock);
    check(!pthread_join(threads[0], NULL) && !pthread_join(threads[1], NULL), "thread join failed");
}

static void open_open(int first_fails)
{
    struct call first = {1, 0, -1}, second = {2, 0, -1};

    reset_test();
    gate = GATE_ON;
    fail_first_on = first_fails;
    interleave(&first, &second);
    if (first_fails)
        check(first.result == -EIO && second.result == 0 && on_calls == 2,
              "failed first open did not permit a serialized retry");
    else
        check(first.result == 0 && second.result == -EIO && on_calls == 1,
              "simultaneous opens both reached function-on");
    check(!overlapping_power, "simultaneous opens overlapped power transitions");
    check_open();
    fail_first_on = 0;
    check(BT_close(NULL, NULL) == 0, "close after open/open test failed");
    check_closed(0);
}

static void close_open(void)
{
    struct call first = {1, 1, -1}, second = {2, 0, -1};

    reset_test();
    check(BT_open(NULL, NULL) == 0, "initial open failed");
    gate = GATE_OFF;
    interleave(&first, &second);
    check(first.result == 0 && second.result == 0, "close/open did not complete successfully");
    check(!overlapping_power, "new open overlapped the previous close's function-off");
    check(on_calls == 2 && off_calls == 1, "close/open called the wrong power transitions");
    check_open();
    check(BT_close(NULL, NULL) == 0, "final close failed");
    check_closed(0);
}

static void failures_and_retry(void)
{
    reset_test();
    fail_on = 1;
    check(BT_open(NULL, NULL) == -EIO && on_calls == 1 && !off_calls,
          "function-on failure handling changed");
    check_closed(0);
    fail_on = 0;
    ready = 0;
    check(BT_open(NULL, NULL) == -EIO && off_calls == 1, "STP readiness failure did not roll back");
    check_closed(0);
    ready = 1;
    check(BT_open(NULL, NULL) == 0, "retry after failed open did not unlock");
    check_open();
    check(BT_open(NULL, NULL) == -EIO && on_calls == 3, "already-open rejection changed");
    fail_off = 1;
    check(BT_close(NULL, NULL) == -EIO, "function-off failure was not returned");
    check_closed(1);
    fail_off = 0;
    check(BT_open(NULL, NULL) == 0, "retry after failed close did not unlock");
    check_open();
    check(BT_close(NULL, NULL) == 0, "normal close after retry failed");
    check_closed(0);
}

int main(int argc, char **argv)
{
    const char *selected = argc > 1 ? argv[1] : "all";

    if (!strcmp(selected, "all") || !strcmp(selected, "open-open")) {
        open_open(0);
        open_open(1);
    }
    if (!strcmp(selected, "all") || !strcmp(selected, "close-open"))
        close_open();
    if (!strcmp(selected, "all"))
        failures_and_retry();
    puts("PASS: BT open/close exclusion, transition ordering, and failure retry");
    return EXIT_SUCCESS;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="read driver source at a git revision")
    parser.add_argument("--case", choices=("all", "open-open", "close-open"), default="all")
    args = parser.parse_args()
    path = SOURCE / "bt/legacy/stp_chrdev_bt.c"
    if args.revision:
        root = Path(subprocess.check_output(
            ["git", "-C", str(SOURCE), "rev-parse", "--show-toplevel"], text=True
        ).strip())
        source = subprocess.check_output([
            "git", "-C", str(root), "show", args.revision + ":" + str(path.relative_to(root)),
        ], text=True)
    else:
        source = path.read_text()
    bodies = "\n".join(re.search(
        r"^static int " + name + r"\(.*?^\}", source, re.MULTILINE | re.DOTALL
    ).group(0) for name in ("BT_open", "BT_close"))
    locks = "\n".join(re.findall(r"^static DEFINE_MUTEX\([^\n]+\);", source, re.MULTILINE))
    failures = 0
    with tempfile.TemporaryDirectory(prefix="bt-open-") as work:
        for logging in (0, 1):
            cfile, binary = Path(work) / "open.c", Path(work) / ("open-" + str(logging))
            cfile.write_text(STUBS + locks + "\n" + bodies + TESTS)
            defines = ["-DCONFIG_MTK_CONNSYS_DEDICATED_LOG_PATH"] if logging else []
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                "-std=gnu11", "-g", "-O1", "-pthread", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-unused-function",
            ] + defines + [str(cfile), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary), args.case], check=False, timeout=10)
            failures += result.returncode != 0
            print(("PASS" if result.returncode == 0 else "FAIL")
                  + ": dedicated-log=" + str(logging), flush=True)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
