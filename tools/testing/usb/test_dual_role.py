#!/usr/bin/env python3
"""Exercise production dual-role registration with allocation/device failures."""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
CLASS = "drivers/usb/phy/class-dual-role.c"
MUSB = "drivers/misc/mediatek/mu3d/drv/musb_dual_role.c"


def function(source, name):
    match = re.search(rf"(?m)^[\w *\n]+?\b{name}\s*\([^;{{}}]*\)\s*\{{", source)
    if not match:
        raise ValueError(f"Missing function: {name}")
    start = match.start()
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


PREAMBLE = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GFP_KERNEL 0
#define __must_check
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define pr_debug(...) ((void)0)
struct device {
    void *class, *type;
    struct device *parent;
    void (*release)(struct device *);
    unsigned refs;
};
struct dual_role_phy_desc {
    const char *name;
    int supported_modes;
    int *properties;
    unsigned num_properties;
    int (*get_property)(void), (*set_property)(void), (*property_is_writeable)(void);
};
struct dual_role_phy_instance {
    struct device dev;
    const struct dual_role_phy_desc *desc;
    int changed_work;
};
struct musb { struct device *controller; };
static int dual_role_class_data, dual_role_dev_type;
static void *dual_role_class = &dual_role_class_data;
static int alloc_error, name_error, wake_error, add_error, register_error;
static unsigned frees, wake_on, wake_off, notifications, desc_frees;
static void *managed_desc;
static void *kzalloc(size_t size, int flags) {
    return alloc_error ? NULL : calloc(1, size);
}
static void kfree(void *p) { if (p) ++frees; free(p); }
static void device_initialize(struct device *dev) { dev->refs = 1; }
static void dev_set_drvdata(struct device *dev, void *data) {}
static int dev_set_name(struct device *dev, const char *format, const char *name) {
    return name_error;
}
static void put_device(struct device *dev) {
    assert(dev->refs == 1);
    if (!--dev->refs) dev->release(dev);
}
static int device_init_wakeup(struct device *dev, bool enable) {
    if (enable) { if (wake_error) return wake_error; ++wake_on; }
    else ++wake_off;
    return 0;
}
static int device_add(struct device *dev) { return add_error; }
#define INIT_WORK(work, callback) (*(work) = 1)
static void cancel_work_sync(int *work) { assert(*work == 1); }
static void device_unregister(struct device *dev) { put_device(dev); }
static void dual_role_instance_changed(struct dual_role_phy_instance *instance) {
    assert(instance && !IS_ERR(instance));
    ++notifications;
}
"""

BRIDGE = r"""
static struct dual_role_phy_instance *dr_usb;
static int state;
enum { DUALROLE_NONE, DUALROLE_DEVICE, DUALROLE_HOST };
enum { DUAL_ROLE_SUPPORTED_MODES_DFP_AND_UFP };
static int mt_dual_role_props[3];
static int mt_dual_role_get_prop(void) { return 0; }
static int mt_dual_role_set_prop(void) { return 0; }
static int mt_dual_role_prop_is_writeable(void) { return 0; }
static void *devm_kzalloc(struct device *dev, size_t size, int flags) {
    managed_desc = kzalloc(size, flags);
    return managed_desc;
}
static void devm_kfree(struct device *dev, void *p) {
    assert(p == managed_desc);
    free(p);
    managed_desc = NULL;
    ++desc_frees;
}
static struct dual_role_phy_instance *devm_dual_role_instance_register(
        struct device *parent, const struct dual_role_phy_desc *desc) {
    return register_error ? ERR_PTR(register_error) : __dual_role_register(parent, desc);
}
"""

MAIN = r"""
int main(int argc, char **argv) {
    assert(argc == 2);
    struct device parent = {0};
    struct musb musb = { .controller = &parent };
    struct dual_role_phy_desc desc = { .name = "test" };
    if (!strcmp(argv[1], "class_alloc")) alloc_error = -ENOMEM;
    else if (!strcmp(argv[1], "name")) name_error = -ENOMEM;
    else if (!strcmp(argv[1], "wakeup")) wake_error = -ENOMEM;
    else if (!strcmp(argv[1], "device_add")) add_error = -EIO;
    else if (!strcmp(argv[1], "bridge_alloc")) alloc_error = -ENOMEM;
    else if (!strcmp(argv[1], "bridge_register")) register_error = -ENOMEM;
    else if (strcmp(argv[1], "class_success") && strcmp(argv[1], "bridge_success")) abort();
    if (!strncmp(argv[1], "bridge_", 7)) {
        int ret = mt_usb_dual_role_init(&musb);
        unsigned before = notifications;
        mt_usb_dual_role_to_device();
        if (alloc_error || register_error) {
            assert(ret == -ENOMEM && dr_usb == NULL);
            assert(notifications == before);
            assert(managed_desc == NULL && desc_frees == (register_error ? 1u : 0u));
        } else {
            assert(ret == 0 && dr_usb && !IS_ERR(dr_usb));
            assert(notifications == before + 1);
            dual_role_instance_unregister(dr_usb);
            assert(frees == 1 && wake_on == 1 && wake_off == 1);
            dr_usb = NULL;
        }
        free(managed_desc);
    } else {
        struct dual_role_phy_instance *instance = __dual_role_register(&parent, &desc);
        int expected = alloc_error + name_error + wake_error + add_error;
        if (expected) {
            assert(IS_ERR(instance) && PTR_ERR(instance) == expected);
            assert(frees == (alloc_error ? 0u : 1u));
            assert(notifications == 0 && wake_on == wake_off);
        } else {
            assert(instance && !IS_ERR(instance) && notifications == 1);
            dual_role_instance_unregister(instance);
            assert(frees == 1 && wake_on == 1 && wake_off == 1);
        }
    }
    printf("PASS %s\n", argv[1]);
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--revision")
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)

    def read(path):
        if args.revision:
            return subprocess.check_output(
                ["git", "show", f"{args.revision}:{path}"], cwd=ROOT, text=True)
        return (ROOT / path).read_text()

    generic, musb = read(CLASS), read(MUSB)
    source = PREAMBLE + "\n".join(function(generic, name) for name in (
        "dual_role_dev_release", "__dual_role_register", "dual_role_instance_unregister"))
    source += BRIDGE + "\n".join(function(musb, name) for name in (
        "mt_usb_dual_role_changed", "mt_usb_dual_role_to_device", "mt_usb_dual_role_init"))
    with tempfile.TemporaryDirectory(dir=args.scratch) as directory:
        path = Path(directory) / "dual-role.c"
        path.write_text(source + MAIN)
        binary = path.with_suffix("")
        subprocess.run(["cc", "-g", "-Wall", "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", str(path), "-o", str(binary)], check=True)
        failed = []
        for case in ("class_alloc", "name", "wakeup", "device_add", "class_success",
                     "bridge_alloc", "bridge_register", "bridge_success"):
            result = subprocess.run([str(binary), case], timeout=10)
            if result.returncode:
                failed.append(case)
        if failed:
            raise SystemExit("FAIL: " + ", ".join(failed))


if __name__ == "__main__":
    main()
