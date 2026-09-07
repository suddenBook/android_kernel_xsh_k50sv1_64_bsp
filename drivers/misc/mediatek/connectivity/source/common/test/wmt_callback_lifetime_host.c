/* Host synchronization and hardware adapters; production bodies are inserted whole. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

#define NO_INSTRUMENT __attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *, void *) NO_INSTRUMENT;
void __cyg_profile_func_exit(void *, void *) NO_INSTRUMENT;

typedef void VOID;
typedef int INT32;
typedef int8_t INT8;
typedef unsigned int UINT32;
typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned char *PUINT8;
typedef int ENUM_WMT_CHIP_TYPE;
typedef atomic_int atomic_t;
#define ATOMIC_INIT(value) (value)
#define atomic_read(value) atomic_load(value)
#define atomic_inc(value) ((void)atomic_fetch_add(value, 1))
#define atomic_dec_and_test(value) (atomic_fetch_sub(value, 1) == 1)
#define unlikely(value) (value)
#define EXPORT_SYMBOL(value)
#define CONNADP_INFO_FUNC(...) ((void)0)
#define CONNADP_DBG_FUNC(...) ((void)0)
#define CONNADP_WARN_FUNC(...) ((void)0)
#define CMB_STUB_LOG_PR_WARN(...) ((void)0)
#define CMB_STUB_LOG_PR_INFO(...) ((void)0)
#define CMB_STUB_LOG_PR_DBG(...) ((void)0)
#define WMT_INFO_FUNC(...) ((void)0)
#define WMT_WARN_FUNC(...) ((void)0)
#define WMT_DBG_FUNC(...) ((void)0)
#define WMT_ERR_FUNC(...) ((void)0)
#define WMT_CHIP_TYPE_SOC 1
#define WMT_CHIP_TYPE_COMBO 0
#define WMT_DEV_MAJOR 190
#define WMT_DEV_NUM 1
#define WMT_OP_BUF_SIZE 16
#define CFG_WMT_WAKELOCK_SUPPORT 1
#define CFG_WMT_PS_SUPPORT 1
#define CFG_WMT_LTE_COEX_HANDLING 1
#define CFG_WMT_DBG_SUPPORT 1
#define CFG_WMT_PROC_FOR_AEE 1
#define CFG_WMT_PROC_FOR_DUMP_INFO 1
#define WMT_CREATE_NODE_DYNAMIC 1
#define STEP_TRIGGER_POINT_WHEN_CLOCK_FAIL 1
#define MKDEV(major, minor) ((dev_t)(((unsigned int)(major) << 20) | (minor)))
#define osal_memcpy memcpy
#define osal_free free

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

static pthread_mutex_t progress_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progress_cond = PTHREAD_COND_INITIALIZER;
static atomic_int module_callbacks, blocked_drains, blocked_writers;
static atomic_int cleanup_calls, thermal_commands, assertion_calls, clock_calls;
static atomic_int operation_references, replacement_calls;
static atomic_bool cleanup_while_callback;
static atomic_bool pause_on_entry[3], pause_on_return[3], arrived[3], released[3];
static bool pause_thermal_command, nested_assertion;
static _Thread_local unsigned int spin_depth;
static _Thread_local bool irq_disabled;

static void progress(void)
{
    CHECK(pthread_mutex_lock(&progress_lock) == 0);
    CHECK(pthread_cond_broadcast(&progress_cond) == 0);
    CHECK(pthread_mutex_unlock(&progress_lock) == 0);
}

static void pause_callback(int kind)
{
    CHECK(!irq_disabled && !spin_depth);
    CHECK(pthread_mutex_lock(&progress_lock) == 0);
    atomic_store(&arrived[kind], true);
    CHECK(pthread_cond_broadcast(&progress_cond) == 0);
    while (!atomic_load(&released[kind]))
        CHECK(pthread_cond_wait(&progress_cond, &progress_lock) == 0);
    CHECK(pthread_mutex_unlock(&progress_lock) == 0);
}

static void release_callback(int kind)
{
    atomic_store(&released[kind], true);
    progress();
}

#define AWAIT(condition) do { \
    struct timespec deadline; \
    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0); deadline.tv_sec += 5; \
    CHECK(pthread_mutex_lock(&progress_lock) == 0); \
    while (!(condition)) \
        CHECK(pthread_cond_timedwait(&progress_cond, &progress_lock, &deadline) == 0); \
    CHECK(pthread_mutex_unlock(&progress_lock) == 0); \
} while (0)

struct mutex { pthread_mutex_t native; const char *name; };
#define DEFINE_MUTEX(name) struct mutex name = {PTHREAD_MUTEX_INITIALIZER, #name}
typedef struct { pthread_mutex_t native; } spinlock_t;
#define DEFINE_SPINLOCK(name) spinlock_t name = {PTHREAD_MUTEX_INITIALIZER}
struct host_wait_queue { pthread_mutex_t lock; pthread_cond_t cond; const char *name; };
#define DECLARE_WAIT_QUEUE_HEAD(name) struct host_wait_queue name = { \
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, #name }

static void mutex_lock(struct mutex *lock)
{
    CHECK(!irq_disabled && !spin_depth);
    int result = pthread_mutex_trylock(&lock->native);
    if (result == EBUSY) {
        if (strcmp(lock->name, "bridge_update_lock") == 0) {
            atomic_fetch_add(&blocked_writers, 1);
            progress();
        }
        result = pthread_mutex_lock(&lock->native);
    }
    CHECK(result == 0);
}

static void mutex_unlock(struct mutex *lock)
{
    CHECK(pthread_mutex_unlock(&lock->native) == 0);
}

#define spin_lock_irqsave(lock, flags) do { \
    (flags) = irq_disabled; irq_disabled = true; \
    CHECK(pthread_mutex_lock(&(lock)->native) == 0); spin_depth++; \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
    CHECK(spin_depth > 0); spin_depth--; \
    CHECK(pthread_mutex_unlock(&(lock)->native) == 0); irq_disabled = (flags); \
} while (0)

#define wait_event(queue, condition) do { \
    bool reported = false; \
    CHECK(!irq_disabled && !spin_depth); \
    CHECK(pthread_mutex_lock(&(queue).lock) == 0); \
    while (!(condition)) { \
        if (!reported && strcmp((queue).name, "bridge_idle") == 0) { \
            atomic_fetch_add(&blocked_drains, 1); reported = true; progress(); \
        } \
        CHECK(pthread_cond_wait(&(queue).cond, &(queue).lock) == 0); \
    } \
    CHECK(pthread_mutex_unlock(&(queue).lock) == 0); \
} while (0)

static void wake_up_all(struct host_wait_queue *queue)
{
    CHECK(pthread_mutex_lock(&queue->lock) == 0);
    CHECK(pthread_cond_broadcast(&queue->cond) == 0);
    CHECK(pthread_mutex_unlock(&queue->lock) == 0);
}

/* SOURCE_TYPES */

typedef int OSAL_SLEEPABLE_LOCK;
typedef spinlock_t OSAL_UNSLEEPABLE_LOCK;
typedef int OSAL_EVENT;
typedef int OSAL_SIGNAL;
typedef int OSAL_THREAD;
typedef int OSAL_TIMER;
typedef int OSAL_WAKE_LOCK;
struct work_struct { int unused; };
struct osal_op_history { int unused; };
typedef struct { OSAL_SIGNAL signal; } OSAL_OP;
typedef struct { OSAL_SLEEPABLE_LOCK sLock; } OSAL_OP_Q;
struct vendor_patch_table { char **active_version; void *patch; int num; };
typedef struct {
    OSAL_THREAD thread, worker_thread;
    OSAL_TIMER worker_timer, utc_sync_timer;
    struct work_struct wmtd_worker_thread_work, utcSyncWorker;
    struct osal_op_history wmtd_op_history, worker_op_history;
    OSAL_OP_Q rFreeOpQ, rActiveOpQ, rWorkerOpQ;
    OSAL_OP arQue[WMT_OP_BUF_SIZE];
    OSAL_EVENT cmdReq, rWmtRxWq, rWmtdWq, rWmtdWorkerWq;
    OSAL_SIGNAL cmdResp;
    OSAL_SLEEPABLE_LOCK mpu_lock, idc_lock, wlan_lock, assert_lock, psm_lock;
    struct { int cfgExist; } rWmtGenConf;
    struct vendor_patch_table patch_table;
} DEV_WMT, *P_DEV_WMT;
static DEV_WMT gDevWmt;
static struct { struct work_struct work; } wmt_assert_work;
static struct work_struct gPwrOnOffWork;
struct cdev { int unused; };
struct class { int unused; };
struct device { int unused; };
struct notifier_block { int unused; };
static struct class class_storage, *wmt_class = &class_storage;
static struct device device_storage, *wmt_dev = &device_storage;
static struct notifier_block wmt_fb_notifier;
static OSAL_UNSLEEPABLE_LOCK g_temp_query_spinlock = {PTHREAD_MUTEX_INITIALIZER};
static OSAL_SLEEPABLE_LOCK g_aee_read_lock, g_dump_info_read_lock, gOsSLock;
static OSAL_WAKE_LOCK wmt_wake_lock;
static int gTemperatureThreshold = 65, gWmtDbgLvl;
static struct { void (*consys_ic_clock_fail_dump)(void); } consys_ops;
static typeof(consys_ops) *wmt_consys_ic_ops = &consys_ops;

/* SOURCE_STATES */
/* SOURCE_PROTOTYPES */

static void dump_stack(void) {}
/* SOURCE_BRIDGE */

static void record_cleanup(void)
{
    atomic_fetch_add(&cleanup_calls, 1);
    if (atomic_load(&module_callbacks))
        atomic_store(&cleanup_while_callback, true);
}

static int osal_thread_destroy(OSAL_THREAD *thread) { record_cleanup(); return 0; }
static void osal_timer_stop_sync(OSAL_TIMER *timer) { record_cleanup(); }
static void wmt_lib_drain_op_queue(OSAL_OP_Q *queue) { record_cleanup(); }
static void cancel_work_sync(struct work_struct *work) { record_cleanup(); }
static void osal_op_history_deinit(struct osal_op_history *history) { record_cleanup(); }
static void wmt_idc_deinit(void) { record_cleanup(); }
static int wmt_lib_ps_deinit(void) { record_cleanup(); return 0; }
static void osal_event_deinit(OSAL_EVENT *event) { record_cleanup(); }
static void osal_signal_deinit(OSAL_SIGNAL *signal) { record_cleanup(); }
static void osal_sleepable_lock_deinit(OSAL_SLEEPABLE_LOCK *lock) { record_cleanup(); }
static void osal_unsleepable_lock_deinit(OSAL_UNSLEEPABLE_LOCK *lock) { record_cleanup(); }
static void wmt_lib_rom_patch_info_free(void) { record_cleanup(); }
/* Broker behavior is covered by test_wmt_command_v2.py. */
static void wmt_lib_cmd_shutdown(void) { record_cleanup(); }
static int wmt_core_deinit(void) { record_cleanup(); return 0; }
static int wmt_conf_deinit(void) { record_cleanup(); return 0; }
static void *osal_memset(void *address, int value, size_t size)
{ record_cleanup(); return memset(address, value, size); }
static int wmt_detect_get_chip_type(void) { return WMT_CHIP_TYPE_SOC; }
static int mtk_wcn_consys_hw_deinit(void) { record_cleanup(); return 0; }
static int mtk_wcn_cmb_hw_deinit(void) { record_cleanup(); return 0; }
static int osal_wake_lock_deinit(OSAL_WAKE_LOCK *lock) { record_cleanup(); return 0; }
static void fb_unregister_client(struct notifier_block *notifier) { record_cleanup(); }
static void wmt_dev_patch_info_free(void) { record_cleanup(); }
static void mtk_wcn_stp_uart_drv_exit(void) { record_cleanup(); }
static void mtk_wcn_stp_sdio_drv_exit(void) { record_cleanup(); }
static void wmt_dev_bgw_desense_deinit(void) { record_cleanup(); }
#define WMT_STEP_DEINIT_FUNC() record_cleanup()
static void wmt_dev_dbg_remove(void) { record_cleanup(); }
static void wmt_dev_proc_for_aee_remove(void) { record_cleanup(); }
static void wmt_dev_proc_for_dump_info_remove(void) { record_cleanup(); }
static void device_destroy(struct class *class_, dev_t dev) { record_cleanup(); }
static void class_destroy(struct class *class_) { record_cleanup(); }
static void cdev_del(struct cdev *dev) { record_cleanup(); }
static void unregister_chrdev_region(dev_t dev, int count) { record_cleanup(); }
static void stp_drv_exit(void) { record_cleanup(); }
/* The power operation is exercised by the separate power-off fixture. */
static int wmt_lib_power_off_for_exit(void) { record_cleanup(); return 0; }
static void mtk_wcn_hif_sdio_driver_exit(void) { record_cleanup(); }
static void osal_lock_unsleepable_lock(OSAL_UNSLEEPABLE_LOCK *lock)
{ CHECK(pthread_mutex_lock(&lock->native) == 0); }
static void osal_unlock_unsleepable_lock(OSAL_UNSLEEPABLE_LOCK *lock)
{ CHECK(pthread_mutex_unlock(&lock->native) == 0); }
static void do_gettimeofday(struct timeval *now) { CHECK(gettimeofday(now, NULL) == 0); }
static int wmt_dev_tra_poll(void) { return 0; }

static int mtk_wcn_wmt_therm_ctrl(int operation)
{
    CHECK(!irq_disabled && !spin_depth);
    atomic_fetch_add(&thermal_commands, 1);
    atomic_fetch_add(&operation_references, 1);
    if (operation == WMTTHERM_READ && nested_assertion)
        CHECK(mtk_wcn_cmb_stub_trigger_assert() == -1);
    if (operation == WMTTHERM_READ && pause_thermal_command)
        pause_callback(0);
    atomic_fetch_sub(&operation_references, 1);
    return operation == WMTTHERM_READ ? 42 : 0;
}

static int wmt_lib_trigger_assert_keyword(ENUM_WMTDRV_TYPE_T type, UINT32 reason, PUINT8 keyword)
{
    CHECK(!spin_depth && !irq_disabled);
    CHECK(type == WMTDRV_TYPE_WMT && reason == 45 && keyword == NULL);
    atomic_fetch_add(&assertion_calls, 1);
    return -1;
}

static void host_step_action(int point)
{
    CHECK(!spin_depth && point == STEP_TRIGGER_POINT_WHEN_CLOCK_FAIL);
    atomic_fetch_add(&clock_calls, 1);
}
#define WMT_STEP_DO_ACTIONS_FUNC(point) host_step_action(point)

/* SOURCE_FUNCTIONS */

static int callback_index(void *function) NO_INSTRUMENT;
static int callback_index(void *function)
{
    if (function == (void *)_mtk_wcn_cmb_stub_query_ctrl) return 0;
    if (function == (void *)_mtk_wcn_cmb_stub_trigger_assert) return 1;
    if (function == (void *)_mtk_wcn_cmb_stub_clock_fail_dump) return 2;
    return -1;
}

void __cyg_profile_func_enter(void *function, void *caller)
{
    int kind = callback_index(function);
    if (kind >= 0) {
        CHECK(!spin_depth);
        atomic_fetch_add(&module_callbacks, 1);
        if (atomic_load(&pause_on_entry[kind]))
            pause_callback(kind);
    }
}

void __cyg_profile_func_exit(void *function, void *caller)
{
    int kind = callback_index(function);
    if (kind >= 0) {
        if (atomic_load(&pause_on_return[kind]))
            pause_callback(kind);
        CHECK(atomic_fetch_sub(&module_callbacks, 1) > 0);
        progress();
    }
}

static void register_actual_chain(void)
{
    struct _CMB_STUB_CB_ callbacks = {0};
    callbacks.size = sizeof(callbacks);
    callbacks.thermal_query_cb = wmt_plat_thermal_ctrl;
    callbacks.trigger_assert_cb = wmt_plat_assert_ctrl;
    callbacks.clock_fail_dump_cb = wmt_plat_clock_fail_dump;
    CHECK(mtk_wcn_cmb_stub_reg(&callbacks) == 0);
    CHECK(wmt_lib_register_thermal_ctrl_cb(wmt_dev_tm_temp_query) == 0);
    CHECK(wmt_lib_register_trigger_assert_cb(wmt_lib_trigger_assert) == 0);
}

static void prepare_lifecycle(void)
{
    gWmtInitStatus = WMT_INIT_DONE;
    g_wmt_platform_initialized = true;
    g_wmt_resources_initialized = true;
    g_wmt_core_initialized = true;
    g_wmt_ps_initialized = true;
    g_wmt_idc_initialized = true;
    g_wmt_op_pool_initialized = true;
    g_wmt_worker_timer_initialized = true;
    g_wmt_utc_timer_initialized = true;
    g_wmt_assert_work_initialized = true;
    gDevWmt.rWmtGenConf.cfgExist = 1;
#ifdef HOST_HAS_PLATFORM_OWNER
    g_wmt_plat_initialized = true;
    g_wmt_plat_chip_type = WMT_CHIP_TYPE_SOC;
#endif
}

static int replacement_query(void)
{
    CHECK(!spin_depth);
    atomic_fetch_add(&replacement_calls, 1);
    return 73;
}

enum operation { QUERY, ASSERTION, CLOCK, UNREGISTER, REGISTER, OUTER_EXIT, LIBRARY_EXIT };
struct job {
    pthread_t thread;
    enum operation operation;
    int result;
    atomic_bool done;
};

static void *run_job(void *argument)
{
    struct job *job = argument;
    struct wmt_platform_bridge replacement = {.thermal_query_cb = replacement_query};
    switch (job->operation) {
    case QUERY: job->result = mtk_wcn_cmb_stub_query_ctrl(); break;
    case ASSERTION: job->result = mtk_wcn_cmb_stub_trigger_assert(); break;
    case CLOCK: mtk_wcn_cmb_stub_clock_fail_dump(); break;
    case UNREGISTER: wmt_export_platform_bridge_unregister(); break;
    case REGISTER: wmt_export_platform_bridge_register(&replacement); break;
    case OUTER_EXIT: WMT_exit(); break;
    case LIBRARY_EXIT: job->result = wmt_lib_deinit(); break;
    }
    atomic_store(&job->done, true);
    progress();
    return NULL;
}

static void start_job(struct job *job, enum operation operation)
{
    job->operation = operation;
    atomic_init(&job->done, false);
    CHECK(pthread_create(&job->thread, NULL, run_job, job) == 0);
}

static void join_job(struct job *job)
{
    CHECK(pthread_join(job->thread, NULL) == 0);
    CHECK(atomic_load(&job->done));
}

static void require_blocked(struct job *writer, int previous_drains, bool before_cleanup)
{
    AWAIT(atomic_load(&writer->done) || atomic_load(&blocked_drains) > previous_drains);
    fprintf(stderr, "writer_returned=%d active_module_callbacks=%d operation_refs=%d cleanup_calls=%d\n",
            atomic_load(&writer->done), atomic_load(&module_callbacks),
            atomic_load(&operation_references), atomic_load(&cleanup_calls));
    CHECK(!atomic_load(&writer->done));
    CHECK(atomic_load(&module_callbacks) > 0);
    if (before_cleanup) CHECK(atomic_load(&cleanup_calls) == 0);
}

static void require_closed(void)
{
    int commands = atomic_load(&thermal_commands), assertions = atomic_load(&assertion_calls);
    int clocks = atomic_load(&clock_calls);
    CHECK(mtk_wcn_cmb_stub_query_ctrl() == -1);
    CHECK(mtk_wcn_cmb_stub_trigger_assert() == -1);
    mtk_wcn_cmb_stub_clock_fail_dump();
    CHECK(atomic_load(&thermal_commands) == commands);
    CHECK(atomic_load(&assertion_calls) == assertions);
    CHECK(atomic_load(&clock_calls) == clocks);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    int test = atoi(argv[1]);
    struct job reader[3] = {0}, writer = {0}, second = {0};
    if (test == 0) {
        require_closed();
        wmt_export_platform_bridge_unregister();
        wmt_export_platform_bridge_unregister();
        wmt_export_platform_bridge_register(NULL);
        require_closed();
    } else if (test == 1) {
        register_actual_chain();
        CHECK(mtk_wcn_cmb_stub_query_ctrl() == 42);
        CHECK(mtk_wcn_cmb_stub_trigger_assert() == -1);
        mtk_wcn_cmb_stub_clock_fail_dump();
        CHECK(atomic_load(&thermal_commands) == 3);
        CHECK(atomic_load(&assertion_calls) == 1 && atomic_load(&clock_calls) == 1);
    } else if ((test >= 2 && test <= 7) || (test >= 14 && test <= 16) || test == 18) {
        register_actual_chain();
        int first = test == 4 ? 1 : test == 5 ? 2 : 0;
        int count = test == 6 ? 3 : 1;
        pause_thermal_command = test == 2;
        for (int kind = first; kind < first + count; kind++) {
            atomic_store(&pause_on_entry[kind], test == 18);
            atomic_store(&pause_on_return[kind], !pause_thermal_command && test != 18);
            start_job(&reader[kind], (enum operation)kind);
            AWAIT(atomic_load(&arrived[kind]));
        }
        bool lifecycle = test >= 14 && test <= 16;
        if (lifecycle) prepare_lifecycle();
        if (test == 16) g_wmt_platform_initialized = false;
        int old_drains = atomic_load(&blocked_drains);
        start_job(&writer, test == 14 ? OUTER_EXIT : lifecycle ? LIBRARY_EXIT : UNREGISTER);
        require_blocked(&writer, old_drains, lifecycle);
        if (test != 2) CHECK(atomic_load(&operation_references) == 0);
        require_closed();
        for (int kind = first; kind < first + count; kind++) {
            release_callback(kind);
            join_job(&reader[kind]);
            if (kind + 1 < first + count) CHECK(!atomic_load(&writer.done));
        }
        join_job(&writer);
        CHECK(!atomic_load(&cleanup_while_callback));
        if (lifecycle) CHECK(atomic_load(&cleanup_calls) > 0);
    } else if (test == 8 || test == 9 || test == 10) {
        register_actual_chain();
        atomic_store(&pause_on_return[0], true);
        start_job(&reader[0], QUERY);
        AWAIT(atomic_load(&arrived[0]));
        int old_drains = atomic_load(&blocked_drains);
        start_job(&writer, test == 8 ? REGISTER : UNREGISTER);
        require_blocked(&writer, old_drains, false);
        require_closed();
        if (test != 8) {
            int old_writers = atomic_load(&blocked_writers);
            start_job(&second, test == 9 ? REGISTER : UNREGISTER);
            AWAIT(atomic_load(&second.done) || atomic_load(&blocked_writers) > old_writers);
            CHECK(!atomic_load(&second.done));
            require_closed();
        }
        release_callback(0);
        join_job(&reader[0]);
        join_job(&writer);
        if (test != 8) join_job(&second);
        if (test != 10) {
            CHECK(mtk_wcn_cmb_stub_query_ctrl() == 73);
            CHECK(atomic_load(&replacement_calls) == 1);
        }
    } else if (test == 11) {
        register_actual_chain();
        nested_assertion = true;
        CHECK(mtk_wcn_cmb_stub_query_ctrl() == 42);
        CHECK(atomic_load(&assertion_calls) == 1);
    } else if (test == 12) {
        register_actual_chain();
        irq_disabled = true;
        mtk_wcn_cmb_stub_clock_fail_dump();
        CHECK(irq_disabled && !spin_depth);
        irq_disabled = false;
        CHECK(atomic_load(&clock_calls) == 1);
    } else if (test == 13) {
        struct wmt_platform_bridge partial = {.thermal_query_cb = replacement_query};
        wmt_export_platform_bridge_register(&partial);
        CHECK(mtk_wcn_cmb_stub_trigger_assert() == -1);
        mtk_wcn_cmb_stub_clock_fail_dump();
        CHECK(mtk_wcn_cmb_stub_query_ctrl() == 73);
        wmt_export_platform_bridge_unregister();
        require_closed();
    } else if (test == 17) {
        register_actual_chain();
        wmt_export_platform_bridge_unregister();
        require_closed();
        register_actual_chain();
        CHECK(mtk_wcn_cmb_stub_query_ctrl() == 42);
    } else {
        CHECK(false);
    }
    wmt_export_platform_bridge_unregister();
    require_closed();
    CHECK(!atomic_load(&module_callbacks) && !atomic_load(&operation_references));
    printf("PASS: drained=%d serialized_writers=%d cleanup=%d\n", atomic_load(&blocked_drains),
           atomic_load(&blocked_writers), atomic_load(&cleanup_calls));
    return 0;
}
