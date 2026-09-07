#!/usr/bin/env python3
"""Run the real GPS open/close functions against transport-failure host stubs."""

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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define MTK_WCN_BOOL_FALSE 0
#define MTK_WCN_BOOL_TRUE 1
#define WMTDRV_TYPE_GPS 2
#define GPS_TASK_INDX 2
#define pr_warn(...) ((void)0)
#define GPS_ERR_FUNC(...) ((void)0)
#define GPS_DBG_FUNC(...) ((void)0)
#define GPS_WARN_FUNC(...) report_warning()
struct inode { int unused; };
struct file { int unused; };
struct task { int pid; };
static struct task task = {123};
#define current (&task)

enum gps_ctrl_status_enum {
    GPS_CLOSED, GPS_OPENED, GPS_SUSPENDED, GPS_RESET_START, GPS_RESET_DONE, GPS_CTRL_STATUS_NUM
};
static enum gps_ctrl_status_enum g_gps_ctrl_status;
static int rstflag, rst_happened_or_gps_close_flag;
static int ready, fail_on, fail_off, inject_reset;
static int power_on, on_calls, off_calls, wake_locked, desense;
static int rollback_failed, rollback_warnings;
static void (*reset_callback)(void), (*event_callback)(void);

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void report_warning(void)
{
    if (rollback_failed)
        rollback_warnings++;
}

static void GPS_event_cb(void) {}
static void gps_cdev_rst_cb(void) {}

static int mtk_wcn_wmt_func_on(int type)
{
    check(type == WMTDRV_TYPE_GPS, "wrong function enabled");
    on_calls++;
    if (fail_on)
        return MTK_WCN_BOOL_FALSE;
    power_on = 1;
    return MTK_WCN_BOOL_TRUE;
}

static int mtk_wcn_wmt_func_off(int type)
{
    check(type == WMTDRV_TYPE_GPS && power_on, "function-off without successful function-on");
    off_calls++;
    if (!ready)
        check(!reset_callback, "failed open retained its reset callback during rollback");
    if (fail_off) {
        rollback_failed = 1;
        return MTK_WCN_BOOL_FALSE;
    }
    power_on = 0;
    return MTK_WCN_BOOL_TRUE;
}

static int mtk_wcn_wmt_msgcb_reg(int type, void (*callback)(void))
{
    check(type == WMTDRV_TYPE_GPS && !reset_callback, "reset callback registered twice");
    reset_callback = callback;
    /* wmt_lib_msgcb_reg returns TRUE for the fixed valid GPS enum. */
    return MTK_WCN_BOOL_TRUE;
}

static int mtk_wcn_wmt_msgcb_unreg(int type)
{
    check(type == WMTDRV_TYPE_GPS && reset_callback, "reset callback removed without registration");
    reset_callback = NULL;
    return MTK_WCN_BOOL_TRUE;
}

static int mtk_wcn_stp_register_event_cb(int type, void (*callback)(void))
{
    check(type == GPS_TASK_INDX, "wrong STP task callback");
    event_callback = callback;
    /* The imported STP registration function returns zero. */
    return 0;
}

static int mtk_wcn_stp_is_ready(void)
{
    if (inject_reset) {
        check(reset_callback != NULL, "missing reset callback before reset injection");
        rstflag = 1;
        g_gps_ctrl_status = GPS_RESET_START;
    }
    return ready;
}

static void gps_hold_wake_lock(int hold) { wake_locked = hold; }
static void GPS_handle_desense(bool on) { desense = on; }
static enum gps_ctrl_status_enum GPS_ctrl_status_change_to(enum gps_ctrl_status_enum to)
{
    enum gps_ctrl_status_enum previous = g_gps_ctrl_status;
    g_gps_ctrl_status = to;
    return previous;
}
'''

TESTS = r'''
static void reset_test(void)
{
    ready = 1;
    task.pid = 123;
    fail_on = fail_off = inject_reset = 0;
    power_on = on_calls = off_calls = wake_locked = desense = 0;
    rollback_failed = rollback_warnings = rstflag = 0;
    reset_callback = event_callback = NULL;
    g_gps_ctrl_status = GPS_CLOSED;
}

static void open_and_close(void)
{
    check(GPS_open(NULL, NULL) == 0, "normal/retry open failed");
    check(power_on && wake_locked && desense && g_gps_ctrl_status == GPS_OPENED,
          "successful open did not acquire its runtime state");
    check(reset_callback == gps_cdev_rst_cb && event_callback == GPS_event_cb,
          "successful open did not register its callbacks");
    check(GPS_close(NULL, NULL) == 0, "normal close failed");
    check(!power_on && !wake_locked && !desense && !reset_callback && !event_callback,
          "normal close leaked runtime state");
    check(g_gps_ctrl_status == GPS_CLOSED, "normal close left wrong control state");
}

int main(void)
{
    int off_failure, reset;

    for (off_failure = 0; off_failure <= 1; off_failure++)
        for (reset = 0; reset <= 1; reset++) {
            reset_test();
            ready = 0;
            fail_off = off_failure;
            inject_reset = reset;
            check(GPS_open(NULL, NULL) == -ENODEV, "unready transport did not fail open");
            check(on_calls == 1 && off_calls == 1, "failed open did not balance function-on with function-off");
            check(power_on == off_failure, "rollback power state did not match transport result");
            check(!reset_callback && !event_callback && !wake_locked && !desense,
                  "failed open leaked a callback or runtime resource");
            check(!rstflag && g_gps_ctrl_status == GPS_CLOSED, "failed open left stale reset/open state");
            if (off_failure)
                check(rollback_warnings > 0, "failed power rollback was not reported");
            ready = 1;
            fail_off = inject_reset = rollback_failed = 0;
            open_and_close();
        }

    reset_test();
    fail_on = 1;
    check(GPS_open(NULL, NULL) == -ENODEV, "function-on failure was not returned");
    check(!off_calls && !reset_callback && !event_callback, "function-on failure acquired or released extra state");
    fail_on = 0;
    open_and_close();

    reset_test();
    rstflag = 1;
    check(GPS_open(NULL, NULL) == -EPERM && !on_calls, "reset-in-progress guard changed");
    reset_test();
    task.pid = 1;
    check(GPS_open(NULL, NULL) == 0 && !on_calls, "init-process open behavior changed");
    puts("PASS: GPS transport rollback, rollback failure, reset state, retry, and normal close");
    return EXIT_SUCCESS;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="read driver source at a git revision")
    args = parser.parse_args()
    path = SOURCE / "gps/stp_chrdev_gps.c"
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
    ).group(0) for name in ("GPS_open", "GPS_close"))
    with tempfile.TemporaryDirectory(prefix="gps-open-") as work:
        cfile, binary = Path(work) / "open.c", Path(work) / "open"
        cfile.write_text(STUBS + bodies + TESTS)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            str(cfile), "-o", str(binary),
        ], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
