/* Complete production functions are inserted between the host kernel adapters. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void VOID;
typedef int INT32;
typedef unsigned int UINT32;
typedef unsigned char UINT8;
typedef unsigned char *PUINT8;
typedef int *PINT32;
typedef unsigned int *PUINT32;
typedef uintptr_t SIZE_T;
typedef uintptr_t phys_addr_t;
typedef uintptr_t resource_size_t;
typedef unsigned long long UINT64;
typedef int MTK_WCN_BOOL;
typedef int pm_message_t;
typedef struct { int unused; } CONSYS_EMI_ADDR_INFO, *P_CONSYS_EMI_ADDR_INFO;
#define MTK_WCN_BOOL_TRUE 1
#define MTK_WCN_BOOL_FALSE 0
#define __iomem
#define __init_or_module
#define CONFIG_OF 1
#define GFP_KERNEL 0
#define EPROBE_DEFER 517
#define EXPORT_SYMBOL(value)
#define EXPORT_SYMBOL_GPL(value)
#define THIS_MODULE NULL
#define ARRAY_SIZE(value) (sizeof(value) / sizeof((value)[0]))
#define ERR_PTR(value) ((void *)(intptr_t)(value))
#define PTR_ERR(value) ((long)(intptr_t)(value))
#define IS_ERR(value) ((uintptr_t)(value) >= (uintptr_t)-4095)
#define WMT_PLAT_PR_ERR(...) ((void)0)
#define WMT_PLAT_PR_INFO(...) ((void)0)
#define WMT_PLAT_PR_DBG(...) ((void)0)
#define WMT_PLAT_PR_WARN(...) ((void)0)
#define dev_warn(...) ((void)0)
#define SZ_1M (1024U * 1024U)
#define FORBIDDEN 0
#define NO_PROTECTION 1
#define SET_ACCESS_PERMISSON(...) 0
#define CONSYS_REG_READ(address) (*(UINT32 *)(address))
#define CONSYS_REG_WRITE(address, value) (*(UINT32 *)(address) = (value))
#define NORMAL_GET 0
#define EXCLUSIVE_GET 1
#define OPTIONAL_GET 2

/* SOURCE_CONSTANTS */

struct device;
struct platform_device;
struct driver_private;
struct bus_type;
struct devres_node;
struct device_node { int references, role; };
struct device_driver {
    const char *name;
    void *owner;
    const void *of_match_table;
    struct bus_type *bus;
    struct driver_private *p;
    bool suppress_bind_attrs;
    int (*probe)(struct device *);
};
struct device {
    struct device_node *of_node;
    struct device_driver *driver;
    struct devres_node *resources;
};
struct platform_device { struct device dev; };
struct platform_driver {
    int (*probe)(struct platform_device *);
    int (*remove)(struct platform_device *);
    int (*suspend)(struct platform_device *, pm_message_t);
    int (*resume)(struct platform_device *);
    struct device_driver driver;
    bool prevent_deferred_probe;
};
struct list_head { unsigned int count; };
struct driver_private { struct { struct list_head k_list; } klist_devices; };
struct bus_private { struct { bool k_lock; } klist_drivers; };
struct bus_type { struct bus_private *p; };
#define container_of(pointer, type, member) ((type *)((char *)(pointer) - offsetof(type, member)))
#define to_platform_driver(pointer) container_of(pointer, struct platform_driver, driver)
#define to_platform_device(pointer) container_of(pointer, struct platform_device, dev)
#define list_empty(list) (!(list)->count)
struct resource { resource_size_t start, end; };
#define resource_size(resource) ((resource)->end - (resource)->start + 1)
struct clk { int marker; };
struct regulator { int marker; };
struct pinctrl { int marker; };
static const int apwmt_of_ids[] = {0};
static int mtk_wmt_suspend(struct platform_device *pdev, pm_message_t state) { return 0; }
static int mtk_wmt_resume(struct platform_device *pdev) { return 0; }

/* SOURCE_TYPES */
/* SOURCE_STATES */
/* SOURCE_PROTOTYPES */
/* SOURCE_UNUSED_OPS */
/* SOURCE_OPS */
/* SOURCE_DRIVER */

static phys_addr_t gConEmiPhyBase = 0x90000000;
static UINT64 gConEmiSize = SZ_1M;
static struct device_node root_node, gps_node, pins_node;
static struct platform_device device = { .dev = { .of_node = &root_node } };
static struct bus_private bus_private;
static struct bus_type platform_bus = { .p = &bus_private };
static struct driver_private driver_private;

enum allocation_kind { REG_MAP, EMI_CLEAR, EMI_DUMP, CLK_HANDLE, REGULATOR_HANDLE, PIN_HANDLE };
struct allocation { void *address; size_t size; enum allocation_kind kind; struct allocation *next; };
static struct allocation *allocations;
static unsigned int live_allocations, live_devres;
static unsigned int map_attempts, regulator_attempts, map_devres_attempts, regulator_devres_attempts;
static unsigned int register_calls, unregister_calls, rollback_calls, probe_calls, remove_calls;
static unsigned int clear_calls, dump_mappings, release_checks;
static bool driver_registered, device_bound, pm_domain_attached;
static bool have_device = true, fail_framework, no_gps_phandle, no_gps_child, no_gps_properties;
static bool gps_uses_pinmux = true;
static int fail_register, fail_groups, fail_resource = -1, fail_map = -1, fail_map_devres = -1;
static int fail_clock, fail_regulator = -1, fail_regulator_devres = -1, fail_pinctrl;
static bool fail_clock_devres, fail_pinctrl_devres, fail_emi_clear, fail_emi_dump;
static int wifi_gpio = 27;

static void *allocate_resource(enum allocation_kind kind, size_t size)
{
    struct allocation *record = calloc(1, sizeof(*record));
    assert(record);
    record->address = calloc(1, size);
    assert(record->address);
    record->size = size;
    record->kind = kind;
    record->next = allocations;
    allocations = record;
    live_allocations++;
    return record->address;
}

static struct allocation *find_allocation(const void *address)
{
    struct allocation *record;
    for (record = allocations; record; record = record->next)
        if (record->address == address)
            return record;
    assert(!"Unknown or already released resource");
    return NULL;
}

static void free_resource(void *address, enum allocation_kind kind)
{
    struct allocation **position = &allocations;
    assert(address && !IS_ERR(address));
    while (*position && (*position)->address != address)
        position = &(*position)->next;
    assert(*position && (*position)->kind == kind);
    struct allocation *record = *position;
    *position = record->next;
    assert(live_allocations-- > 0);
    free(record->address);
    free(record);
}

static void require_handles_withdrawn(void)
{
    fprintf(stderr, "withdrawn=%d maps=%u devres=%u bound=%d\n",
            !g_pdev && !consys_pinctrl && !conn_reg.mcu_base && !conn_reg.ap_rgu_base &&
            !conn_reg.topckgen_base && !conn_reg.spm_base && !clk_scp_conn_main &&
            !reg_VCN18 && !reg_VCN28 && !reg_VCN33_BT && !reg_VCN33_WIFI,
            live_allocations, live_devres, device_bound);
    assert(!g_pdev && !consys_pinctrl && !pEmibaseaddr);
    assert(!conn_reg.mcu_base && !conn_reg.ap_rgu_base && !conn_reg.topckgen_base && !conn_reg.spm_base);
    assert(!clk_scp_conn_main && !reg_VCN18 && !reg_VCN28 && !reg_VCN33_BT && !reg_VCN33_WIFI);
    release_checks++;
}

struct devres_node {
    void (*release)(struct device *, void *);
    struct device *owner;
    struct devres_node *next;
    unsigned char data[];
};

static void *devres_alloc(void (*release)(struct device *, void *), size_t size, int flags)
{
    if (release == devm_ioremap_release && (int)map_devres_attempts++ == fail_map_devres)
        return NULL;
    if (release == devm_clk_release && fail_clock_devres)
        return NULL;
    if (release == devm_regulator_release && (int)regulator_devres_attempts++ == fail_regulator_devres)
        return NULL;
    if (release == devm_pinctrl_release && fail_pinctrl_devres)
        return NULL;
    struct devres_node *node = calloc(1, sizeof(*node) + size);
    assert(node);
    node->release = release;
    live_devres++;
    return node->data;
}

static void devres_add(struct device *dev, void *data)
{
    struct devres_node *node = container_of(data, struct devres_node, data);
    assert(!node->owner);
    node->owner = dev;
    node->next = dev->resources;
    dev->resources = node;
}

static void devres_free(void *data)
{
    struct devres_node *node = container_of(data, struct devres_node, data);
    assert(!node->owner);
    assert(live_devres-- > 0);
    free(node);
}

static void devres_release_all(struct device *dev)
{
    /* Local drivers/base/dd.c calls this after failed probe or after remove. */
    require_handles_withdrawn();
    while (dev->resources) {
        struct devres_node *node = dev->resources;
        dev->resources = node->next;
        assert(node->owner == dev);
        node->release(dev, node->data);
        assert(live_devres-- > 0);
        free(node);
    }
}

static int of_address_to_resource(struct device_node *node, unsigned int index, struct resource *resource)
{
    static const uintptr_t addresses[] = {0x18070000, 0x10007000, 0x10000000, 0x10006000};
    static const size_t sizes[] = {0x200, 0x100, 0x2000, 0x1000};
    assert(node == &root_node && index < 4);
    if ((int)index == fail_resource)
        return -EINVAL;
    resource->start = addresses[index];
    resource->end = addresses[index] + sizes[index] - 1;
    return 0;
}

static void *ioremap(resource_size_t address, unsigned long size)
{
    if ((int)map_attempts++ == fail_map)
        return NULL;
    return allocate_resource(REG_MAP, size);
}

static void *of_iomap(struct device_node *node, unsigned int index)
{
    /* The local OF helper is exactly address translation followed by ioremap. */
    struct resource resource;
    if (of_address_to_resource(node, index, &resource))
        return NULL;
    return ioremap(resource.start, resource_size(&resource));
}

static void *ioremap_nocache(resource_size_t address, unsigned long size)
{
    if (address == gConEmiPhyBase) {
        if (fail_emi_clear)
            return NULL;
        return allocate_resource(EMI_CLEAR, size);
    }
    assert(address == gConEmiPhyBase + CONSYS_EMI_COREDUMP_OFFSET);
    if (fail_emi_dump)
        return NULL;
    dump_mappings++;
    return allocate_resource(EMI_DUMP, size);
}

static void iounmap(void *address)
{
    struct allocation *record = find_allocation(address);
    enum allocation_kind kind = record->kind;
    assert(kind == REG_MAP || kind == EMI_CLEAR || kind == EMI_DUMP);
    if (kind == REG_MAP) {
        assert((SIZE_T)address != conn_reg.mcu_base && (SIZE_T)address != conn_reg.ap_rgu_base);
        assert((SIZE_T)address != conn_reg.topckgen_base && (SIZE_T)address != conn_reg.spm_base);
    } else if (kind == EMI_DUMP) {
        assert(address != pEmibaseaddr);
    }
    free_resource(address, kind);
}

static void memset_io(void *address, int value, size_t size)
{
    assert(address);
    assert(size <= find_allocation(address)->size);
    memset(address, value, size);
    clear_calls++;
}

static struct clk *clk_get(struct device *dev, const char *name)
{
    assert(dev == &device.dev && strcmp(name, "conn") == 0);
    if (fail_clock)
        return ERR_PTR(fail_clock);
    return allocate_resource(CLK_HANDLE, sizeof(struct clk));
}

static void clk_put(struct clk *clock)
{
    assert(clock != clk_scp_conn_main);
    free_resource(clock, CLK_HANDLE);
}

static struct regulator *regulator_get(struct device *dev, const char *name)
{
    static const char *names[] = {"vcn18", "vcn28", "vcn33_bt", "vcn33_wifi"};
    unsigned int index = regulator_attempts++;
    assert(dev == &device.dev && index < ARRAY_SIZE(names) && strcmp(name, names[index]) == 0);
    if ((int)index == fail_regulator)
        return ERR_PTR(-EPROBE_DEFER);
    return allocate_resource(REGULATOR_HANDLE, sizeof(struct regulator));
}
static struct regulator *regulator_get_exclusive(struct device *dev, const char *name)
{ return regulator_get(dev, name); }
static struct regulator *regulator_get_optional(struct device *dev, const char *name)
{ return regulator_get(dev, name); }

static void regulator_put(struct regulator *regulator)
{
    assert(regulator != reg_VCN18 && regulator != reg_VCN28);
    assert(regulator != reg_VCN33_BT && regulator != reg_VCN33_WIFI);
    free_resource(regulator, REGULATOR_HANDLE);
}

static struct pinctrl *pinctrl_get(struct device *dev)
{
    assert(dev == &device.dev);
    if (fail_pinctrl)
        return ERR_PTR(fail_pinctrl);
    return allocate_resource(PIN_HANDLE, sizeof(struct pinctrl));
}
static void pinctrl_put(struct pinctrl *pinctrl)
{
    assert(pinctrl != consys_pinctrl);
    free_resource(pinctrl, PIN_HANDLE);
}

static struct device_node *of_parse_phandle(struct device_node *node, const char *name, int index)
{
    assert(node == &root_node && strcmp(name, "pinctrl-1") == 0 && index == 0);
    if (no_gps_phandle)
        return NULL;
    gps_node.references++;
    return &gps_node;
}
static struct device_node *of_get_child_by_name(struct device_node *node, const char *name)
{
    assert(node == &gps_node && strcmp(name, "pins_cmd_dat") == 0);
    if (no_gps_child)
        return NULL;
    pins_node.references++;
    return &pins_node;
}
static int of_property_read_u32(struct device_node *node, const char *name, UINT32 *value)
{
    assert(node == &pins_node);
    if (no_gps_properties || (strcmp(name, "pinmux") == 0 && !gps_uses_pinmux))
        return -EINVAL;
    *value = 27U << 8;
    return 0;
}
static void of_node_put(struct device_node *node)
{
    if (node)
        assert(node->references-- > 0);
}
static int of_get_named_gpio(struct device_node *node, const char *name, int index)
{
    assert(node == &root_node && strcmp(name, "wifi_ant_swap_gpio") == 0 && index == 0);
    return wifi_gpio;
}

static void pm_runtime_enable(struct device *dev) { assert(!"MT6755 does not request runtime PM"); }
static void pm_runtime_disable(struct device *dev) { assert(!"MT6755 does not request runtime PM"); }
static void emi_mpu_set_region_protection(phys_addr_t start, phys_addr_t end, int region, int flags)
{ assert(start <= end && region == 13); }
static void spin_lock(bool *lock) { assert(!*lock); *lock = true; }
static void spin_unlock(bool *lock) { assert(*lock); *lock = false; }
static int of_clk_set_defaults(struct device_node *node, bool supplier)
{ return fail_framework ? -EPROBE_DEFER : 0; }
static int dev_pm_domain_attach(struct device *dev, bool power_on)
{
    assert(!pm_domain_attached);
    pm_domain_attached = true;
    return 0;
}
static void dev_pm_domain_detach(struct device *dev, bool power_off)
{
    assert(pm_domain_attached);
    pm_domain_attached = false;
}

static int platform_driver_register(struct platform_driver *driver)
{
    register_calls++;
    driver->driver.bus = &platform_bus;
    if (fail_register)
        return fail_register;
    assert(!driver_registered);
    driver_registered = true;
    driver->driver.p = &driver_private;
    driver_private.klist_devices.k_list.count = 0;
    if (have_device) {
        device.dev.driver = &driver->driver;
        probe_calls++;
        int ret = platform_drv_probe(&device.dev);
        if (ret) {
            /* really_probe() releases devres, then deliberately returns 0. */
            devres_release_all(&device.dev);
            device.dev.driver = NULL;
        } else {
            device_bound = true;
            driver_private.klist_devices.k_list.count = 1;
        }
    }
    if (fail_groups) {
        /* driver_register() calls bus_remove_driver() on add-groups failure. */
        if (device_bound) {
            remove_calls++;
            assert(platform_drv_remove(&device.dev) == 0);
            devres_release_all(&device.dev);
            device_bound = false;
            device.dev.driver = NULL;
        }
        driver_private.klist_devices.k_list.count = 0;
        driver->driver.p = NULL;
        driver_registered = false;
        rollback_calls++;
        return fail_groups;
    }
    return 0;
}

static void platform_driver_unregister(struct platform_driver *driver)
{
    assert(driver_registered);
    unregister_calls++;
    if (device_bound) {
        remove_calls++;
        assert(platform_drv_remove(&device.dev) == 0);
        devres_release_all(&device.dev);
        device_bound = false;
        device.dev.driver = NULL;
    }
    driver_private.klist_devices.k_list.count = 0;
    driver->driver.p = NULL;
    driver_registered = false;
}

/* SOURCE_DEVM */
/* SOURCE_FUNCTIONS */
/* SOURCE_PLATFORM_PROBE */

static void resources_clean(void)
{
    assert(!allocations && !live_allocations && !live_devres && !device.dev.resources);
    assert(!g_pdev && !consys_pinctrl && !pEmibaseaddr);
    assert(!conn_reg.mcu_base && !conn_reg.ap_rgu_base && !conn_reg.topckgen_base && !conn_reg.spm_base);
    assert(!clk_scp_conn_main && !reg_VCN18 && !reg_VCN28 && !reg_VCN33_BT && !reg_VCN33_WIFI);
    assert(!gps_node.references && !pins_node.references);
    assert(!driver_registered && !device_bound && !pm_domain_attached);
}

static void clear_failures(void)
{
    fail_register = fail_groups = fail_clock = fail_pinctrl = 0;
    fail_resource = fail_map = fail_map_devres = fail_regulator = fail_regulator_devres = -1;
    fail_clock_devres = fail_pinctrl_devres = fail_emi_clear = fail_emi_dump = false;
    have_device = true;
    fail_framework = no_gps_phandle = no_gps_child = no_gps_properties = false;
    gps_uses_pinmux = true;
    wifi_gpio = 27;
    gConEmiPhyBase = 0x90000000;
    gConEmiSize = SZ_1M;
    device.dev.of_node = &root_node;
    map_attempts = regulator_attempts = map_devres_attempts = regulator_devres_attempts = 0;
#ifdef HOST_HAS_PROBE_GATE
    consys_ic_ops.consys_ic_probe_required = MTK_WCN_BOOL_TRUE;
#endif
}

static void healthy(void)
{
    assert(driver_registered && device_bound && g_pdev == &device);
    assert(conn_reg.mcu_base && conn_reg.ap_rgu_base && conn_reg.topckgen_base && conn_reg.spm_base);
    assert(clk_scp_conn_main && !IS_ERR(clk_scp_conn_main));
    assert(reg_VCN18 && reg_VCN28 && reg_VCN33_BT && reg_VCN33_WIFI && pEmibaseaddr);
    assert(!gps_node.references && !pins_node.references);
}

static bool is_case(const char *name, const char *expected) { return strcmp(name, expected) == 0; }
static int case_index(const char *name, const char *prefix)
{
    size_t size = strlen(prefix);
    return strncmp(name, prefix, size) == 0 ? atoi(name + size) : -1;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *name = argv[1];
    int expected = 0, index, result;
    clear_failures();
    if ((index = case_index(name, "missing-register-resource-")) >= 0) {
        fail_resource = index; expected = -EINVAL;
    } else if ((index = case_index(name, "register-devres-allocation-")) >= 0) {
        fail_map_devres = index; expected = -ENOMEM;
    } else if ((index = case_index(name, "register-ioremap-failure-")) >= 0) {
        fail_map = index; expected = -ENOMEM;
    } else if ((index = case_index(name, "regulator-devres-allocation-")) >= 0) {
        fail_regulator_devres = index; expected = -ENOMEM;
    } else if ((index = case_index(name, "regulator-provider-failure-")) >= 0) {
        fail_regulator = index; expected = -EPROBE_DEFER;
    } else if (is_case(name, "driver-register-enomem")) {
        expected = fail_register = -ENOMEM;
    } else if (is_case(name, "driver-register-ebusy")) {
        expected = fail_register = -EBUSY;
    } else if (is_case(name, "driver-groups-rollback")) {
        expected = fail_groups = -ENOMEM;
    } else if (is_case(name, "driver-groups-rollback-after-probe-failure")) {
        expected = fail_groups = -EIO; fail_clock = -ENOENT;
    } else if (is_case(name, "no-matching-device")) {
        have_device = false; expected = -ENODEV;
    } else if (is_case(name, "probe-missing-device-node")) {
        device.dev.of_node = NULL; expected = -ENODEV;
    } else if (is_case(name, "clock-devres-allocation")) {
        fail_clock_devres = true; expected = -ENOMEM;
    } else if (is_case(name, "clock-provider-enoent")) {
        expected = fail_clock = -ENOENT;
    } else if (is_case(name, "clock-provider-deferred")) {
        expected = fail_clock = -EPROBE_DEFER;
    } else if (is_case(name, "emi-base-missing")) {
        gConEmiPhyBase = 0; expected = -ENODEV;
    } else if (is_case(name, "emi-size-zero")) {
        gConEmiSize = 0; expected = -EINVAL;
    } else if (is_case(name, "emi-clear-map-failure")) {
        fail_emi_clear = true; expected = -ENOMEM;
    } else if (is_case(name, "coredump-map-failure")) {
        fail_emi_dump = true; expected = -ENOMEM;
    } else if (is_case(name, "coredump-region-too-small")) {
        gConEmiSize = CONSYS_EMI_COREDUMP_OFFSET; expected = -EINVAL;
    } else if (is_case(name, "pinctrl-devres-allocation")) {
        fail_pinctrl_devres = true; expected = -ENOMEM;
    } else if (is_case(name, "pinctrl-provider-enomem")) {
        expected = fail_pinctrl = -ENOMEM;
    } else if (is_case(name, "pinctrl-provider-deferred")) {
        expected = fail_pinctrl = -EPROBE_DEFER;
    } else if (is_case(name, "optional-pinctrl-absent")) {
        fail_pinctrl = -ENODEV;
    } else if (is_case(name, "gps-pins-fallback")) {
        gps_uses_pinmux = false;
    } else if (is_case(name, "gps-phandle-absent")) {
        no_gps_phandle = true;
    } else if (is_case(name, "gps-child-absent")) {
        no_gps_child = true;
    } else if (is_case(name, "gps-properties-absent")) {
        no_gps_properties = true;
    } else if (is_case(name, "wifi-gpio-provider-deferred")) {
        expected = wifi_gpio = -EPROBE_DEFER;
    } else if (is_case(name, "optional-wifi-gpio-absent")) {
        wifi_gpio = -ENOENT;
    } else if (is_case(name, "framework-failure-before-probe")) {
        fail_framework = true; expected = -ENODEV;
    } else if (is_case(name, "legacy-ops-allow-unbound-registration")) {
        have_device = false;
#ifdef HOST_HAS_PROBE_GATE
        consys_ic_ops.consys_ic_probe_required = MTK_WCN_BOOL_FALSE;
#endif
    }

    if (is_case(name, "remove-before-probe")) {
        assert(mtk_wmt_remove(&device) == 0);
        assert(mtk_wcn_consys_hw_deinit() == 0);
        assert(!register_calls && !unregister_calls);
    } else if (is_case(name, "probe-null-device")) {
        wmt_consys_ic_ops = mtk_wcn_get_consys_ic_ops();
        assert(mtk_wmt_probe(NULL) == -EINVAL);
    } else {
        result = mtk_wcn_consys_hw_init();
        fprintf(stderr, "init_ret=%d expected=%d register=%u unregister=%u rollback=%u probe=%u remove=%u\n",
                result, expected, register_calls, unregister_calls, rollback_calls, probe_calls, remove_calls);
        assert(result == expected);
#ifdef HOST_HAS_PROBE_GATE
        if (consys_ic_ops.consys_ic_probe_required) {
            assert(mtk_wmt_dev_drv.prevent_deferred_probe);
            assert(mtk_wmt_dev_drv.driver.suppress_bind_attrs);
            assert(!mtk_wmt_dev_drv.probe);
            assert(mtk_wmt_dev_drv.driver.probe(&device.dev) == -ENXIO);
        }
#endif
        if (!result && have_device && !fail_framework) {
            healthy();
            if (is_case(name, "gps-properties-absent") || no_gps_child || no_gps_phandle || fail_pinctrl)
                assert(gps_lna_pin_num == 0xffffffff);
            else
                assert(gps_lna_pin_num == 27);
            if (is_case(name, "duplicate-hardware-init")) {
                assert(mtk_wcn_consys_hw_init() == -EBUSY);
                assert(register_calls == 1 && probe_calls == 1);
                healthy();
            }
            if (is_case(name, "coredump-restore-reuses-mapping")) {
                unsigned int mappings = dump_mappings;
                void *saved = pEmibaseaddr;
                assert(mtk_wcn_consys_hw_restore(&device.dev) == 0);
                assert(mtk_wcn_consys_hw_restore(&device.dev) == 0);
                assert(pEmibaseaddr == saved && dump_mappings == mappings);
            }
            if (is_case(name, "coredump-unmap-is-idempotent")) {
                assert(consys_emi_coredump_remapping(&pEmibaseaddr, 0) == 0);
                assert(consys_emi_coredump_remapping(&pEmibaseaddr, 0) == 0);
            }
        } else if (!result) {
            assert(driver_registered && !device_bound && !g_pdev);
        } else {
            resources_clean();
            if (fail_register || fail_groups)
                assert(unregister_calls == 0);
            else
                assert(unregister_calls == 1);
        }
        assert(mtk_wcn_consys_hw_deinit() == 0);
        unsigned int unregisters = unregister_calls;
        assert(mtk_wcn_consys_hw_deinit() == 0);
        assert(unregister_calls == unregisters);
    }
    resources_clean();
    /* Every injected failure must allow a fresh, complete successful cycle. */
    clear_failures();
    assert(mtk_wcn_consys_hw_init() == 0);
    healthy();
    assert(mtk_wcn_consys_hw_deinit() == 0);
    resources_clean();
    assert(gps_lna_pin_num == 0xffffffff && wifi_ant_swap_gpio_pin_num == -1);
    unsigned int unregisters = unregister_calls;
    assert(mtk_wcn_consys_hw_deinit() == 0);
    assert(unregister_calls == unregisters);
    printf("PASS: register=%u unregister=%u rollback=%u probe=%u remove=%u release_checks=%u\n",
           register_calls, unregister_calls, rollback_calls, probe_calls, remove_calls, release_checks);
    return 0;
}
