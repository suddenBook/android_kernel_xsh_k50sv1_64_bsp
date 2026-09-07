/* Strict lifecycle adapters. Production init/deinit and OSAL constructors are inserted below. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void VOID;
typedef void *PVOID;
typedef int INT32;
typedef unsigned int UINT32;
typedef char UINT8;
typedef UINT8 *PUINT8;
typedef unsigned long ULONG;
typedef int ENUM_WMT_CHIP_TYPE;
typedef int ENUM_WMTDRV_TYPE_T;
typedef int MTK_WCN_BOOL;
typedef int MTKSTP_BTM_WMT_OP_T;
#define MTK_WCN_BOOL_FALSE 0
#define MTK_WCN_BOOL_TRUE 1
enum { BTM_RST_OP, BTM_DMP_OP, BTM_GET_AEE_SUPPORT_FLAG, BTM_TRIGGER_STP_ASSERT_OP };
#define WMTRSTSRC_RESET_STP 4
typedef atomic_int atomic_t;
#define ATOMIC_INIT(value) (value)
#define atomic_read(pointer) atomic_load(pointer)
#define READ_ONCE(value) __atomic_load_n(&(value), __ATOMIC_RELAXED)
#define WRITE_ONCE(value, data) __atomic_store_n(&(value), (data), __ATOMIC_RELAXED)
#define WMT_ERR_FUNC(...) ((void)0)
#define WMT_WARN_FUNC(...) ((void)0)
#define WMT_DBG_FUNC(...) ((void)0)
#define WMT_INFO_FUNC(...) ((void)0)
#define osal_strncpy strncpy
#define WMT_OP_BUF_SIZE 16
#define WMTDRV_TYPE_WIFI 3
#define WMTHWVER_MAX 8
#define WMT_CHIP_TYPE_SOC 1
#define UTC_SYNC_TIME 1000
#define CFG_WMT_PS_SUPPORT 1
#define CFG_WMT_LTE_COEX_HANDLING 1
#define MTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT 1
#define GFP_KERNEL 0
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))
#define ERR_PTR(error) ((void *)(intptr_t)(error))

struct mutex { pthread_mutex_t native; };
#define DEFINE_MUTEX(name) struct mutex name = {PTHREAD_MUTEX_INITIALIZER}
typedef struct { bool initialized; } spinlock_t;
#define DEFINE_SPINLOCK(name) spinlock_t name = {true}
#define DECLARE_WAIT_QUEUE_HEAD(name) int name
#define wait_event(queue, condition) assert(condition)
typedef struct { bool initialized; } OSAL_SLEEPABLE_LOCK;
typedef struct { bool initialized; } OSAL_EVENT;
typedef struct { UINT32 timeoutValue; bool initialized; } OSAL_SIGNAL;
struct work_struct {
    bool initialized, pending, running;
    void (*callback)(struct work_struct *);
};
typedef struct {
    PVOID timeoutHandler;
    ULONG timeroutHandlerData;
    bool initialized, active, stopped;
} OSAL_TIMER;
struct host_task { bool stopped, started; };
typedef struct {
    struct host_task *pThread;
    PVOID pThreadFunc, pThreadData;
    char threadName[32];
} OSAL_THREAD, *P_OSAL_THREAD;
struct ring { void *base; int size; };
struct osal_op_history_entry { unsigned char data[80]; };
struct osal_op_history {
    struct ring ring_buffer, dump_ring_buffer;
    struct osal_op_history_entry *queue;
    spinlock_t lock;
    struct work_struct dump_work;
    bool dump_busy;
};
typedef struct { OSAL_SIGNAL signal; } OSAL_OP, *P_OSAL_OP;
typedef struct { OSAL_SLEEPABLE_LOCK sLock; int size, count; } OSAL_OP_Q, *P_OSAL_OP_Q;
#define RB_INIT(queue, length) do { (queue)->size = (length); (queue)->count = 0; } while (0)
typedef struct { int ldoStableTime, rstStableTime, onStableTime, offStableTime, rtcStableTime; } PWR_SEQ_TIME;
struct vendor_patch_table { char **active_version; void *patch; int num; };
typedef struct {
    OSAL_THREAD thread, worker_thread;
    OSAL_TIMER worker_timer, utc_sync_timer;
    struct work_struct wmtd_worker_thread_work, utcSyncWorker;
    struct osal_op_history wmtd_op_history, worker_op_history;
    int hw_ver;
    OSAL_EVENT rWmtdWq, rWmtdWorkerWq, rWmtRxWq, cmdReq;
    OSAL_SIGNAL cmdResp;
    OSAL_SLEEPABLE_LOCK psm_lock, idc_lock, wlan_lock, assert_lock, mpu_lock;
    OSAL_OP_Q rFreeOpQ, rActiveOpQ, rWorkerOpQ;
    OSAL_OP arQue[WMT_OP_BUF_SIZE];
    struct { unsigned long data; } state;
    struct { void *fDrvRst[WMTDRV_TYPE_WIFI]; } rFdrvCb;
    struct {
        int cfgExist, co_clock_flag, pwr_on_ldo_slot, pwr_on_rst_slot,
            pwr_on_on_slot, pwr_on_off_slot, pwr_on_rtc_slot;
        void *allocation;
    } rWmtGenConf;
    struct vendor_patch_table patch_table;
} DEV_WMT, *P_DEV_WMT;
static DEV_WMT gDevWmt;
struct assert_work_st {
    struct work_struct work;
    ENUM_WMTDRV_TYPE_T type;
    UINT32 reason;
    UINT8 keyword[20];
};
static struct assert_work_st wmt_assert_work;
/* SOURCE_STATES */
/* SOURCE_PROTOTYPES */
/* The concurrent callback barrier is exercised by test_wmt_callback_lifetime.py. */
static void wmt_export_platform_bridge_unregister(void) {}

struct gate { pthread_mutex_t lock; pthread_cond_t cond; bool enabled, arrived, released; };
#define GATE_INIT {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, false, false}
static struct gate publication = GATE_INIT, close_blocked = GATE_INIT;
static struct gate assert_running = GATE_INIT, cancel_entered = GATE_INIT;
static struct gate stp_callback = GATE_INIT;
static _Thread_local bool teardown_thread;
static pthread_mutex_t spin_serial = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t work_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cond = PTHREAD_COND_INITIALIZER;
static int failure, chip_type = WMT_CHIP_TYPE_SOC, history_allocations, creates;
static int live_allocations, live_tasks, live_locks, live_events, live_signals;
static int core_live, platform_live, ps_live, idc_live;
static int core_inits, platform_inits, ps_inits, idc_inits, task_stops, clears;
static int timer_stops, work_cancels, published_asserts, finished_asserts;
static struct host_task tasks[32];
static int task_count;

static void mark(struct gate *gate)
{
    pthread_mutex_lock(&gate->lock);
    gate->arrived = true;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->lock);
}
static void await(struct gate *gate)
{
    pthread_mutex_lock(&gate->lock);
    while (!gate->arrived) pthread_cond_wait(&gate->cond, &gate->lock);
    pthread_mutex_unlock(&gate->lock);
}
static void release(struct gate *gate)
{
    pthread_mutex_lock(&gate->lock);
    gate->released = true;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->lock);
}
static void pause_at(struct gate *gate)
{
    if (!gate->enabled) return;
    pthread_mutex_lock(&gate->lock);
    gate->arrived = true;
    pthread_cond_broadcast(&gate->cond);
    while (!gate->released) pthread_cond_wait(&gate->cond, &gate->lock);
    pthread_mutex_unlock(&gate->lock);
}
static void mutex_lock(struct mutex *lock) { assert(pthread_mutex_lock(&lock->native) == 0); }
static void mutex_unlock(struct mutex *lock) { assert(pthread_mutex_unlock(&lock->native) == 0); }
static void spin_lock_init(spinlock_t *lock) { lock->initialized = true; }
static void host_spin_lock(spinlock_t *lock)
{
    assert(lock->initialized);
    if (teardown_thread) {
        int ret = pthread_mutex_trylock(&spin_serial);
        if (!ret) return;
        assert(ret == EBUSY);
        mark(&close_blocked);
    }
    assert(pthread_mutex_lock(&spin_serial) == 0);
}
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; host_spin_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); pthread_mutex_unlock(&spin_serial); } while (0)
static void *allocate(size_t bytes)
{
    void *pointer = calloc(1, bytes);
    assert(pointer);
    live_allocations++;
    return pointer;
}
static void osal_free(void *pointer)
{
    if (!pointer) return;
    assert(live_allocations > 0);
    live_allocations--;
    free(pointer);
}
#define kfree osal_free
static void *kcalloc(size_t count, size_t size, int flags)
{
    history_allocations++;
    if (failure >= 13 && failure <= 16 && history_allocations == failure - 12) return NULL;
    return allocate(count * size);
}
static void *osal_memset(void *pointer, int byte, size_t length)
{
    if (pointer == &gDevWmt) {
        assert(live_allocations == 0 && live_tasks == 0);
        assert(!live_locks && !live_events && !live_signals);
        assert(!core_live && !platform_live && !ps_live && !idc_live);
        assert(!wmt_assert_work.work.pending && !wmt_assert_work.work.running);
        clears++;
    }
    return memset(pointer, byte, length);
}
static void osal_sleepable_lock_init(OSAL_SLEEPABLE_LOCK *lock)
{ assert(!lock->initialized); lock->initialized = true; live_locks++; }
static void osal_sleepable_lock_deinit(OSAL_SLEEPABLE_LOCK *lock)
{ assert(lock->initialized); lock->initialized = false; assert(live_locks-- > 0); }
static void osal_event_init(OSAL_EVENT *event)
{ assert(!event->initialized); event->initialized = true; live_events++; }
static void osal_event_deinit(OSAL_EVENT *event)
{
    /* Production event deinit is a no-op, including on zeroed storage. */
    if (event->initialized) { event->initialized = false; assert(live_events-- > 0); }
}
static void osal_signal_init(OSAL_SIGNAL *signal)
{ assert(!signal->initialized); signal->initialized = true; live_signals++; }
static void osal_signal_deinit(OSAL_SIGNAL *signal)
{
    /* Production signal deinit only clears timeoutValue for a non-NULL pointer. */
    signal->timeoutValue = 0;
    if (signal->initialized) { signal->initialized = false; assert(live_signals-- > 0); }
}
static struct host_task *kthread_create(PVOID function, PVOID data, char *name)
{
    creates++;
    if ((creates == 1 && failure >= 2 && failure <= 4) ||
        (creates == 2 && failure >= 5 && failure <= 7)) {
        int kind = (failure - 2) % 3;
        return kind == 0 ? NULL : ERR_PTR(kind == 1 ? -ENOMEM : -EINTR);
    }
    assert(task_count < (int)(sizeof(tasks) / sizeof(tasks[0])));
    live_tasks++;
    return &tasks[task_count++];
}
static int kthread_stop(struct host_task *task)
{
    assert(task && !IS_ERR(task));
    /* The kernel releases a stopped task; a second stop may use a stale handle. */
    assert(!task->stopped);
    task->stopped = true;
    assert(live_tasks-- > 0);
    task_stops++;
    return task->started ? 0 : -EINTR;
}
static int osal_thread_run(P_OSAL_THREAD thread)
{
    assert(thread->pThread && !IS_ERR(thread->pThread) && !thread->pThread->stopped);
    assert(core_live && platform_live && ps_live);
    assert(live_locks == 8 && live_events == 4 && live_signals == WMT_OP_BUF_SIZE + 1);
    if ((failure == 11 && thread == &gDevWmt.thread) ||
        (failure == 12 && thread == &gDevWmt.worker_thread)) return -1;
    thread->pThread->started = true;
    return 0;
}
static int wmtd_thread(PVOID data) { return 0; }
static int wmtd_worker_thread(PVOID data) { return 0; }
static void init_work(struct work_struct *work, void (*callback)(struct work_struct *))
{
    assert(!work->pending && !work->running);
    work->initialized = true;
    work->callback = callback;
}
#define INIT_WORK(work, callback) init_work(work, callback)
static void osal_op_history_print_work(struct work_struct *work) {}
static void wmt_lib_wmtd_worker_thread_work_handler(struct work_struct *work) {}
static void wmt_lib_utc_sync_worker_handler(struct work_struct *work) {}
static void wmt_lib_wmtd_worker_thread_timeout_handler(ULONG data) {}
static void wmt_lib_utc_sync_timeout_handler(ULONG data) {}
static void osal_timer_create(OSAL_TIMER *timer)
{ assert(!timer->initialized); timer->initialized = true; }
static void osal_timer_start(OSAL_TIMER *timer, UINT32 delay)
{ assert(timer->initialized); timer->active = true; }
static void osal_timer_stop_sync(OSAL_TIMER *timer)
{
    assert(timer->initialized && !timer->stopped);
    timer->stopped = true;
    timer->active = false;
    timer_stops++;
}
static void cancel_work_sync(struct work_struct *work)
{
    assert(work->initialized);
    if (work == &gDevWmt.wmtd_worker_thread_work) assert(gDevWmt.worker_timer.stopped);
    if (work == &gDevWmt.utcSyncWorker) assert(gDevWmt.utc_sync_timer.stopped);
    if (work == &wmt_assert_work.work) mark(&cancel_entered);
    pthread_mutex_lock(&work_lock);
    work->pending = false;
    while (work->running) pthread_cond_wait(&work_cond, &work_lock);
    pthread_mutex_unlock(&work_lock);
    work_cancels++;
}
static bool schedule_work(struct work_struct *work)
{
    assert(work->initialized);
    if (work == &wmt_assert_work.work) pause_at(&publication);
    pthread_mutex_lock(&work_lock);
    work->pending = true;
    pthread_mutex_unlock(&work_lock);
    if (work == &wmt_assert_work.work) published_asserts++;
    return true;
}
static void ring_init(void *data, int count, int read, int write, struct ring *ring)
{ ring->base = data; ring->size = count; }
static int wmt_lib_put_op(P_OSAL_OP_Q queue, P_OSAL_OP operation)
{
    assert(queue->sLock.initialized && queue->count < queue->size);
    assert(operation->signal.initialized);
    queue->count++;
    return true;
}
static void wmt_lib_drain_op_queue(P_OSAL_OP_Q queue)
{
    /* Queue ownership schedules are exercised by test_wmt_shutdown.py. */
    assert(g_wmt_op_pool_stopping);
    if (g_wmt_op_pool_initialized) assert(queue->sLock.initialized && queue->count == 0);
}
static int wmt_detect_get_chip_type(void) { return chip_type; }
static int wmt_conf_read_file(void)
{
    if (failure == 1) return -1;
    assert(!gDevWmt.rWmtGenConf.allocation);
    gDevWmt.rWmtGenConf.allocation = allocate(32);
    gDevWmt.rWmtGenConf.cfgExist = 1;
    return 0;
}
static int wmt_conf_deinit(void)
{
    assert(gDevWmt.rWmtGenConf.cfgExist && gDevWmt.rWmtGenConf.allocation);
    osal_free(gDevWmt.rWmtGenConf.allocation);
    gDevWmt.rWmtGenConf.allocation = NULL;
    gDevWmt.rWmtGenConf.cfgExist = 0;
    return 0;
}
static int wmt_core_init(void)
{ if (failure == 8) return -1; assert(!core_live); core_live = 1; core_inits++; return 0; }
static int wmt_core_deinit(void)
{ /* Production clears static core storage and is safe before init. */ core_live = 0; return 0; }
static int wmt_plat_init(PWR_SEQ_TIME *sequence, int flags)
{ if (failure == 9) return -1; assert(!platform_live); platform_live = 1; platform_inits++; return 0; }
static int wmt_plat_deinit(void)
{ assert(platform_live); platform_live = 0; return 0; }
static int wmt_plat_soc_co_clock_flag_get(void) { return 0; }
static int wmt_lib_ps_init(void)
{ if (failure == 10) return -1; assert(!ps_live); ps_live = 1; ps_inits++; return 0; }
static int wmt_lib_ps_deinit(void)
{ /* Production PS deinit currently does no work. */ ps_live = 0; return 0; }
static int wmt_idc_init(void)
{ if (failure == 17) return -1; assert(!idc_live); idc_live = 1; idc_inits++; return 0; }
static int wmt_idc_deinit(void)
{ assert(idc_live && live_locks); idc_live = 0; return 0; }
static void wmt_lib_rom_patch_info_free(void) {}
/* The command v2 fixture exercises these broker stages, including cancellation. */
static void wmt_lib_cmd_start(void) {}
static void wmt_lib_cmd_shutdown(void) {}
static void wmt_dev_patch_info_free(void)
{
    assert(!gDevWmt.thread.pThread && !gDevWmt.worker_thread.pThread);
    assert(atomic_read(&g_wmt_ops_checked_out) == 0);
}
static void mtk_wcn_wmt_system_state_reset(void) {}
#define wmt_plat_irq_cb_reg(callback) assert(platform_live)
#define wmt_plat_aif_cb_reg(callback) assert(platform_live)
#define wmt_plat_func_ctrl_cb_reg(callback) assert(platform_live)
#define wmt_plat_deep_idle_ctrl_cb_reg(callback) assert(platform_live)
#define WMT_STEP_DEINIT_FUNC() assert(!"STEP is owned by outer WMT_init/WMT_exit")
static void wmt_lib_assert_work_cb(struct work_struct *work)
{
    pause_at(&assert_running);
    assert(core_live && platform_live && live_locks == 8);
    finished_asserts++;
}
static void wmt_lib_cmb_rst(int source)
{
    assert(source == WMTRSTSRC_RESET_STP);
    pause_at(&stp_callback);
    /* The production reset path uses these after the WMT pool is already idle. */
    assert(core_live && platform_live && ps_live);
    assert(gDevWmt.psm_lock.initialized && gDevWmt.rWmtRxWq.initialized);
}
static int wmt_core_get_aee_dump_flag(void) { assert(core_live); return 0; }
static int wmt_core_trigger_stp_assert(void) { assert(core_live); return 0; }

/* PRODUCTION */

static void clean(void)
{
    assert(!live_allocations && !live_tasks && !live_locks && !live_events && !live_signals);
    assert(!core_live && !platform_live && !ps_live && !idc_live);
    assert(g_wmt_op_pool_stopping && !g_wmt_op_pool_initialized);
    assert(!g_wmt_worker_timer_initialized && !g_wmt_utc_timer_initialized);
    assert(!gDevWmt.thread.pThread && !gDevWmt.worker_thread.pThread);
    assert(!wmt_assert_work.work.pending && !wmt_assert_work.work.running);
}
static void next_cycle(void)
{
    failure = 0;
    history_allocations = creates = 0;
    assert(wmt_lib_init() == 0);
    assert(!g_wmt_op_pool_stopping && g_wmt_op_pool_initialized);
    assert(wmt_lib_deinit() == 0);
    clean();
    /* The outer cleanup may be invoked again after an earlier failure. */
    assert(wmt_lib_deinit() == 0);
    clean();
}
static void *shutdown_call(void *unused)
{
    teardown_thread = true;
    assert(wmt_lib_deinit() == 0);
    return NULL;
}
static void *publish_call(void *unused)
{ wmt_lib_trigger_assert_keyword_delay(1, 2, "test"); return NULL; }
static void *assert_work_call(void *unused)
{
    struct work_struct *work = &wmt_assert_work.work;
    pthread_mutex_lock(&work_lock);
    assert(work->pending && !work->running);
    work->pending = false;
    work->running = true;
    pthread_mutex_unlock(&work_lock);
    work->callback(work);
    pthread_mutex_lock(&work_lock);
    work->running = false;
    pthread_cond_broadcast(&work_cond);
    pthread_mutex_unlock(&work_lock);
    return NULL;
}
static void join(pthread_t thread) { assert(pthread_join(thread, NULL) == 0); }
static void *stp_callback_call(void *unused)
{ assert(wmt_lib_btm_cb(BTM_RST_OP) == MTK_WCN_BOOL_TRUE); return NULL; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    int test = atoi(argv[1]);
    failure = test <= 17 ? test : 0;
    chip_type = test == 19 ? 0 : WMT_CHIP_TYPE_SOC;
    if (test == 0 || test == 25) {
        if (test == 25) {
            wmt_lib_trigger_assert_keyword_delay(1, 2, "early");
            assert(published_asserts == 0);
        }
        assert(wmt_lib_deinit() == 0);
        clean();
    } else {
        int ret = wmt_lib_init();
        int expected = test == 1 || test == 8 ? -1 :
                       test >= 2 && test <= 7 ? -2 : test == 9 ? -3 :
                       test == 10 ? -4 : test == 11 || test == 12 ? -5 : 0;
        assert(ret == expected);
        assert(g_wmt_op_pool_stopping == (expected != 0));
        assert(task_stops == 0);
        if (test == 20) {
            schedule_work(&gDevWmt.wmtd_worker_thread_work);
            schedule_work(&gDevWmt.utcSyncWorker);
            schedule_work(&gDevWmt.wmtd_op_history.dump_work);
            schedule_work(&gDevWmt.worker_op_history.dump_work);
        }
        if (test == 21) {
            struct vendor_patch_table *table = &gDevWmt.patch_table;
            table->num = 3;
            table->patch = allocate(96);
            table->active_version = allocate(3 * sizeof(*table->active_version));
            table->active_version[0] = allocate(32);
            table->active_version[2] = allocate(32);
        }
        if (test == 22) {
            pthread_t producer, teardown;
            publication.enabled = true;
            assert(pthread_create(&producer, NULL, publish_call, NULL) == 0);
            await(&publication);
            assert(pthread_create(&teardown, NULL, shutdown_call, NULL) == 0);
            await(&close_blocked);
            assert(clears == 1 && core_live && platform_live);
            release(&publication);
            join(producer);
            join(teardown);
            assert(published_asserts == 1);
        } else if (test == 23) {
            pthread_t worker, teardown;
            assert_running.enabled = true;
            wmt_lib_trigger_assert_keyword_delay(1, 2, "test");
            assert(pthread_create(&worker, NULL, assert_work_call, NULL) == 0);
            await(&assert_running);
            assert(pthread_create(&teardown, NULL, shutdown_call, NULL) == 0);
            await(&cancel_entered);
            assert(clears == 1 && core_live && platform_live);
            release(&assert_running);
            join(worker);
            join(teardown);
            assert(finished_asserts == 1);
        } else if (test == 26) {
            pthread_t callback;
            stp_callback.enabled = true;
            assert(pthread_create(&callback, NULL, stp_callback_call, NULL) == 0);
            await(&stp_callback);
            assert(atomic_read(&g_wmt_ops_checked_out) == 0);
#ifdef HOST_SPLIT_DEINIT
            assert(wmt_lib_deinit_prepare() == 0);
            assert(wmt_lib_deinit_prepare() == 0);
#else
            assert(wmt_lib_deinit() == 0);
#endif
            release(&stp_callback);
            join(callback);
#ifdef HOST_SPLIT_DEINIT
            assert(wmt_lib_deinit_finish() == 0);
#endif
        } else {
            assert(wmt_lib_deinit() == 0);
        }
        clean();
        if (test == 24) {
            wmt_lib_trigger_assert_keyword_delay(1, 2, "late");
            assert(published_asserts == 0);
            clean();
        }
        assert(task_stops == (test == 1 || (test >= 2 && test <= 4) ? 0 :
                              test >= 5 && test <= 7 ? 1 : 2));
        assert(timer_stops == (test <= 4 ? 0 : test <= 7 ? 1 : 2));
        if (test == 20) assert(work_cancels == 5);
    }
    next_cycle();
    printf("PASS: clears=%d tasks_joined=%d core=%d platform=%d ps=%d idc=%d\n",
           clears, task_stops, core_inits, platform_inits, ps_inits, idc_inits);
    return 0;
}
