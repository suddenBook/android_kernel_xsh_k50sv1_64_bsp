/* Host-only queue and completion substitutes for production operation helpers. */
#include <pthread.h>
#include <sched.h>

typedef struct { int rActiveOpQ, rFreeOpQ, rWorkerOpQ, rWmtdWq, rWmtdWorkerWq, worker_thread, thread; } DEVICE, *P_DEV_WMT;
static DEVICE gDevWmt;
static OSAL_OP operation;
static P_OSAL_OP queued_operation;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_changed = PTHREAD_COND_INITIALIZER;
static pthread_barrier_t start;
static bool pool_owned, completed, stopping;
static unsigned int completed_count;
static atomic_int failures;

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
    completed = false;
    pthread_mutex_unlock(&queue_lock);
}
static int wmt_lib_put_op(int *queue, P_OSAL_OP op)
{
    pthread_mutex_lock(&queue_lock);
    assert(op == &operation && pool_owned);
    if (queue == &gDevWmt.rActiveOpQ) {
        assert(!queued_operation);
        queued_operation = op;
    } else {
        assert(queue == &gDevWmt.rFreeOpQ && atomic_load(&op->ref_count) == 0);
        pool_owned = false;
    }
    pthread_cond_broadcast(&queue_changed);
    pthread_mutex_unlock(&queue_lock);
    return true;
}
static int osal_wait_for_signal_timeout(P_OSAL_SIGNAL signal, int *thread)
{
    (void)signal; (void)thread;
    pthread_mutex_lock(&queue_lock);
    while (!completed) pthread_cond_wait(&queue_changed, &queue_lock);
    pthread_mutex_unlock(&queue_lock);
    return 1;
}
static int osal_op_is_wait_for_signal(P_OSAL_OP op)
{
    return op->signal.timeoutValue != 0;
}
static void osal_op_raise_signal(P_OSAL_OP op, int result)
{
    op->result = result;
    pthread_mutex_lock(&queue_lock);
    completed = true;
    pthread_cond_broadcast(&queue_changed);
    pthread_mutex_unlock(&queue_lock);
}

/* TESTS */
static void *consumer(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&queue_lock);
        while (!queued_operation && !stopping) pthread_cond_wait(&queue_changed, &queue_lock);
        if (stopping) {
            pthread_mutex_unlock(&queue_lock);
            return NULL;
        }
        P_OSAL_OP op = queued_operation;
        queued_operation = NULL;
        pthread_mutex_unlock(&queue_lock);
        op->op.au4OpData[0] ^= 0x20000000UL;
        wmt_lib_complete_op(op, 0);
        completed_count++;
    }
}

static void *sender(void *argument)
{
    unsigned long identity = (uintptr_t)argument;
    pthread_barrier_wait(&start);
    for (unsigned long round = 0; round < 1000; round++) {
        unsigned long value = (identity << 20) | round;
        pthread_mutex_lock(&queue_lock);
        while (pool_owned) pthread_cond_wait(&queue_changed, &queue_lock);
        pool_owned = true;
#if HAS_OP_POOL
        atomic_fetch_add(&g_wmt_ops_checked_out, 1);
#endif
        /* Production get_free_op also clears the complete released slot. */
        memset(&operation, 0, sizeof(operation));
        pthread_mutex_unlock(&queue_lock);
        operation.op.opId = WMT_OPID_LPBK;
        operation.op.au4OpData[0] = value;
        operation.signal.timeoutValue = 2000;
        OSAL_OP_DAT result;
        int success = wmt_lib_put_act_op_result(&operation, &result);
        if (!success || result.au4OpData[0] != (value ^ 0x20000000UL)) {
            atomic_fetch_add(&failures, 1);
            return NULL;
        }
    }
    return NULL;
}

int main(void)
{
    pthread_t worker, producers[2];
    assert(pthread_barrier_init(&start, NULL, 2) == 0);
    assert(pthread_create(&worker, NULL, consumer, NULL) == 0);
    for (uintptr_t i = 0; i < 2; i++)
        assert(pthread_create(&producers[i], NULL, sender, (void *)(i + 1)) == 0);
    for (int i = 0; i < 2; i++) assert(pthread_join(producers[i], NULL) == 0);
    pthread_mutex_lock(&queue_lock);
    stopping = true;
    pthread_cond_broadcast(&queue_changed);
    pthread_mutex_unlock(&queue_lock);
    assert(pthread_join(worker, NULL) == 0);
    assert(pthread_barrier_destroy(&start) == 0);
    printf("completed=%u failures=%d pool_owned=%d\n", completed_count, atomic_load(&failures), pool_owned);
    return atomic_load(&failures) || completed_count != 2000 || pool_owned;
}
