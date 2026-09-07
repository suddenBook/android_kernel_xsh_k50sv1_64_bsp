/* Host queue/completion substitutes; production helpers follow this block. */
#include <sched.h>
typedef struct {
    int rActiveOpQ, rFreeOpQ, rWorkerOpQ, rWmtdWq, rWmtdWorkerWq, worker_thread, thread;
    P_OSAL_OP pCurOP, pWorkerOP;
} DEVICE, *P_DEV_WMT;
static DEVICE gDevWmt;
static OSAL_OP operation;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_changed = PTHREAD_COND_INITIALIZER;
static pthread_barrier_t start;
static bool queued, consumer_started, signalled, pool_free;
static atomic_bool sender_finished, stopping;
static atomic_int signal_calls, failures;
static int scenario;
static VOID wmt_lib_complete_op(P_OSAL_OP op, INT32 result);
static VOID wmt_lib_cancel_current_op(P_DEV_WMT dev);
static INT32 wmt_lib_set_current_op(P_DEV_WMT dev, P_OSAL_OP op);
#define atomic_set(p, v) atomic_store(p, v)
#define atomic_inc(p) atomic_fetch_add(p, 1)
#define atomic_dec(p) atomic_fetch_sub(p, 1)
#define atomic_dec_and_test(p) (atomic_fetch_sub(p, 1) == 1)
static int mtk_wcn_stp_coredump_start_get(void) { return 0; }
static void osal_trigger_event(int *event) { (void)event; }
static void osal_signal_init(P_OSAL_SIGNAL signal)
{
    (void)signal;
    pthread_mutex_lock(&queue_lock);
    signalled = false;
    pthread_mutex_unlock(&queue_lock);
}
static int osal_op_is_wait_for_signal(P_OSAL_OP op)
{ return op && op->signal.timeoutValue; }
static void osal_op_raise_signal(P_OSAL_OP op, int result)
{
    op->result = result;
    atomic_fetch_add(&signal_calls, 1);
    pthread_mutex_lock(&queue_lock);
    signalled = true;
    pthread_cond_broadcast(&queue_changed);
    pthread_mutex_unlock(&queue_lock);
}
static int wmt_lib_put_op(int *queue, P_OSAL_OP op)
{
    pthread_mutex_lock(&queue_lock);
    assert(op == &operation);
    if (queue == &gDevWmt.rActiveOpQ) {
        queued = true;
    } else {
        assert(queue == &gDevWmt.rFreeOpQ && atomic_load(&op->ref_count) == 0);
        pool_free = true;
    }
    pthread_cond_broadcast(&queue_changed);
    pthread_mutex_unlock(&queue_lock);
    return true;
}
static int osal_wait_for_signal_timeout(P_OSAL_SIGNAL signal, int *thread)
{
    (void)signal; (void)thread;
    pthread_mutex_lock(&queue_lock);
    while (!consumer_started) pthread_cond_wait(&queue_changed, &queue_lock);
    pthread_mutex_unlock(&queue_lock);
    wmt_lib_cancel_current_op(&gDevWmt);
    pthread_mutex_lock(&queue_lock);
    while (!signalled) pthread_cond_wait(&queue_changed, &queue_lock);
    pthread_mutex_unlock(&queue_lock);
    return 1;
}

/* TESTS */
static void *consumer(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&queue_lock);
    while (!queued) pthread_cond_wait(&queue_changed, &queue_lock);
    pthread_mutex_unlock(&queue_lock);
    wmt_lib_set_current_op(&gDevWmt, &operation);
    pthread_mutex_lock(&queue_lock);
    consumer_started = true;
    pthread_cond_broadcast(&queue_changed);
    pthread_mutex_unlock(&queue_lock);
    for (unsigned long i = 0; !atomic_load(&sender_finished) || i < 1000; i++)
        operation.op.au4OpData[0] = i;
    wmt_lib_set_current_op(&gDevWmt, NULL);
    wmt_lib_complete_op(&operation, 0);
    return NULL;
}

static void *publisher(void *unused)
{
    (void)unused;
    pthread_barrier_wait(&start);
    for (unsigned int i = 0; i < 20000; i++) {
        memset(&operation, 0, sizeof(operation));
        operation.signal.timeoutValue = 2000;
        operation.op.opId = WMT_OPID_LPBK;
        operation.result = 47;
        atomic_store(&operation.ref_count, 1);
        wmt_lib_set_current_op(&gDevWmt, &operation);
        wmt_lib_set_worker_op(&gDevWmt, &operation);
        /* The core changes the ID before transferring Wi-Fi work. */
        WRITE_ONCE(operation.op.opId, WMT_OPID_WLAN_PROBE);
        if ((i & 7) == 0) sched_yield();
        wmt_lib_set_current_op(&gDevWmt, NULL);
        wmt_lib_set_worker_op(&gDevWmt, NULL);
    }
    atomic_store(&stopping, true);
    return NULL;
}

static void *observer(void *unused)
{
    (void)unused;
    pthread_barrier_wait(&start);
    while (!atomic_load(&stopping)) {
        wmt_lib_cancel_current_op(&gDevWmt);
        for (int worker = 0; worker < 2; worker++) {
            UINT32 id = wmt_lib_active_op_id(&gDevWmt, worker);
            if (id != (UINT32)-1 && id != WMT_OPID_LPBK && id != WMT_OPID_WLAN_PROBE)
                atomic_fetch_add(&failures, 1);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = atoi(argv[1]);
    if (scenario == 0) {
        pthread_t worker;
        operation.signal.timeoutValue = 2000;
        operation.op.opId = WMT_OPID_LPBK;
        operation.op.au4OpData[0] = 17;
        assert(pthread_create(&worker, NULL, consumer, NULL) == 0);
        OSAL_OP_DAT result;
        int success = wmt_lib_put_act_op_result(&operation, &result);
        atomic_store(&sender_finished, true);
        assert(pthread_join(worker, NULL) == 0);
        printf("success=%d output=%lu signals=%d pool_free=%d\n",
               success, result.au4OpData[0], atomic_load(&signal_calls), pool_free);
        return success || result.au4OpData[0] != 17 || atomic_load(&signal_calls) != 1 || !pool_free;
    }
    pthread_t owner, readers[2];
    assert(pthread_barrier_init(&start, NULL, 3) == 0);
    assert(pthread_create(&owner, NULL, publisher, NULL) == 0);
    for (int i = 0; i < 2; i++) assert(pthread_create(&readers[i], NULL, observer, NULL) == 0);
    assert(pthread_join(owner, NULL) == 0);
    for (int i = 0; i < 2; i++) assert(pthread_join(readers[i], NULL) == 0);
    assert(pthread_barrier_destroy(&start) == 0);
    printf("reuses=20000 signals=%d failures=%d\n", atomic_load(&signal_calls), atomic_load(&failures));
    return atomic_load(&failures) || gDevWmt.pCurOP || gDevWmt.pWorkerOP;
}
