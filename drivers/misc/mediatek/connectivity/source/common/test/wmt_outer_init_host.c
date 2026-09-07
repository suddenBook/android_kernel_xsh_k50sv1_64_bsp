/* Kernel resource substitutes; the runner inserts unmodified WMT functions. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef int INT32;
typedef bool MTK_WCN_BOOL;
typedef void VOID;
typedef struct { bool initialized; } wait_queue_head_t;
typedef struct { int unused; } OSAL_SLEEPABLE_LOCK;
typedef OSAL_SLEEPABLE_LOCK OSAL_UNSLEEPABLE_LOCK;
typedef enum { WMT_CHIP_TYPE_SOC, WMT_CHIP_TYPE_COMBO } ENUM_WMT_CHIP_TYPE;
struct cdev { void *owner; };
struct class { int unused; };
struct device { int unused; };
struct notifier_block { void (*notifier_call)(void); };
struct work_struct { void (*callback)(struct work_struct *); bool pending, running; };
struct workqueue_struct { struct work_struct *pending[2]; unsigned int count; };
struct proc_dir_entry { int unused; };
struct file_operations { void *owner; void (*read)(void), (*write)(void); };
#define WMT_DEV_MAJOR 190
#define WMT_DEV_NUM 1
#define WMT_DRIVER_NAME "mtk_stp_wmt"
#define CFG_WMT_DBG_SUPPORT 1
#define CFG_WMT_PROC_FOR_AEE 1
#define CFG_WMT_PROC_FOR_DUMP_INFO 1
#define CFG_WMT_PS_SUPPORT 1
#define WMT_DBG_PROCNAME "driver/wmt_dbg"
#define WMTDRV_TYPE_WMT 4
#define WMTDRV_TYPE_LPBK 5
#define MTK_WCN_BOOL_FALSE false
#define WQ_UNBOUND 1
#define GFP_KERNEL 0
#define kmalloc(size, flags) malloc(size)
#define kvfree free
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define THIS_MODULE ((void *)1)
#define MKDEV(major, minor) ((dev_t)(((unsigned int)(major) << 20) | (minor)))
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((long)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define WMT_DBG_FUNC(...) ((void)0)
#define WMT_ERR_FUNC(...) ((void)0)
#define WMT_INFO_FUNC(...) ((void)snprintf(NULL, 0, __VA_ARGS__))
#define INIT_WORK(work, handler) do { \
    (work)->callback = (handler); (work)->pending = false; (work)->running = false; \
} while (0)

/* GLOBALS */

static const int gWmtFops;
static struct class *wmt_class;
static struct device *wmt_dev;
static OSAL_UNSLEEPABLE_LOCK g_temp_query_spinlock;
static OSAL_SLEEPABLE_LOCK g_aee_read_lock, g_dump_info_read_lock;
static OSAL_SLEEPABLE_LOCK g_dbg_emi_lock;
static struct proc_dir_entry *gWmtDbgEntry;
static struct workqueue_struct *g_wmt_dbg_wq;
static struct workqueue_struct system_workqueue;
#ifndef CONFIG_EARLYSUSPEND
static struct notifier_block wmt_fb_notifier;
#else
static int wmt_early_suspend_handler;
#endif
static struct work_struct gPwrOnOffWork;

enum resource {
    HIF, STP, REGION, CDEV, CLASS, DEVICE, LIBRARY, DEBUG, AEE, DUMP, STEP,
    TEMP_LOCK, AEE_LOCK, DUMP_LOCK, EMI_LOCK, DEBUG_QUEUE, THERMAL_CB, ASSERT_CB, SOCKET, DISPLAY,
    UART, SDIO, RESOURCE_COUNT
};
static const char *const resource_name[] = {
    "hif", "stp", "region", "cdev", "class", "device", "library", "debug", "aee", "dump", "step",
    "temp-lock", "aee-lock", "dump-lock", "emi-lock", "debug-queue", "thermal-callback", "assert-callback", "socket", "display",
    "uart", "sdio"
};
static void *live[RESOURCE_COUNT];
static unsigned int attempts[RESOURCE_COUNT], releases[RESOURCE_COUNT];
static enum resource released[128];
static unsigned int release_count, wake_count, library_cleanup_calls, combo_callback_calls;
static bool library_attempted, library_prepared, library_ready, last_producer_work;
static unsigned int cancel_count, completed_work, poweroff_calls;
static unsigned int debug_commands;
static bool pending_debug_commands;
static int poweroff_result;
static ENUM_WMT_CHIP_TYPE chip_type = WMT_CHIP_TYPE_SOC;
static int fault;
static const int failure_code[] = {0, -ENODEV, -3, -EBUSY, -ENOMEM, -ENOMEM, -ENXIO, -4};

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s (fault=%d status=%d)\n", \
            __LINE__, #condition, fault, gWmtInitStatus); exit(1); \
} } while (0)

static void *acquire(enum resource resource)
{
    REQUIRE(!live[resource]);
    live[resource] = calloc(1, 32);
    REQUIRE(live[resource]);
    return live[resource];
}

static void release(enum resource resource)
{
    if (!live[resource])
        fprintf(stderr, "cleanup without ownership: %s\n", resource_name[resource]);
    REQUIRE(live[resource]);
    REQUIRE(release_count < sizeof(released) / sizeof(released[0]));
    released[release_count++] = resource;
    releases[resource]++;
    free(live[resource]);
    live[resource] = NULL;
}

static int constructor(enum resource resource, int stage)
{
    attempts[resource]++;
    REQUIRE(!live[resource]);
    if (fault == stage)
        return failure_code[stage];
    acquire(resource);
    return 0;
}

static int mtk_wcn_hif_sdio_drv_init(void) { return constructor(HIF, 1); }
static int stp_drv_init(void)
{
    REQUIRE(live[HIF]);
    return constructor(STP, 2);
}
static void mtk_wcn_hif_sdio_driver_exit(void)
{
    REQUIRE(!live[STP]);
    release(HIF);
}
static void stp_drv_exit(void)
{
    REQUIRE(!library_attempted || (library_prepared && live[LIBRARY]));
    REQUIRE(!live[UART] && !live[SDIO]);
    REQUIRE(!live[DEBUG] && !live[AEE] && !live[DUMP]);
    REQUIRE(!live[STEP]);
    release(STP);
}
static int register_chrdev_region(dev_t dev, int count, const char *name)
{
    REQUIRE(live[STP]);
    REQUIRE(dev == MKDEV(WMT_DEV_MAJOR, 0));
    return constructor(REGION, 3);
}
static void unregister_chrdev_region(dev_t dev, int count)
{
    REQUIRE(dev == MKDEV(WMT_DEV_MAJOR, 0));
    REQUIRE(!live[CDEV]);
    release(REGION);
}
static void cdev_init(struct cdev *dev, const int *fops) { REQUIRE(live[REGION]); }
static int cdev_add(struct cdev *dev, dev_t number, int count)
{
    REQUIRE(live[REGION]);
    return constructor(CDEV, 4);
}
static void cdev_del(struct cdev *dev)
{
    REQUIRE(!live[CLASS] && !live[DEVICE]);
    release(CDEV);
}
static struct class *class_create(void *owner, const char *name)
{
    int result;

    REQUIRE(live[CDEV]);
    result = constructor(CLASS, 5);
    return result ? ERR_PTR(result) : live[CLASS];
}
static void class_destroy(struct class *class_)
{
    REQUIRE(class_ == live[CLASS]);
    REQUIRE(!live[DEVICE]);
    release(CLASS);
}
static struct device *device_create(struct class *class_, void *parent, dev_t dev,
                                   void *data, const char *name)
{
    int result;

    REQUIRE(live[CLASS] && class_ == live[CLASS]);
    result = constructor(DEVICE, 6);
    return result ? ERR_PTR(result) : live[DEVICE];
}
static void device_destroy(struct class *class_, dev_t dev)
{
    REQUIRE(live[CLASS] && class_ == live[CLASS]);
    REQUIRE(!live[LIBRARY]);
    release(DEVICE);
}

/* A failed library constructor still requires its own partial-init cleanup. */
static int wmt_lib_init(void)
{
    REQUIRE(live[STP] && live[CDEV]);
    REQUIRE(!WMT_CREATE_NODE_DYNAMIC || live[DEVICE]);
    REQUIRE(!library_attempted);
    library_attempted = true;
    attempts[LIBRARY]++;
    acquire(LIBRARY);
    library_ready = fault != 7;
    return fault == 7 ? failure_code[7] : 0;
}
static int wmt_lib_deinit_prepare(void)
{
    REQUIRE(library_attempted);
    REQUIRE(!library_prepared);
    REQUIRE(!live[DEBUG] && !live[AEE] && !live[DUMP] && !live[STEP]);
    REQUIRE(!gPwrOnOffWork.pending && !gPwrOnOffWork.running);
    if (library_ready)
        REQUIRE(poweroff_calls);
    library_prepared = true;
    return 0;
}
static int wmt_lib_deinit_finish(void)
{
    REQUIRE(library_prepared && !live[STP]);
    if (live[ASSERT_CB])
        release(ASSERT_CB);
    release(LIBRARY);
    library_attempted = library_prepared = library_ready = false;
    library_cleanup_calls++;
    return 0;
}
/* The old API remains only so the same fixture can reject baseline source. */
static void wmt_lib_deinit(void)
{
    REQUIRE(!live[DEBUG] && !live[AEE] && !live[DUMP]);
    REQUIRE(!gPwrOnOffWork.pending && !gPwrOnOffWork.running);
    if (live[ASSERT_CB])
        release(ASSERT_CB);
    release(LIBRARY);
    library_attempted = library_ready = false;
    library_cleanup_calls++;
#if LEGACY_LIBRARY_STEP_CLEANUP
    release(STEP);
#endif
}

static int wmt_lib_power_off_for_exit(void)
{
    REQUIRE(live[LIBRARY] && live[STP] && !library_prepared);
    REQUIRE(!live[DISPLAY] && !live[DEBUG] && !live[AEE] && !live[DUMP]);
    REQUIRE(!live[SOCKET] && !live[STEP]);
    REQUIRE(!live[DEBUG_QUEUE] && !system_workqueue.count);
    REQUIRE(debug_commands == (pending_debug_commands ? 2u : 0u));
    REQUIRE(!gPwrOnOffWork.pending && !gPwrOnOffWork.running);
    poweroff_calls++;
    return poweroff_result;
}

static void init_waitqueue_head(wait_queue_head_t *wait) { wait->initialized = true; }
static void wake_up(wait_queue_head_t *wait)
{
    REQUIRE(wait->initialized && gWmtInitStatus == WMT_INIT_DONE);
    REQUIRE(live[STP] && live[CDEV] && live[LIBRARY]);
    REQUIRE(gPwrOnOffWork.callback && live[TEMP_LOCK] && live[AEE_LOCK] && live[DUMP_LOCK]);
    REQUIRE(live[DISPLAY] && live[STEP]);
    wake_count++;
}
static ENUM_WMT_CHIP_TYPE wmt_detect_get_chip_type(void) { return chip_type; }
static void wmt_dbg_read(void) {}
static void wmt_dbg_write(void) {}
static struct proc_dir_entry *proc_create(const char *name, int mode, void *parent,
                                         const struct file_operations *fops)
{
    REQUIRE(live[EMI_LOCK] && live[DEBUG_QUEUE] && live[STEP] && gPwrOnOffWork.callback);
    REQUIRE(live[TEMP_LOCK] && live[AEE_LOCK] && live[DUMP_LOCK]);
    return acquire(DEBUG);
}
static void proc_remove(struct proc_dir_entry *entry)
{
    REQUIRE(entry == live[DEBUG] && live[EMI_LOCK] && live[LIBRARY]);
    if (last_producer_work)
        gPwrOnOffWork.pending = true;
    release(DEBUG);
}
static int wmt_lib_ps_deinit(void) { return 0; }
static struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags, int max_active)
{
    REQUIRE(flags == WQ_UNBOUND && live[EMI_LOCK] && !live[DEBUG]);
    return acquire(DEBUG_QUEUE);
}
static bool queue_work(struct workqueue_struct *queue, struct work_struct *work)
{
    REQUIRE(queue && queue->count < 2 && work->callback && !work->pending);
    queue->pending[queue->count++] = work;
    work->pending = true;
    return true;
}
/* A baseline async request lands here and is deliberately outside private drain. */
static bool schedule_work(struct work_struct *work) { return queue_work(&system_workqueue, work); }
static void destroy_workqueue(struct workqueue_struct *queue)
{
    unsigned int i;

    REQUIRE(queue == live[DEBUG_QUEUE] && !live[DEBUG]);
    for (i = 0; i < queue->count; i++) {
        struct work_struct *work = queue->pending[i];

        work->pending = false;
        work->callback(work);
    }
    release(DEBUG_QUEUE);
}
static MTK_WCN_BOOL mtk_wcn_wmt_func_on(INT32 type)
{
    REQUIRE(type == WMTDRV_TYPE_LPBK && live[LIBRARY] && live[STP] && live[STEP]);
    REQUIRE(!library_prepared && !poweroff_calls);
    debug_commands++;
    return true;
}
static MTK_WCN_BOOL mtk_wcn_wmt_func_off(INT32 type) { return mtk_wcn_wmt_func_on(type); }
static INT32 wmt_dbg_func_ctrl(INT32 x, INT32 y, INT32 z);
static INT32 (*const wmt_dev_dbg_func[8])(INT32, INT32, INT32) = { [7] = wmt_dbg_func_ctrl };
static void wmt_dev_proc_for_aee_setup(void) { REQUIRE(live[AEE_LOCK]); acquire(AEE); }
static void wmt_dev_proc_for_aee_remove(void) { release(AEE); }
static void wmt_dev_proc_for_dump_info_setup(void) { REQUIRE(live[DUMP_LOCK]); acquire(DUMP); }
static void wmt_dev_proc_for_dump_info_remove(void) { release(DUMP); }
#define WMT_STEP_INIT_FUNC() acquire(STEP)
#define WMT_STEP_DEINIT_FUNC() release(STEP)
static void wmt_dev_tra_sdio_update(void) {}
static void mtk_wcn_hif_sdio_update_cb_reg(void (*callback)(void))
{
    REQUIRE(chip_type == WMT_CHIP_TYPE_COMBO && live[HIF]);
    combo_callback_calls++;
}
static enum resource lock_resource(OSAL_SLEEPABLE_LOCK *lock)
{
    if (lock == &g_temp_query_spinlock)
        return TEMP_LOCK;
    if (lock == &g_aee_read_lock)
        return AEE_LOCK;
    if (lock == &g_dbg_emi_lock)
        return EMI_LOCK;
    REQUIRE(lock == &g_dump_info_read_lock);
    return DUMP_LOCK;
}
static void osal_sleepable_lock_init(OSAL_SLEEPABLE_LOCK *lock) { acquire(lock_resource(lock)); }
static void osal_sleepable_lock_deinit(OSAL_SLEEPABLE_LOCK *lock)
{
    enum resource resource = lock_resource(lock);

    if (resource == EMI_LOCK)
        REQUIRE(!live[DEBUG] && !live[DEBUG_QUEUE]);
    else
        REQUIRE(!live[AEE] && !live[DUMP] && !live[LIBRARY]);
    release(resource);
}
#define osal_unsleepable_lock_init osal_sleepable_lock_init
#define osal_unsleepable_lock_deinit osal_sleepable_lock_deinit
static void wmt_dev_tm_temp_query(void) {}
static void wmt_lib_trigger_assert(void) {}
static void wmt_lib_register_thermal_ctrl_cb(void (*callback)(void))
{
    if (callback)
        acquire(THERMAL_CB);
    else
        release(THERMAL_CB);
}
static void wmt_lib_register_trigger_assert_cb(void (*callback)(void)) { acquire(ASSERT_CB); }
static void wmt_dev_bgw_desense_init(void) { acquire(SOCKET); }
static void wmt_dev_bgw_desense_deinit(void)
{
    if (live[SOCKET])
        release(SOCKET);
}
static void wmt_pwr_on_off_handler(struct work_struct *work)
{
    REQUIRE(live[LIBRARY] && live[STP] && !library_prepared && live[TEMP_LOCK]);
    completed_work++;
}
static void cancel_work_sync(struct work_struct *work)
{
    REQUIRE(work == &gPwrOnOffWork && work->callback);
    REQUIRE(!live[DISPLAY] && !live[DEBUG] && !live[SOCKET]);
    /* Complete a held running callback and discard its last queued publication. */
    if (work->running)
        work->callback(work);
    work->pending = work->running = false;
    cancel_count++;
}
static void wmt_fb_notifier_callback(void) {}
static int fb_register_client(struct notifier_block *notifier) { acquire(DISPLAY); return 0; }
static void remove_display(void)
{
    if (last_producer_work)
        gPwrOnOffWork.running = true;
    release(DISPLAY);
}
static void fb_unregister_client(struct notifier_block *notifier) { remove_display(); }
static void register_early_suspend(int *handler) { acquire(DISPLAY); }
static void unregister_early_suspend(int *handler) { remove_display(); }
static void mtk_wcn_stp_uart_drv_init(void) { acquire(UART); }
static void mtk_wcn_stp_sdio_drv_init(void) { acquire(SDIO); }
static void mtk_wcn_stp_uart_drv_exit(void) { release(UART); }
static void mtk_wcn_stp_sdio_drv_exit(void) { release(SDIO); }
static void wmt_dev_patch_info_free(void) {}
/* The concurrent callback barrier is exercised by test_wmt_callback_lifetime.py. */
static void wmt_export_platform_bridge_unregister(void) {}

/* PRODUCTION */

static bool stage_enabled(int stage)
{
    return !(stage == 1 && MTK_WCN_REMOVE_KO)
        && !(stage >= 5 && stage <= 6 && !WMT_CREATE_NODE_DYNAMIC);
}

static void assert_no_owned_resources(bool external_hif)
{
    int i;

    for (i = 0; i < RESOURCE_COUNT; i++) {
        if (i == HIF && external_hif) {
            REQUIRE(live[i]);
            continue;
        }
        if (live[i])
            fprintf(stderr, "retained resource: %s\n", resource_name[i]);
        REQUIRE(!live[i]);
    }
    REQUIRE(!library_attempted);
    REQUIRE(!wmt_class && !wmt_dev);
}

static void assert_success(void)
{
    int i;

    REQUIRE(gWmtInitStatus == WMT_INIT_DONE && wake_count == 1);
    REQUIRE(gWmtMajor == WMT_DEV_MAJOR);
    for (i = 0; i < RESOURCE_COUNT; i++) {
        bool expected = true;

        if (i == CLASS || i == DEVICE)
            expected = WMT_CREATE_NODE_DYNAMIC;
        if (i == SOCKET)
            expected = chip_type == WMT_CHIP_TYPE_SOC;
        if (i == UART || i == SDIO)
            expected = !MTK_WCN_REMOVE_KO;
        REQUIRE(!!live[i] == expected);
    }
    REQUIRE(combo_callback_calls == (unsigned int)(chip_type == WMT_CHIP_TYPE_COMBO));
}

static void finish_success(void)
{
    unsigned int prior_cleanup = library_cleanup_calls;
    unsigned int prior_release;

    /* do_common_drv_init owns these constructors in the external variant. */
    if (MTK_WCN_REMOVE_KO) {
        mtk_wcn_stp_uart_drv_init();
        mtk_wcn_stp_sdio_drv_init();
    }
    WMT_exit();
    REQUIRE(gWmtInitStatus == WMT_INIT_NOT_START);
    REQUIRE(library_cleanup_calls == prior_cleanup + 1);
    REQUIRE(cancel_count == 1 && poweroff_calls == 1);
    REQUIRE(completed_work == (unsigned int)last_producer_work);
    assert_no_owned_resources(false);
    prior_release = release_count;
    WMT_exit();
    REQUIRE(release_count == prior_release);
}

static void fail_once(int stage)
{
    unsigned int prior_cleanup = library_cleanup_calls;
    unsigned int first_release = release_count;
    unsigned int prior_attempts[RESOURCE_COUNT];
    enum resource expected[7];
    unsigned int count = 0, i;
    int resource;

    REQUIRE(stage_enabled(stage));
    memcpy(prior_attempts, attempts, sizeof(attempts));
    fault = stage;
    REQUIRE(WMT_init() == failure_code[stage]);
    REQUIRE(gWmtInitStatus == WMT_INIT_NOT_START && !wake_count);
    REQUIRE(gWmtMajor == WMT_DEV_MAJOR);
    REQUIRE(library_cleanup_calls == prior_cleanup + (stage == 7));
    assert_no_owned_resources(MTK_WCN_REMOVE_KO);
    for (resource = HIF; resource <= LIBRARY; resource++) {
        bool should_attempt = resource < stage;

        if ((resource == HIF && MTK_WCN_REMOVE_KO)
            || ((resource == CLASS || resource == DEVICE) && !WMT_CREATE_NODE_DYNAMIC))
            should_attempt = false;
        REQUIRE(attempts[resource] == prior_attempts[resource] + should_attempt);
    }
    if (stage == 7) {
        expected[count++] = STP;
        expected[count++] = LIBRARY;
    }
    if (stage > 6 && WMT_CREATE_NODE_DYNAMIC)
        expected[count++] = DEVICE;
    if (stage > 5 && WMT_CREATE_NODE_DYNAMIC)
        expected[count++] = CLASS;
    if (stage > 4)
        expected[count++] = CDEV;
    if (stage > 3)
        expected[count++] = REGION;
    if (stage > 2 && stage != 7)
        expected[count++] = STP;
    if (stage > 1 && !MTK_WCN_REMOVE_KO)
        expected[count++] = HIF;
    REQUIRE(release_count - first_release == count);
    for (i = 0; i < count; i++)
        REQUIRE(released[first_release + i] == expected[i]);
    first_release = release_count;
    WMT_exit();
    REQUIRE(release_count == first_release);
    fault = 0;
}

int main(int argc, char **argv)
{
    int scenario, stage;
    unsigned int prior_attempts[RESOURCE_COUNT];

    REQUIRE(argc == 2);
    scenario = atoi(argv[1]);
    if (scenario == 3) {
        WMT_exit();
        REQUIRE(!release_count && !wake_count);
        assert_no_owned_resources(false);
        return 0;
    }
    if (MTK_WCN_REMOVE_KO)
        acquire(HIF);
    if (scenario == 1)
        chip_type = WMT_CHIP_TYPE_COMBO;
    if (scenario == 28)
        last_producer_work = true;
    if (scenario == 29)
        poweroff_result = -EIO;
    if (scenario == 30)
        pending_debug_commands = true;
    if (scenario >= 10 && scenario <= 16) {
        fail_once(scenario - 9);
        if (MTK_WCN_REMOVE_KO)
            mtk_wcn_hif_sdio_driver_exit();
        return 0;
    }
    if (scenario >= 20 && scenario <= 26)
        fail_once(scenario - 19);
    if (scenario == 27) {
        for (stage = 1; stage <= 7; stage++) {
            if (stage_enabled(stage))
                fail_once(stage);
        }
    }
    REQUIRE(WMT_init() == 0);
    assert_success();
    if (pending_debug_commands) {
        wmt_dbg_delay_work(7, WMTDRV_TYPE_LPBK, 1);
        wmt_dbg_delay_work(7, WMTDRV_TYPE_LPBK, 0);
        REQUIRE(debug_commands == 0);
    }
    if (scenario == 2) {
        memcpy(prior_attempts, attempts, sizeof(attempts));
        REQUIRE(WMT_init() == 0);
        REQUIRE(memcmp(prior_attempts, attempts, sizeof(attempts)) == 0);
        assert_success();
    }
    finish_success();
    return 0;
}
