/* Host scheduling and allocator substitutes; pool, workers and teardown are extracted verbatim. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void VOID;
typedef void *PVOID;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef unsigned long ULONG;
typedef size_t SIZE_T;
typedef atomic_int atomic_t;
typedef bool MTK_WCN_BOOL;
#define MTK_WCN_BOOL_FALSE false
#define MTK_WCN_BOOL_TRUE true
#define ATOMIC_INIT(value) (value)
#define atomic_set(p, value) atomic_store(p, value)
#define atomic_inc(p) ((void)atomic_fetch_add(p, 1))
#define atomic_dec(p) ((void)atomic_fetch_sub(p, 1))
#define atomic_read(p) atomic_load(p)
#define atomic_dec_and_test(p) (atomic_fetch_sub(p, 1) == 1)
#define READ_ONCE(value) __atomic_load_n(&(value), __ATOMIC_RELAXED)
#define WRITE_ONCE(value, data) __atomic_store_n(&(value), (data), __ATOMIC_RELAXED)
#define WMT_ERR_FUNC(...) ((void)0)
#define WMT_WARN_FUNC(...) ((void)0)
#define WMT_DBG_FUNC(...) ((void)0)
#define WMT_INFO_FUNC(...) ((void)0)
#define osal_assert assert
#define osal_sizeof sizeof
#define osal_strncpy strncpy
#define WMTDRV_TYPE_WIFI 3
#define WMT_STAT_RST_ON 0
#define WMTHWVER_MAX 8
#define WMT_CHIP_TYPE_SOC 1
#define UTC_SYNC_TIME 1000
#define MAX_FUNC_ON_TIME 1000
#define CFG_WMT_PS_SUPPORT 0
#define CFG_WMT_LTE_COEX_HANDLING 0
#define MTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT 1
typedef int ENUM_WMT_CHIP_TYPE;

struct mutex { pthread_mutex_t native; bool initialized; };
#define DEFINE_MUTEX(name) struct mutex name = {PTHREAD_MUTEX_INITIALIZER, true}
#define DEFINE_SPINLOCK(name) DEFINE_MUTEX(name)
typedef struct { struct mutex lock; } OSAL_SLEEPABLE_LOCK, *P_OSAL_SLEEPABLE_LOCK;
typedef struct { pthread_mutex_t lock; pthread_cond_t cond; } wait_queue_head_t;
#define DECLARE_WAIT_QUEUE_HEAD(name) wait_queue_head_t name = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER}
typedef struct { UINT32 timeoutValue; atomic_int done; } OSAL_SIGNAL, *P_OSAL_SIGNAL;
typedef struct { wait_queue_head_t waitQueue; UINT32 timeoutValue; bool initialized; } OSAL_EVENT, *P_OSAL_EVENT;
struct work_struct { bool initialized, cancelled; };
typedef struct { PVOID timeoutHandler; ULONG timeroutHandlerData; bool initialized, stopped; } OSAL_TIMER;
struct host_task { pthread_t thread; pthread_mutex_t lock; pthread_cond_t cond; bool run, joined; atomic_int stop; };
typedef struct {
    struct host_task *pThread;
    PVOID pThreadFunc, pThreadData;
    char threadName[32];
} OSAL_THREAD, *P_OSAL_THREAD;

/* SOURCE_TYPES */
/* SOURCE_RINGS */
/* SOURCE_STATES */

typedef struct { int ldoStableTime, rstStableTime, onStableTime, offStableTime, rtcStableTime; } PWR_SEQ_TIME;
struct vendor_patch_table { char **active_version; void *patch; int num; };
typedef struct {
    OSAL_THREAD thread, worker_thread;
    OSAL_TIMER worker_timer, utc_sync_timer;
    struct work_struct wmtd_worker_thread_work, utcSyncWorker;
    int wmtd_op_history, worker_op_history, hw_ver;
    OSAL_EVENT rWmtdWq, rWmtdWorkerWq, rWmtRxWq, cmdReq;
    OSAL_SIGNAL cmdResp;
    OSAL_SLEEPABLE_LOCK psm_lock, idc_lock, wlan_lock, assert_lock, mpu_lock;
    OSAL_OP_Q rFreeOpQ, rActiveOpQ, rWorkerOpQ;
    OSAL_OP arQue[WMT_OP_BUF_SIZE];
    P_OSAL_OP pCurOP, pWorkerOP;
    struct { unsigned long data; } state;
    struct { void *fDrvRst[WMTDRV_TYPE_WIFI]; } rFdrvCb;
    struct { int cfgExist, co_clock_flag, pwr_on_ldo_slot, pwr_on_rst_slot,
                 pwr_on_on_slot, pwr_on_off_slot, pwr_on_rtc_slot; } rWmtGenConf;
    struct vendor_patch_table patch_table;
} DEV_WMT, *P_DEV_WMT;
static DEV_WMT gDevWmt;
static struct { struct work_struct work; } wmt_assert_work;
/* SOURCE_PROTOTYPES */
/* The concurrent callback barrier is exercised by test_wmt_callback_lifetime.py. */
static void wmt_export_platform_bridge_unregister(void) {}

struct gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool enabled, arrived, released;
};
#define GATE_INIT {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, false, false}
static struct gate enqueue_gate = GATE_INIT, handler_gate = GATE_INIT, recycle_gate = GATE_INIT;
static struct gate idle_entered = GATE_INIT, stop_entered = GATE_INIT, pool_contended = GATE_INIT;
static struct gate worker_queued = GATE_INIT;
static atomic_int allow_regular, allow_worker, live_allocations, allocation_frees, clears;
static atomic_int worker_wakeups, signal_calls, destroyed_pool_locks;
static int fail_core_init, fail_plat_init, fail_worker_run;
static bool pause_wifi_dispatch, pause_worker;
static _Thread_local bool teardown_thread;

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
static void mutex_lock(struct mutex *lock)
{
    assert(lock->initialized);
    if (teardown_thread && lock == &g_wmt_op_pool_lock) {
        int ret = pthread_mutex_trylock(&lock->native);
        if (!ret) return;
        assert(ret == EBUSY);
        mark(&pool_contended);
    }
    assert(pthread_mutex_lock(&lock->native) == 0);
}
static void mutex_unlock(struct mutex *lock) { assert(pthread_mutex_unlock(&lock->native) == 0); }
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; mutex_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); mutex_unlock(lock); } while (0)
static void wake_up(wait_queue_head_t *queue)
{
    pthread_mutex_lock(&queue->lock);
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
}
#define wait_event(queue, condition) do { \
    pthread_mutex_lock(&(queue).lock); mark(&idle_entered); \
    while (!(condition)) pthread_cond_wait(&(queue).cond, &(queue).lock); \
    pthread_mutex_unlock(&(queue).lock); \
} while (0)
static void *osal_malloc(SIZE_T size)
{
    void *pointer = malloc(size);
    assert(pointer);
    atomic_inc(&live_allocations);
    return pointer;
}
static void osal_free(PVOID pointer)
{
    if (!pointer) return;
    assert(atomic_fetch_sub(&live_allocations, 1) > 0);
    atomic_inc(&allocation_frees);
    free(pointer);
}
static void *osal_memset(void *dest, int byte, size_t size)
{
    if (dest == &gDevWmt) {
        assert(size == sizeof(gDevWmt));
        assert(atomic_read(&g_wmt_ops_checked_out) == 0);
        assert(atomic_read(&live_allocations) == 0);
        atomic_inc(&clears);
    }
    return memset(dest, byte, size);
}
static void osal_sleepable_lock_init(P_OSAL_SLEEPABLE_LOCK lock)
{ assert(pthread_mutex_init(&lock->lock.native, NULL) == 0); lock->lock.initialized = true; }
static void osal_sleepable_lock_deinit(P_OSAL_SLEEPABLE_LOCK lock)
{
    bool pool = lock == &gDevWmt.rFreeOpQ.sLock || lock == &gDevWmt.rActiveOpQ.sLock ||
                lock == &gDevWmt.rWorkerOpQ.sLock;
    if (pool) {
        assert(lock->lock.initialized);
        assert(atomic_read(&g_wmt_ops_checked_out) == 0);
        atomic_inc(&destroyed_pool_locks);
    }
    assert(lock->lock.initialized);
    assert(pthread_mutex_destroy(&lock->lock.native) == 0);
    lock->lock.initialized = false;
}
static int osal_unlock_sleepable_lock(P_OSAL_SLEEPABLE_LOCK lock)
{
    mutex_unlock(&lock->lock);
    if (lock == &gDevWmt.rFreeOpQ.sLock) pause_at(&recycle_gate);
    return 0;
}
static void osal_event_init(P_OSAL_EVENT event)
{
    pthread_mutex_init(&event->waitQueue.lock, NULL);
    pthread_cond_init(&event->waitQueue.cond, NULL);
    event->initialized = true;
}
static void osal_event_deinit(P_OSAL_EVENT event)
{
    if (!event->initialized) return;
    assert(pthread_mutex_destroy(&event->waitQueue.lock) == 0);
    assert(pthread_cond_destroy(&event->waitQueue.cond) == 0);
    event->initialized = false;
}
static void osal_trigger_event(P_OSAL_EVENT event)
{
    assert(event->initialized);
    if (event == &gDevWmt.rWmtdWq) pause_at(&enqueue_gate);
    else if (event == &gDevWmt.rWmtdWorkerWq) {
        atomic_inc(&worker_wakeups);
        mark(&worker_queued);
    }
    wake_up(&event->waitQueue);
}
static void *thread_start(void *input)
{
    P_OSAL_THREAD thread = input;
    struct host_task *task = thread->pThread;
    pthread_mutex_lock(&task->lock);
    while (!task->run && !atomic_read(&task->stop)) pthread_cond_wait(&task->cond, &task->lock);
    bool run = task->run;
    pthread_mutex_unlock(&task->lock);
    if (run) ((INT32 (*)(PVOID))thread->pThreadFunc)(thread->pThreadData);
    return NULL;
}
static int osal_thread_create(P_OSAL_THREAD thread)
{
    struct host_task *task = calloc(1, sizeof(*task));
    assert(task);
    pthread_mutex_init(&task->lock, NULL);
    pthread_cond_init(&task->cond, NULL);
    thread->pThread = task;
    return pthread_create(&task->thread, NULL, thread_start, thread);
}
static int osal_thread_run(P_OSAL_THREAD thread)
{
    if (fail_worker_run && thread == &gDevWmt.worker_thread) return -1;
    pthread_mutex_lock(&thread->pThread->lock);
    thread->pThread->run = true;
    pthread_cond_broadcast(&thread->pThread->cond);
    pthread_mutex_unlock(&thread->pThread->lock);
    return 0;
}
static int osal_thread_should_stop(P_OSAL_THREAD thread) { return atomic_read(&thread->pThread->stop); }
static int osal_thread_wait_for_event(P_OSAL_THREAD thread, P_OSAL_EVENT event,
                                    UINT32 (*checker)(P_OSAL_THREAD))
{
    bool worker = thread == &gDevWmt.worker_thread;
    P_OSAL_OP_Q queue = worker ? &gDevWmt.rWorkerOpQ : &gDevWmt.rActiveOpQ;
    pthread_mutex_lock(&event->waitQueue.lock);
    while (!osal_thread_should_stop(thread)) {
        bool ready = false;
        if (atomic_read(worker ? &allow_worker : &allow_regular)) {
            /* Serialize the host predicate with ring mutation to model a kernel waitqueue read. */
            mutex_lock(&queue->sLock.lock);
            ready = checker(thread);
            mutex_unlock(&queue->sLock.lock);
        }
        if (ready) break;
        pthread_cond_wait(&event->waitQueue.cond, &event->waitQueue.lock);
    }
    pthread_mutex_unlock(&event->waitQueue.lock);
    return 0;
}
static int osal_thread_stop(P_OSAL_THREAD thread)
{
    struct host_task *task = thread->pThread;
    mark(&stop_entered);
    if (!task) return 0;
    assert(!task->joined);
    atomic_set(&task->stop, 1);
    pthread_mutex_lock(&task->lock);
    pthread_cond_broadcast(&task->cond);
    pthread_mutex_unlock(&task->lock);
    OSAL_EVENT *event = thread == &gDevWmt.thread ? &gDevWmt.rWmtdWq : &gDevWmt.rWmtdWorkerWq;
    if (event->initialized) wake_up(&event->waitQueue);
    assert(pthread_join(task->thread, NULL) == 0);
    task->joined = true;
    return 0;
}
static int osal_thread_destroy(P_OSAL_THREAD thread)
{
    if (!thread->pThread) return 0;
    /* Production destroy performs the single join and clears the handle. */
    assert(osal_thread_stop(thread) == 0);
    pthread_mutex_destroy(&thread->pThread->lock);
    pthread_cond_destroy(&thread->pThread->cond);
    free(thread->pThread);
    thread->pThread = NULL;
    return 0;
}
static pthread_mutex_t signal_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t signal_cond = PTHREAD_COND_INITIALIZER;
static void osal_signal_init(P_OSAL_SIGNAL signal) { atomic_set(&signal->done, 0); }
static void osal_signal_deinit(P_OSAL_SIGNAL signal) { signal->timeoutValue = 0; }
static int osal_op_is_wait_for_signal(P_OSAL_OP operation) { return operation->signal.timeoutValue != 0; }
static void osal_op_raise_signal(P_OSAL_OP operation, int result)
{
    operation->result = result;
    pthread_mutex_lock(&signal_lock);
    atomic_set(&operation->signal.done, 1);
    atomic_inc(&signal_calls);
    pthread_cond_broadcast(&signal_cond);
    pthread_mutex_unlock(&signal_lock);
}
static int osal_wait_for_signal_timeout(P_OSAL_SIGNAL signal, P_OSAL_THREAD thread)
{
    struct timespec end;
    clock_gettime(CLOCK_REALTIME, &end);
    end.tv_nsec += (long)signal->timeoutValue * 1000000;
    end.tv_sec += end.tv_nsec / 1000000000;
    end.tv_nsec %= 1000000000;
    pthread_mutex_lock(&signal_lock);
    while (!atomic_read(&signal->done)) {
        int ret = pthread_cond_timedwait(&signal_cond, &signal_lock, &end);
        if (ret == ETIMEDOUT) break;
        assert(ret == 0);
    }
    int done = atomic_read(&signal->done);
    pthread_mutex_unlock(&signal_lock);
    return done;
}
static int wmt_core_opid(P_OSAL_OP_DAT data)
{
    if (data->opId == WMT_OPID_FUNC_ON) {
        if (pause_wifi_dispatch) pause_at(&handler_gate);
        P_OSAL_OP operation = wmt_lib_get_current_op(&gDevWmt);
        WRITE_ONCE(data->opId, WMT_OPID_WLAN_PROBE);
        return wmt_lib_put_worker_op(operation) ? 0 : -4;
    }
    if (pause_worker && data->opId == WMT_OPID_WLAN_PROBE) pause_at(&handler_gate);
    memset((void *)data->au4OpData[0], 0x6b, 32);
    return 0;
}
static int mtk_wcn_stp_coredump_start_get(void) { return 0; }
static int wmt_detect_get_chip_type(void) { return WMT_CHIP_TYPE_SOC; }
static int wmt_conf_read_file(void) { return 0; }
static int wmt_conf_deinit(void) { return 0; }
static int wmt_core_init(void) { return fail_core_init ? -1 : 0; }
static int wmt_core_deinit(void) { return 0; }
static int wmt_plat_init(PWR_SEQ_TIME *sequence, int flags) { return fail_plat_init ? -1 : 0; }
static int wmt_plat_deinit(void) { return 0; }
static int wmt_plat_soc_co_clock_flag_get(void) { return 0; }
static void osal_op_history_init(int *history, int count) { *history = count; }
static void osal_op_history_deinit(int *history) { *history = 0; }
static void osal_op_history_save(int *history, P_OSAL_OP operation) { (void)history; }
static void osal_opq_dump(char *name, P_OSAL_OP_Q queue) { (void)name; }
static void wmt_lib_print_wmtd_op_history(void) {}
static void wmt_lib_print_worker_op_history(void) {}
static void wmt_lib_rom_patch_info_free(void) {}
/* Broker behavior is covered by test_wmt_command_v2.py. */
static void wmt_lib_cmd_start(void) {}
static void wmt_lib_cmd_shutdown(void) {}
static void wmt_dev_patch_info_free(void)
{
    assert(!gDevWmt.thread.pThread && !gDevWmt.worker_thread.pThread);
    assert(atomic_read(&g_wmt_ops_checked_out) == 0);
}
static void mtk_wcn_wmt_system_state_reset(void) {}
#define osal_test_bit(bit, state) (((state)->data >> (bit)) & 1)
#define wmt_plat_irq_cb_reg(callback) ((void)0)
#define wmt_plat_aif_cb_reg(callback) ((void)0)
#define wmt_plat_func_ctrl_cb_reg(callback) ((void)0)
#define wmt_plat_deep_idle_ctrl_cb_reg(callback) ((void)0)
#define WMT_STEP_DEINIT_FUNC() ((void)0)
#define INIT_WORK(work, callback) ((work)->initialized = true)
static void wmt_lib_wmtd_worker_thread_timeout_handler(ULONG data) {}
static void wmt_lib_utc_sync_timeout_handler(ULONG data) {}
static void osal_timer_create(OSAL_TIMER *timer) { timer->initialized = true; }
static void osal_timer_start(OSAL_TIMER *timer, UINT32 duration) { assert(timer->initialized); }
static void osal_timer_stop(OSAL_TIMER *timer) { assert(timer->initialized); }
static void osal_timer_stop_sync(OSAL_TIMER *timer) { assert(timer->initialized); timer->stopped = true; }
static void cancel_work_sync(struct work_struct *work)
{
    assert(work->initialized);
    OSAL_TIMER *timer = work == &gDevWmt.utcSyncWorker ? &gDevWmt.utc_sync_timer : &gDevWmt.worker_timer;
    if (work != &wmt_assert_work.work) assert(timer->stopped);
    work->cancelled = true;
}

/* PRODUCTION */

static P_OSAL_OP borrow(UINT32 timeout)
{
    P_OSAL_OP operation = wmt_lib_get_free_op();
    assert(operation);
    unsigned char *payload = wmt_lib_alloc_op_data(operation, 32);
    assert(payload);
    memset(payload, 0x3d, 32);
    operation->op.opId = WMT_OPID_LPBK;
    operation->op.au4OpData[0] = (SIZE_T)payload;
    operation->signal.timeoutValue = timeout;
    return operation;
}
static void *shutdown_call(void *unused)
{
    teardown_thread = true;
    assert(wmt_lib_deinit() == 0);
    return NULL;
}
static pthread_t start_shutdown(void)
{
    pthread_t thread;
    assert(pthread_create(&thread, NULL, shutdown_call, NULL) == 0);
    return thread;
}
static void join(pthread_t thread) { assert(pthread_join(thread, NULL) == 0); }
static void *submit_call(void *operation) { assert(wmt_lib_put_act_op(operation)); return NULL; }
static void *release_call(void *operation) { wmt_lib_put_op_ref(operation); return NULL; }
static void *reset_call(void *unused) { wmt_lib_state_init(); return NULL; }
static void still_borrowed(int allocations)
{
    assert(atomic_read(&clears) == 1);
    assert(atomic_read(&g_wmt_ops_checked_out) == 1);
    assert(atomic_read(&live_allocations) == allocations);
    assert(atomic_read(&destroyed_pool_locks) == 0);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    int test = atoi(argv[1]);
    P_OSAL_OP operation;
    pthread_t teardown, sender;
    OSAL_OP_DAT result;
    assert(wmt_lib_get_free_op() == NULL);
    if (test >= 11 && test <= 13) {
        fail_core_init = test == 11;
        fail_plat_init = test == 12;
        fail_worker_run = test == 13;
        assert(wmt_lib_init() < 0);
        assert(wmt_lib_get_free_op() == NULL);
        assert(wmt_lib_deinit() == 0);
        assert(atomic_read(&destroyed_pool_locks) == (test == 11 ? 0 : 3));
    } else {
        atomic_set(&allow_regular, test == 1 || test == 2 || test == 7 || test == 8 || test == 10);
        atomic_set(&allow_worker, test == 8);
        assert(wmt_lib_init() == 0);
        if (test == 0 || test == 9 || test == 14) {
            operation = borrow(0);
            assert(wmt_lib_put_act_op(operation));
            assert(wmt_lib_deinit() == 0);
            if (test == 9) {
                wmt_lib_state_init();
                assert(wmt_lib_get_free_op() == NULL);
            }
            if (test == 14) {
                assert(wmt_lib_init() == 0);
                operation = borrow(0);
                assert(wmt_lib_put_act_op(operation));
                assert(wmt_lib_deinit() == 0);
            }
        } else if (test == 1 || test == 7 || test == 8) {
            operation = borrow(0);
            operation->op.opId = WMT_OPID_FUNC_ON;
            pause_wifi_dispatch = test == 7;
            pause_worker = test == 8;
            handler_gate.enabled = test != 1;
            assert(wmt_lib_put_act_op(operation));
            if (test == 1) {
                await(&worker_queued);
                assert(wmt_lib_deinit() == 0);
            } else {
                await(&handler_gate);
                teardown = start_shutdown();
                await(&stop_entered);
                still_borrowed(1);
                release(&handler_gate);
                join(teardown);
                assert(atomic_read(&worker_wakeups) == (test == 7 ? 0 : 1));
            }
        } else if (test == 2 || test == 3 || test == 4 || test == 5) {
            operation = borrow(test == 2 ? 2000 : 1);
            if (test == 2 || test == 3)
                assert(wmt_lib_submit_op_result(operation, &result) == (test == 2));
            teardown = start_shutdown();
            await(&idle_entered);
            still_borrowed(1);
            assert(wmt_lib_get_free_op() == NULL);
            if (test == 4) {
                /* A borrower paused before waking the device can still unwind after closure. */
                mutex_lock(&gDevWmt.psm_lock.lock);
                assert(!wmt_lib_submit_op_result(operation, &result));
                mutex_unlock(&gDevWmt.psm_lock.lock);
                assert(RB_EMPTY(&gDevWmt.rActiveOpQ));
                assert(atomic_read(&operation->ref_count) == 1);
                assert(result.au4OpData[0] == operation->op.au4OpData[0]);
            }
            if (test == 2) {
                unsigned char *payload = operation->wmt_payload;
                for (size_t i = 0; i < 32; i++) assert(payload[i] == 0x6b);
            }
            if (test == 3) {
                assert(operation->result == -1 && atomic_read(&signal_calls) == 1);
                assert(atomic_read(&operation->ref_count) == 1);
            }
            if (test == 5) assert(wmt_lib_put_op_to_free_queue(operation) == 0);
            else wmt_lib_put_op_ref(operation);
            join(teardown);
        } else if (test == 6) {
            operation = borrow(0);
            enqueue_gate.enabled = true;
            assert(pthread_create(&sender, NULL, submit_call, operation) == 0);
            await(&enqueue_gate);
            teardown = start_shutdown();
            await(&pool_contended);
            assert(atomic_read(&clears) == 1);
            pthread_mutex_lock(&stop_entered.lock);
            assert(!stop_entered.arrived);
            pthread_mutex_unlock(&stop_entered.lock);
            release(&enqueue_gate);
            join(sender);
            join(teardown);
        } else if (test == 10 || test == 15) {
            operation = borrow(test == 10 ? 2000 : 0);
            if (test == 10) assert(wmt_lib_submit_op_result(operation, &result));
            else assert(wmt_lib_put_act_op(operation));
            recycle_gate.enabled = true;
            assert(pthread_create(&sender, NULL, test == 10 ? release_call : reset_call, operation) == 0);
            await(&recycle_gate);
            teardown = start_shutdown();
            await(&idle_entered);
            still_borrowed(0);
            release(&recycle_gate);
            join(sender);
            join(teardown);
        } else abort();
    }
    assert(atomic_read(&live_allocations) == 0 && atomic_read(&g_wmt_ops_checked_out) == 0);
    assert(atomic_read(&clears) == (test == 14 ? 4 : 2));
    assert(!g_wmt_op_pool_initialized && g_wmt_op_pool_stopping);
    assert(!g_wmt_worker_timer_initialized && !g_wmt_utc_timer_initialized);
    puts("PASS");
    return 0;
}
