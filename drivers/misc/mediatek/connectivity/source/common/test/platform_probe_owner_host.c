// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* platform.c is built into the kernel even when its caller is a module. */
#define THIS_MODULE NULL
#define __init_or_module
#define platform_driver_register(drv) __platform_driver_register(drv, THIS_MODULE)

struct module { unsigned identity; };
struct platform_device { int unused; };
struct list_head { bool empty; };
struct klist { struct list_head k_list; int k_lock; };
struct bus_private { struct klist klist_drivers; };
struct bus_type { struct bus_private *p; };
struct driver_private { struct klist klist_devices; };
struct device_driver {
    struct module *owner;
    struct bus_type *bus;
    int (*probe)(struct platform_device *);
    int (*remove)(struct platform_device *);
    void (*shutdown)(struct platform_device *);
    bool suppress_bind_attrs;
    struct driver_private *p;
};
struct platform_driver {
    struct device_driver driver;
    int (*probe)(struct platform_device *);
    int (*remove)(struct platform_device *);
    void (*shutdown)(struct platform_device *);
    bool prevent_deferred_probe;
};

static struct bus_private bus_private;
static struct bus_type platform_bus_type = { &bus_private };
static struct driver_private driver_private;
static struct module *observed_owner, *removed_owner;
static int registration_error, unregisters, held_lock;
static bool bound;

static int platform_drv_probe(struct platform_device *p) { (void)p; return 0; }
static int platform_drv_remove(struct platform_device *p) { (void)p; return 0; }
static void platform_drv_shutdown(struct platform_device *p) { (void)p; }
static int platform_drv_probe_fail(struct platform_device *p) { (void)p; return -ENODEV; }
static void spin_lock(int *lock) { (void)lock; assert(!held_lock); held_lock = 1; }
static void spin_unlock(int *lock) { (void)lock; assert(held_lock); held_lock = 0; }
static bool list_empty(struct list_head *list) { assert(held_lock); return list->empty; }

static int driver_register(struct device_driver *driver)
{
    assert(driver->suppress_bind_attrs);
    assert(driver->bus == &platform_bus_type);
    observed_owner = driver->owner;
    if (registration_error)
        return registration_error;
    driver_private.klist_devices.k_list.empty = !bound;
    driver->p = &driver_private;
    return 0;
}

static void driver_unregister(struct device_driver *driver)
{
    assert(!held_lock && driver->p);
    removed_owner = driver->owner;
    driver->p = NULL;
    unregisters++;
}

/* ACTUAL_PLATFORM_HELPERS */

static void run_probe(struct platform_driver *driver, struct module *owner,
                      bool should_bind, int error)
{
    bound = should_bind;
    registration_error = error;
    observed_owner = removed_owner = NULL;
    unregisters = 0;
    int result = platform_driver_probe(driver, platform_drv_probe);
    assert(result == (error ? error : should_bind ? 0 : -ENODEV));
    assert(driver->driver.owner == owner);
    assert(observed_owner == owner);
    assert(driver->prevent_deferred_probe && driver->driver.suppress_bind_attrs);
    assert(!driver->probe && driver->driver.probe == platform_drv_probe_fail);
    assert(!held_lock);
    if (!error && !should_bind) {
        assert(unregisters == 1 && removed_owner == owner && !driver->driver.p);
    } else {
        assert(unregisters == 0);
        if (!error) {
            platform_driver_unregister(driver);
            assert(unregisters == 1 && removed_owner == owner && !driver->driver.p);
        }
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *name = argv[1];
    struct module owner_a = { 1 }, owner_b = { 2 };
    bool builtin = !strncmp(name, "builtin-", 8);
    struct module *owner = builtin ? NULL : &owner_a;
    struct platform_driver driver = { .driver.owner = owner };
    if (strstr(name, "-registration-error")) {
        run_probe(&driver, owner, false, -EBUSY);
    } else if (strstr(name, "-unbound")) {
        run_probe(&driver, owner, false, 0);
    } else if (!strcmp(name, "module-retry")) {
        run_probe(&driver, owner, false, 0);
        run_probe(&driver, owner, true, 0);
    } else if (!strcmp(name, "distinct-module-owners")) {
        run_probe(&driver, owner, true, 0);
        struct platform_driver second = { .driver.owner = &owner_b };
        run_probe(&second, &owner_b, true, 0);
        assert(driver.driver.owner == &owner_a);
    } else {
        assert(!strcmp(name, "module-bound") || !strcmp(name, "builtin-bound"));
        run_probe(&driver, owner, true, 0);
    }
    printf("PASS %s\n", name);
    return 0;
}
