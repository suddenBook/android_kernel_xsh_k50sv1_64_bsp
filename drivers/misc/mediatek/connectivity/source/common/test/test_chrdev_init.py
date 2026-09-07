#!/usr/bin/env python3
"""Fault-inject the actual MT6755 GPS/BT character-device init functions.

The host stubs track kernel resource ownership and readiness when cdev_add()
publishes the device. No kernel build, module load, or handset is required.
Pass --revision REF to run against a committed version of the driver sources.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


SOURCE = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(
        r"^(?:static )?(?:int|void) (?:__(?:init|exit) )?"
        + re.escape(name)
        + r"\(void\)\n\{.*?^\}",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not match:
        raise ValueError("function not found: " + name)
    return match.group(0)


STUBS = r'''
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define __init
#define __exit
#define THIS_MODULE ((void *)1)
#define MKDEV(major, minor) ((dev_t)((major) * 256 + (minor)))
#define IS_ERR(ptr) ((uintptr_t)(ptr) >= (uintptr_t)-4095)
#define PTR_ERR(ptr) ((long)(ptr))
#define ERR_PTR(err) ((void *)(intptr_t)(err))
#define pr_warn(...) ((void)0)
#define pr_info(...) ((void)0)
#define BT_LOG_PRT_ERR(...) ((void)0)
#define BT_LOG_PRT_INFO(...) ((void)0)
#define GPS_DRIVER_NAME "gps-test"
#define BT_DRIVER_NAME "bt-test"

struct semaphore { int count; };
struct cdev { void *owner; };
struct class { int unused; };
struct device { int unused; };
struct wakeup_source { int unused; };
typedef int INT32;

static struct semaphore status_mtx, fwctl_mtx, wr_mtx, rd_mtx;
static struct cdev GPS_cdev, BT_cdev;
static int GPS_devs = 1, BT_devs = 1;
static int GPS_major = 191, BT_major = 192;
static int GPS_fops, BT_fops, inq;
static struct class *stpgps_class, *stpbt_class;
static struct device *stpbt_dev;
static struct wakeup_source *gps_wake_lock_ptr, *bt_wakelock;
static struct class class_value;
static struct device device_value;
static struct wakeup_source wake_value;
static int wake_live, region_live, cdev_live, class_live, device_live;
static int errors, failure;
static const char *case_name;

enum { OK, WAKE, REGION, CDEV, CLASS, DEVICE };

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s: %s\n", case_name, message);
        errors++;
    }
}

static void sema_init(struct semaphore *sem, int value)
{
    sem->count = value;
}

static void init_waitqueue_head(int *queue) { *queue = 1; }

static struct wakeup_source *wakeup_source_register(const char *name)
{
    if (failure == WAKE)
        return NULL;
    check(!wake_live, "wakeup source allocated twice");
    wake_live = 1;
    return &wake_value;
}

static void wakeup_source_unregister(struct wakeup_source *wake)
{
    check(wake == &wake_value && wake_live, "invalid wakeup source cleanup");
    wake_live = 0;
}

static int register_chrdev_region(dev_t dev, int count, const char *name)
{
    if (failure == REGION)
        return -EBUSY;
    check(!region_live, "device numbers allocated twice");
    region_live = 1;
    return 0;
}

static void unregister_chrdev_region(dev_t dev, int count)
{
    check(region_live && !cdev_live, "invalid device number cleanup");
    region_live = 0;
}

static void cdev_init(struct cdev *dev, const void *fops) {}

static int cdev_add(struct cdev *dev, dev_t number, int count)
{
    if (failure == CDEV)
        return -ENOSPC;
    check(region_live && !cdev_live, "invalid character-device publication");
    check(wake_live && wr_mtx.count == 1 && rd_mtx.count == 1,
          "character device published before wakeup source/read/write locks");
#ifdef TEST_GPS
    check(status_mtx.count == 1 && fwctl_mtx.count == 1,
          "GPS character device published before status/firmware locks");
#else
    check(inq == 1, "BT character device published before wait queue");
#endif
    cdev_live = 1;
    return 0;
}

static void cdev_del(struct cdev *dev)
{
    check(cdev_live, "unregistered character device deleted");
    cdev_live = 0;
}

static struct class *class_create(void *owner, const char *name)
{
    if (failure == CLASS)
        return ERR_PTR(-ENOMEM);
    check(!class_live, "class allocated twice");
    class_live = 1;
    return &class_value;
}

static void class_destroy(struct class *cls)
{
    check(cls == &class_value && class_live && !device_live,
          "invalid class cleanup");
    class_live = 0;
}

static struct device *device_create(struct class *cls, void *parent,
                                   dev_t dev, void *data, const char *name)
{
    check(cls == &class_value && class_live, "device created without a class");
    if (failure == DEVICE)
        return ERR_PTR(-ENODEV);
    check(!device_live, "device allocated twice");
    device_live = 1;
    return &device_value;
}

static void device_destroy(struct class *cls, dev_t dev)
{
    check(cls == &class_value && class_live && device_live,
          "invalid device cleanup");
    device_live = 0;
}
'''

TEST = r'''
int main(void)
{
    static const char *names[] = {"success", "wakeup allocation", "device numbers",
                                  "cdev registration", "class creation", "device creation"};
    static const int expected[] = {0, -ENOMEM, -EBUSY, -ENOSPC, -ENOMEM, -ENODEV};
    int ret;

    for (failure = OK; failure <= (DYNAMIC ? DEVICE : CDEV); failure++) {
        case_name = names[failure];
        wake_live = region_live = cdev_live = class_live = device_live = 0;
        status_mtx.count = fwctl_mtx.count = wr_mtx.count = rd_mtx.count = 0;
        stpgps_class = stpbt_class = NULL;
        stpbt_dev = NULL;
        gps_wake_lock_ptr = bt_wakelock = NULL;
        inq = 0;

#ifdef TEST_GPS
        ret = gps_mod_init();
#else
        ret = BT_init();
#endif
        if (ret != expected[failure]) {
            fprintf(stderr, "%s: expected errno %d, got %d\n",
                    case_name, expected[failure], ret);
            errors++;
        }
        if (!ret) {
            check(wake_live && region_live && cdev_live,
                  "init returned success without its required resources");
            check(class_live == DYNAMIC && device_live == DYNAMIC,
                  "init returned success without the requested device node");
#ifdef TEST_GPS
            gps_mod_exit();
#else
            BT_exit();
#endif
        }
        check(!wake_live && !region_live && !cdev_live && !class_live && !device_live,
              "resources leaked after failed init or normal exit");
    }
    return errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="read driver sources at a git revision")
    args = parser.parse_args()
    git_root = Path(subprocess.check_output(
        ["git", "-C", str(SOURCE), "rev-parse", "--show-toplevel"], text=True
    ).strip())
    failures = 0
    with tempfile.TemporaryDirectory(prefix="connectivity-chrdev-init-") as work:
        work = Path(work)
        for driver, relative, names in (
            ("GPS", "gps/stp_chrdev_gps.c", (
                "GPS_init", "GPS_exit", "mtk_wcn_stpgps_drv_init",
                "mtk_wcn_stpgps_drv_exit", "gps_mod_init", "gps_mod_exit")),
            ("BT", "bt/legacy/stp_chrdev_bt.c", ("BT_init", "BT_exit")),
        ):
            path = SOURCE / relative
            source = (subprocess.check_output(
                ["git", "-C", str(git_root), "show",
                 args.revision + ":" + str(path.relative_to(git_root))], text=True
            ) if args.revision else path.read_text())
            extracted = "\n\n".join(function(source, name) for name in names)
            for dynamic in (0, 1):
                case = driver + "-dynamic-" + str(dynamic)
                cfile, binary = work / (case + ".c"), work / case
                cfile.write_text(STUBS + "\n" + extracted + "\n" + TEST)
                subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                    "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
                    "-DTEST_" + driver, "-DDYNAMIC=" + str(dynamic),
                    "-DWMT_CREATE_NODE_DYNAMIC=" + str(dynamic),
                    "-DCREATE_NODE_DYNAMIC=" + str(dynamic), "-DREMOVE_MK_NODE=0",
                    str(cfile), "-o", str(binary),
                ], check=True)
                result = subprocess.run([str(binary)], check=False)
                failures += result.returncode != 0
                print(("PASS" if result.returncode == 0 else "FAIL") + ": " + case, flush=True)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
