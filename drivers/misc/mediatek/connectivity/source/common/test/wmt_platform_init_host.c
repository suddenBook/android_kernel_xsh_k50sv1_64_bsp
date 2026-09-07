/* Hardware-free adapters surrounding complete extracted production functions. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void VOID;
typedef int INT32;
typedef unsigned int UINT32;
typedef int ENUM_WMT_CHIP_TYPE;
#define WMT_CHIP_TYPE_SOC 1
#define WMT_CHIP_TYPE_COMBO 0
#define CFG_WMT_WAKELOCK_SUPPORT 1
#define WMT_DBG_FUNC(...) ((void)0)
#define WMT_WARN_FUNC(...) ((void)0)
#define WMT_ERR_FUNC(...) ((void)0)
#define WMT_PLAT_PR_ERR(...) ((void)0)
#define CMB_STUB_LOG_PR_WARN(...) ((void)0)
#define CMB_STUB_LOG_PR_INFO(...) ((void)0)
#define CMB_STUB_LOG_PR_DBG(...) ((void)0)
#define CONNADP_INFO_FUNC(...) ((void)0)
#define CONNADP_DBG_FUNC(...) ((void)0)
#define CONNADP_WARN_FUNC(...) ((void)0)
#define EXPORT_SYMBOL(symbol)
#define pr_info(...) ((void)0)
#define unlikely(value) (value)
#define osal_strcpy strcpy
#define osal_sizeof sizeof
#define osal_memcpy memcpy

/* SOURCE_TYPES */
/* SOURCE_CONSTANTS */
struct wakeup_source { int marker; };
typedef struct {
    char name[32];
    int init_flag;
    struct wakeup_source *wake_lock;
} OSAL_WAKE_LOCK, *P_OSAL_WAKE_LOCK;
struct mutex { bool initialized, locked; };
typedef struct { struct mutex lock; } OSAL_SLEEPABLE_LOCK, *P_OSAL_SLEEPABLE_LOCK;
typedef struct {
    UINT32 ldoStableTime, rtcStableTime, offStableTime, onStableTime, rstStableTime;
} PWR_SEQ_TIME, *P_PWR_SEQ_TIME;
static PWR_SEQ_TIME gPwrSeqTime;
static OSAL_SLEEPABLE_LOCK gOsSLock;
static OSAL_WAKE_LOCK wmt_wake_lock;
static UINT32 gCoClockFlag;
static struct { bool lock; } g_bgf_irq_lock;
/* SOURCE_STATES */
static struct { int marker; } mtk_wmt_dev_drv;
static unsigned char *pEmibaseaddr;
struct consys_ops {
    int (*consys_ic_co_clock_type)(void);
    int (*consys_ic_emi_coredump_remapping)(unsigned char **, UINT32);
    bool consys_ic_probe_required;
};
static struct consys_ops *wmt_consys_ic_ops;
static int chip_type = WMT_CHIP_TYPE_SOC, clock_query = -1;
static int fail_wake, fail_driver, fail_stub;
static bool fail_driver_after_bind;
static int live_wakes, live_mutexes, live_maps, driver_registered;
static int driver_registers, driver_unregisters, driver_rollbacks, wake_creates, wake_destroys;
static int stub_registers, callback_invocations;
static bool require_publish_ready, require_withdraw_first;

/* Sequential constructors must reach drain with no outstanding callbacks. */
typedef atomic_int atomic_t;
#define ATOMIC_INIT(value) (value)
#define atomic_read(value) atomic_load(value)
#define atomic_inc(value) ((void)atomic_fetch_add(value, 1))
#define atomic_dec_and_test(value) (atomic_fetch_sub(value, 1) == 1)
#define DEFINE_MUTEX(name) struct mutex name = { .initialized = true }
typedef struct { bool locked; } spinlock_t;
#define DEFINE_SPINLOCK(name) spinlock_t name
struct host_wait_queue { unsigned int wakeups; };
#define DECLARE_WAIT_QUEUE_HEAD(name) struct host_wait_queue name
static bool host_irq_disabled;
static unsigned int host_spin_depth;

static void mutex_lock(struct mutex *lock)
{
    assert(!host_irq_disabled && !host_spin_depth);
    assert(lock->initialized && !lock->locked);
    lock->locked = true;
}
static void mutex_unlock(struct mutex *lock)
{
    assert(lock->initialized && lock->locked);
    lock->locked = false;
}
#define spin_lock_irqsave(lock, flags) do { \
    (flags) = host_irq_disabled; host_irq_disabled = true; \
    assert(!(lock)->locked); (lock)->locked = true; host_spin_depth++; \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
    assert((lock)->locked && host_spin_depth); host_spin_depth--; \
    (lock)->locked = false; host_irq_disabled = (flags); \
} while (0)
#define wait_event(queue, condition) do { \
    (void)&(queue); assert(!host_irq_disabled && !host_spin_depth); \
    assert(condition); \
} while (0)
static void wake_up_all(struct host_wait_queue *queue) { queue->wakeups++; }
static void dump_stack(void) {}

/* SOURCE_BRIDGE */

static bool bridge_present(void)
{
    return bridge.thermal_query_cb || bridge.trigger_assert_cb || bridge.clock_fail_dump_cb;
}
static struct wakeup_source *wakeup_source_register(const char *name)
{
    wake_creates++;
    assert(strcmp(name, "wmtFuncCtrl") == 0);
    if (fail_wake) return NULL;
    struct wakeup_source *wake = calloc(1, sizeof(*wake));
    assert(wake);
    live_wakes++;
    return wake;
}
static void wakeup_source_unregister(struct wakeup_source *wake)
{
    assert(wake && live_wakes > 0);
    live_wakes--;
    wake_destroys++;
    free(wake);
}
static void mutex_init(struct mutex *lock)
{
    assert(!lock->initialized);
    lock->initialized = true;
    live_mutexes++;
}
static void mutex_destroy(struct mutex *lock)
{
    assert(lock->initialized);
    lock->initialized = false;
    assert(live_mutexes-- > 0);
}
static void spin_lock_init(bool *lock) { *lock = true; }
static int fake_clock_query(void) { return clock_query; }
static int fake_remap(unsigned char **address, UINT32 enable)
{
    assert(!enable);
    if (*address) {
        assert(live_maps-- > 0);
        free(*address);
        *address = NULL;
    }
    return 0;
}
static struct consys_ops supported_ops = {
    .consys_ic_co_clock_type = fake_clock_query,
    .consys_ic_emi_coredump_remapping = fake_remap,
};
static struct consys_ops *mtk_wcn_get_consys_ic_ops(void) { return &supported_ops; }
static int platform_driver_register(void *driver)
{
    driver_registers++;
    if (fail_driver && !fail_driver_after_bind) return fail_driver;
    if (driver_registered) return -EBUSY;
    driver_registered = 1;
    assert(!pEmibaseaddr);
    pEmibaseaddr = calloc(1, 32);
    assert(pEmibaseaddr);
    live_maps++;
    if (fail_driver_after_bind) {
        /* driver_register removes its driver if driver_add_groups fails. */
        fake_remap(&pEmibaseaddr, 0);
        driver_registered = 0;
        driver_rollbacks++;
        return fail_driver;
    }
    return 0;
}
/* This suite uses legacy abstract ops; the MT6755 suite runs the real probe. */
static int mtk_wmt_probe(void *device)
{ assert(!"Use test_wmt_probe_lifecycle.py for the MT6755 probe"); return -ENODEV; }
static int platform_driver_probe(void *driver, int (*probe)(void *))
{ assert(!"The abstract platform ops do not require probing"); return -ENODEV; }
static void platform_driver_unregister(void *driver)
{
    if (require_withdraw_first) assert(!bridge_present());
    /* driver_unregister warns on a driver that was never registered. */
    assert(driver_registered);
    driver_registered = 0;
    driver_unregisters++;
    /* Model the remove callback's idempotent coredump-unmap helper. */
    fake_remap(&pEmibaseaddr, 0);
}
static int wmt_detect_get_chip_type(void) { return chip_type; }
static void mtk_wcn_cmb_hw_dmp_seq(void) {}
static int wmt_plat_audio_ctrl(enum CMB_STUB_AIF_X state, enum CMB_STUB_AIF_CTRL ctrl)
{ callback_invocations++; return 0; }
static void wmt_plat_func_ctrl(UINT32 type, UINT32 on) { callback_invocations++; }
static signed long wmt_plat_thermal_ctrl(void) { callback_invocations++; return 0; }
static int wmt_plat_assert_ctrl(void) { callback_invocations++; return 0; }
static int fake_reset(UINT32 type) { callback_invocations++; return 0; }
static int wmt_plat_deep_idle_ctrl(UINT32 state) { callback_invocations++; return 0; }
static void wmt_plat_clock_fail_dump(void) { callback_invocations++; }

/* SOURCE_WAKE */
/* SOURCE_STUB */
static int mtk_wcn_cmb_stub_reg(struct _CMB_STUB_CB_ *callbacks)
{
    stub_registers++;
    if (require_publish_ready) {
        assert(live_wakes == 1 && live_mutexes == 1 && g_bgf_irq_lock.lock);
        assert(chip_type != WMT_CHIP_TYPE_SOC || driver_registered);
    }
    /* Valid production input cannot fail validation; inject the API rejection. */
    if (fail_stub) return source_cmb_stub_reg(NULL);
    return source_cmb_stub_reg(callbacks);
}
/* SOURCE_HARDWARE */
/* SOURCE_PLATFORM */

static void resources_clean(void)
{
    assert(!live_wakes && !live_mutexes && !live_maps && !driver_registered);
    assert(!wmt_wake_lock.init_flag && !gOsSLock.lock.initialized && !pEmibaseaddr);
    assert(!bridge_present());
}
static void callbacks_clean(void)
{
    assert(!cmb_stub_aif_ctrl_cb && !cmb_stub_func_ctrl_cb && !cmb_stub_thermal_ctrl_cb);
    assert(!cmb_stub_trigger_assert_cb && !cmb_stub_deep_idle_ctrl_cb);
    assert(!cmb_stub_do_reset_cb && !cmb_stub_clock_fail_dump_cb);
}
static void healthy(void)
{
    assert(live_wakes == 1 && live_mutexes == 1 && wmt_wake_lock.init_flag);
    assert(bridge_present());
    assert(driver_registered == (chip_type == WMT_CHIP_TYPE_SOC));
    assert(live_maps == (chip_type == WMT_CHIP_TYPE_SOC));
}
static void successful_cycle(void)
{
    fail_wake = fail_driver = fail_stub = 0;
    fail_driver_after_bind = false;
    assert(wmt_plat_init(NULL, 0) == 0);
    healthy();
    assert(wmt_plat_deinit() == 0);
    resources_clean();
}
static void valid_callbacks(struct _CMB_STUB_CB_ *callbacks)
{
    memset(callbacks, 0, sizeof(*callbacks));
    callbacks->size = sizeof(*callbacks);
    callbacks->aif_ctrl_cb = wmt_plat_audio_ctrl;
    callbacks->func_ctrl_cb = wmt_plat_func_ctrl;
    callbacks->thermal_query_cb = wmt_plat_thermal_ctrl;
    callbacks->trigger_assert_cb = wmt_plat_assert_ctrl;
    callbacks->wmt_do_reset_cb = fake_reset;
    callbacks->deep_idle_ctrl_cb = wmt_plat_deep_idle_ctrl;
    callbacks->clock_fail_dump_cb = wmt_plat_clock_fail_dump;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    int test = atoi(argv[1]);
    struct _CMB_STUB_CB_ callbacks;
    PWR_SEQ_TIME sequence = { .ldoStableTime = 200, .rtcStableTime = 200,
                             .offStableTime = 50, .onStableTime = 50, .rstStableTime = 50 };
    if (test == 18 || test == 19) {
        valid_callbacks(&callbacks);
        if (test == 19) {
            assert(source_cmb_stub_reg(NULL) == -1);
            callbacks.size = 0;
            assert(source_cmb_stub_reg(&callbacks) == -1);
            callbacks_clean();
            assert(!bridge_present());
        } else {
            assert(source_cmb_stub_reg(&callbacks) == 0);
            wmt_bridge_trigger_assert_cb saved_dispatcher = bridge.trigger_assert_cb;
            assert(saved_dispatcher);
            assert(mtk_wcn_cmb_stub_unreg() == 0);
            assert(saved_dispatcher() == 0);
            fprintf(stderr, "post_unregister_assert_calls=%d\n", callback_invocations);
            assert(callback_invocations == 0);
            callbacks_clean();
        }
    } else if (test == 11) {
        assert(wmt_plat_deinit() == 0);
        resources_clean();
    } else {
        chip_type = test == 1 || test == 2 || test == 7 || test == 15 || test == 21 || test == 22 ? WMT_CHIP_TYPE_COMBO : WMT_CHIP_TYPE_SOC;
        fail_wake = test == 6 || test == 7;
        fail_driver = test == 8 || test == 20 ? -ENOMEM : test == 9 ? -EBUSY : 0;
        fail_stub = test == 10 || test == 22;
        fail_driver_after_bind = test == 20;
        if (test == 21) sequence.rtcStableTime = 0;
        require_publish_ready = test == 16;
        require_withdraw_first = test == 17;
        clock_query = test == 4 || test == 5 ? 2 : -1;
        int input_clock = test == 3 || test == 5 ? 3 : 0;
        int ret = wmt_plat_init(test == 2 || test == 21 || test == 22 ? &sequence : NULL, input_clock);
        int expected = fail_wake ? -ENOMEM : fail_driver ? fail_driver : fail_stub ? -1 : 0;
        fprintf(stderr, "init_result=%d expected=%d wake=%d mutex=%d driver=%d bridge=%d\n",
                ret, expected, live_wakes, live_mutexes, driver_registered, bridge_present());
        assert(ret == expected);
        if (ret) {
            /* The library skips platform deinit after an unsuccessful constructor. */
            resources_clean();
            callbacks_clean();
            if (test == 22) assert(gPwrSeqTime.ldoStableTime == DFT_LDO_STABLE_TIME);
            if (test == 20) assert(driver_rollbacks == 1 && driver_unregisters == 0);
            assert(wmt_plat_deinit() == 0);
            resources_clean();
        } else {
            healthy();
            if (test == 1 || test == 21)
                assert(gPwrSeqTime.ldoStableTime == DFT_LDO_STABLE_TIME &&
                       gPwrSeqTime.rtcStableTime == DFT_RTC_STABLE_TIME);
            if (test == 2) assert(memcmp(&gPwrSeqTime, &sequence, sizeof(sequence)) == 0);
            if (test == 3 || test == 5) assert(wmt_plat_soc_co_clock_flag_get() == 3);
            if (test == 4) assert(wmt_plat_soc_co_clock_flag_get() == 2);
            if (test == 13) {
                int old_registers = driver_registers, old_wakes = wake_creates;
                assert(wmt_plat_init(NULL, 0) == -EBUSY);
                assert(driver_registers == old_registers && wake_creates == old_wakes);
                healthy();
            }
            if (test == 14) chip_type = WMT_CHIP_TYPE_COMBO;
            if (test == 15) chip_type = WMT_CHIP_TYPE_SOC;
            assert(wmt_plat_deinit() == 0);
            resources_clean();
            if (test == 1 || test == 2 || test == 15)
                assert(gPwrSeqTime.ldoStableTime == DFT_LDO_STABLE_TIME);
            if (test == 12) { assert(wmt_plat_deinit() == 0); resources_clean(); }
        }
    }
    /* Each scenario ends with a successful retry/reinitialization cycle. */
    successful_cycle();
    printf("PASS: wake=%d/%d driver=%d/%d stub=%d\n", wake_creates, wake_destroys,
           driver_registers, driver_unregisters, stub_registers);
    return 0;
}
