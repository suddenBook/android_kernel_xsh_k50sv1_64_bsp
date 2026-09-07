/* Host scheduler/allocation substitutes; production functions are injected below. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

typedef void VOID;
typedef void *PVOID;
typedef int INT32;
typedef unsigned int UINT32;
typedef unsigned char UINT8;
typedef unsigned char *PUINT8;
typedef unsigned long ULONG;
typedef unsigned long long UINT64;
typedef size_t SIZE_T;
typedef char INT8;
typedef char *PINT8;
typedef bool MTK_WCN_BOOL;
typedef int MTKSTP_PSM_ACTION_T;
typedef atomic_int atomic_t;
#define PAGE_SIZE 4096
#define MAX_THREAD_NAME_LEN 16
#define MAX_WAKE_LOCK_NAME_LEN 32
#define GFP_ATOMIC 1
#define GFP_KERNEL 2
#define pr_err(...) ((void)0)
#define pr_info(...) ((void)0)
#define STP_PSM_PR_ERR(...) ((void)0)
#define STP_PSM_PR_DBG(...) ((void)0)
#define STP_PSM_PR_INFO(...) ((void)0)
#define STP_PSM_PR_LOUD(...) ((void)0)
#define osal_assert(v) do { if (!(v)) abort(); } while (0)
#define osal_strlen(s) strlen((char *)(s))
#define osal_memcpy memcpy
#define osal_memset memset
#define osal_sizeof sizeof
#define osal_ftrace_print(...) ((void)0)
#define is_power_of_2(v) ((v) && !((v) & ((v) - 1)))
#define ERR_PTR(err) ((void *)(intptr_t)(err))
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define do_gettimeofday(p) gettimeofday((p), NULL)
struct { unsigned int pid; } host_current = {42};
#define current (&host_current)
static void osal_get_local_time(UINT64 *sec, ULONG *usec) { *sec=1; *usec=2; }

static atomic_int allocations[6], dynamic_locks, event_count, signal_count;
static atomic_int thread_count, thread_create_calls, debug_alloc_calls;
static atomic_bool worker_entry_checked, records_ready_at_start;
static atomic_bool reader_paused, release_reader, thread_stop_entered, timer_stop_entered;
static atomic_bool modify_paused, release_modify, monitor_contended;
static atomic_bool request_reader, deinit_done;
static atomic_bool synchronize_entered;
static void *paused_record;
static int scenario, fault;
#define REQUIRE(v) do { if (!(v)) { fprintf(stderr, "line %d: %s (alloc=%d/%d/%d/%d/%d/%d locks=%d events=%d signals=%d threads=%d)\n", \
    __LINE__, #v, allocations[0], allocations[1], allocations[2], allocations[3], allocations[4], allocations[5], \
    dynamic_locks, event_count, signal_count, thread_count); exit(1); } } while (0)
struct allocation { max_align_t alignment; unsigned int kind; };
static void *tracked_alloc(size_t bytes, unsigned int kind)
{
    struct allocation *p = calloc(1, sizeof(*p)+bytes);
    if (p) { p->kind=kind; allocations[kind]++; return p+1; }
    return NULL;
}
static void kfree(void *ptr)
{
    if (!ptr) return;
    struct allocation *p = (struct allocation *)ptr-1;
    allocations[p->kind]--; free(p);
}
static void *kzalloc(size_t bytes, int flags) { return fault==1 ? NULL : tracked_alloc(bytes,0); }
static void *kmalloc(size_t bytes, int flags)
{
    int n=atomic_fetch_add(&debug_alloc_calls,1)+1;
    if ((fault==4 && n==1) || (fault==5 && n==2)) return NULL;
    return tracked_alloc(bytes,3);
}
#define vmalloc(n) kmalloc((n), GFP_KERNEL)
#define kvfree kfree

typedef pthread_mutex_t spinlock_t;
struct mutex { pthread_mutex_t native; bool initialized; };
#define DEFINE_MUTEX(name) struct mutex name = { PTHREAD_MUTEX_INITIALIZER, true }
#define DEFINE_SPINLOCK(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
static void mutex_lock(struct mutex *m) { pthread_mutex_lock(&m->native); }
static void mutex_unlock(struct mutex *m) { pthread_mutex_unlock(&m->native); }
#define __rcu
static pthread_rwlock_t record_rcu = PTHREAD_RWLOCK_INITIALIZER;
static void rcu_read_lock(void) { pthread_rwlock_rdlock(&record_rcu); }
static void rcu_read_unlock(void) { pthread_rwlock_unlock(&record_rcu); }
#define rcu_dereference(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define rcu_dereference_protected(p, condition) rcu_dereference(p)
#define rcu_assign_pointer(p, value) __atomic_store_n(&(p), (value), __ATOMIC_RELEASE)
#define RCU_INIT_POINTER(p, value) rcu_assign_pointer(p, value)
static void synchronize_rcu(void)
{
    synchronize_entered=true;
    pthread_rwlock_wrlock(&record_rcu);
    pthread_rwlock_unlock(&record_rcu);
}
static void host_spin_lock(spinlock_t *lock);
#define spin_lock_irqsave(lock, flags) do { (flags)=0; host_spin_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) pthread_mutex_unlock(lock)
typedef struct { struct mutex lock; } OSAL_SLEEPABLE_LOCK;
typedef struct { spinlock_t lock; ULONG flag; bool initialized; } OSAL_UNSLEEPABLE_LOCK;
typedef struct { ULONG data; OSAL_UNSLEEPABLE_LOCK opLock; } OSAL_BIT_OP_VAR;
typedef struct { bool initialized; unsigned int timeoutValue; } OSAL_SIGNAL;
typedef struct { bool initialized; unsigned int timeoutValue; } OSAL_EVENT;
typedef struct { UINT32 opId, u4InfoBit; SIZE_T au4OpData[8]; } OSAL_OP_DAT, *P_OSAL_OP_DAT;
typedef struct { OSAL_OP_DAT op; OSAL_SIGNAL signal; atomic_t ref_count; } OSAL_OP, *P_OSAL_OP;
typedef struct { UINT32 read,write,size; P_OSAL_OP queue[16]; } OSAL_OP_Q, *P_OSAL_OP_Q;
#define RB_INIT(q,n) do { (q)->read=(q)->write=0; (q)->size=(n); } while (0)
struct osal_op_history { void *queue, *snapshot; };
typedef struct {
    VOID (*timeoutHandler)(ULONG);
    ULONG timeroutHandlerData;
    bool initialized;
    atomic_bool pending, running;
} OSAL_TIMER;
struct task_struct {
    pthread_t native;
    int (*fn)(void *);
    void *data;
    atomic_bool go, stop;
};
struct wakeup_source { int active; };
struct kfifo { unsigned char *data; unsigned int size,in,out; };

/* TYPES */
/* GLOBALS */

static void host_spin_lock(spinlock_t *lock)
{
    if (pthread_mutex_trylock(lock)==0) return;
#if HAS_MONITOR_LOCK
    if (lock==&g_psm_monitor_lock) monitor_contended=true;
#endif
    pthread_mutex_lock(lock);
}
static int osal_sleepable_lock_init(OSAL_SLEEPABLE_LOCK *lock)
{ pthread_mutex_init(&lock->lock.native,NULL); lock->lock.initialized=true; dynamic_locks++; return 0; }
static int osal_sleepable_lock_deinit(OSAL_SLEEPABLE_LOCK *lock)
{ REQUIRE(lock->lock.initialized); pthread_mutex_destroy(&lock->lock.native); lock->lock.initialized=false; dynamic_locks--; return 0; }
static int osal_unsleepable_lock_init(OSAL_UNSLEEPABLE_LOCK *lock)
{ pthread_mutex_init(&lock->lock,NULL); lock->initialized=true; dynamic_locks++; return 0; }
static int osal_unsleepable_lock_deinit(OSAL_UNSLEEPABLE_LOCK *lock)
{ REQUIRE(lock->initialized); pthread_mutex_destroy(&lock->lock); lock->initialized=false; dynamic_locks--; return 0; }
static int osal_lock_unsleepable_lock(OSAL_UNSLEEPABLE_LOCK *lock)
{
    if (!reader_paused && paused_record && lock==paused_record) {
        reader_paused=true;
        while (!release_reader) sched_yield();
    }
    pthread_mutex_lock(&lock->lock);
    return 0;
}
static int osal_unlock_unsleepable_lock(OSAL_UNSLEEPABLE_LOCK *lock)
{ pthread_mutex_unlock(&lock->lock); return 0; }
static int osal_set_bit(unsigned int bit, OSAL_BIT_OP_VAR *flag)
{ __atomic_fetch_or(&flag->data,1UL<<bit,__ATOMIC_SEQ_CST); return 0; }
static int osal_clear_bit(unsigned int bit, OSAL_BIT_OP_VAR *flag)
{ __atomic_fetch_and(&flag->data,~(1UL<<bit),__ATOMIC_SEQ_CST); return 0; }
static int osal_test_bit(unsigned int bit, OSAL_BIT_OP_VAR *flag)
{ return (__atomic_load_n(&flag->data,__ATOMIC_SEQ_CST)>>bit)&1; }
static int osal_event_init(OSAL_EVENT *event) { event->initialized=true; event_count++; return 0; }
static int osal_event_deinit(OSAL_EVENT *event)
{ REQUIRE(event->initialized); event->initialized=false; event_count--; return 0; }
static int osal_signal_init(OSAL_SIGNAL *signal) { signal->initialized=true; signal_count++; return 0; }
static int osal_signal_deinit(OSAL_SIGNAL *signal)
{ REQUIRE(signal->initialized); signal->initialized=false; signal->timeoutValue=0; signal_count--; return 0; }
static void osal_op_history_init(struct osal_op_history *history, int count)
{
    history->queue=history->snapshot=NULL;
    if (fault==8) return;
    history->queue=tracked_alloc(16,4);
    if (fault==9) { kfree(history->queue); history->queue=NULL; return; }
    history->snapshot=tracked_alloc(16,4);
}
static void osal_op_history_deinit(struct osal_op_history *history)
{ kfree(history->queue); kfree(history->snapshot); history->queue=history->snapshot=NULL; }
static int _stp_psm_put_op(MTKSTP_PSM_T *psm, P_OSAL_OP_Q queue, P_OSAL_OP op)
{ REQUIRE(queue->write<queue->size); queue->queue[queue->write++]=op; return 1; }
static INT32 wmt_lib_ps_stp_cb(MTKSTP_PSM_ACTION_T action) { return 0; }
static MTK_WCN_BOOL wmt_lib_is_quick_ps_support(void) { return false; }
static INT32 wmt_lib_update_fw_patch_chip_rst(void) { return 0; }
static int _stp_psm_notify_wmt_sleep_wq(MTKSTP_PSM_T *psm) { return 0; }
static INT32 _stp_psm_proc(void *data);

static void *task_entry(void *context)
{
    struct task_struct *task=context;
    while (!task->go) sched_yield();
    if (!task->stop) task->fn(task->data);
    return NULL;
}
static struct task_struct *kthread_create(void *fn, void *data, const char *name)
{
    thread_create_calls++;
    if (fault==6 || fault==7) return ERR_PTR(fault==6 ? -ENOMEM : -EINTR);
    struct task_struct *task=tracked_alloc(sizeof(*task),5);
    task->fn=fn; task->data=data; thread_count++;
    pthread_create(&task->native,NULL,task_entry,task);
    return task;
}
static int wake_up_process(struct task_struct *task)
{
    REQUIRE(task && !IS_ERR(task));
    worker_entry_checked=false;
    task->go=true;
    while (!worker_entry_checked) sched_yield();
    return 1;
}
static int kthread_stop(struct task_struct *task)
{
    REQUIRE(task && !IS_ERR(task));
    thread_stop_entered=true; task->stop=true; task->go=true;
    pthread_join(task->native,NULL);
    thread_count--; kfree(task); return 0;
}
static struct wakeup_source *wakeup_source_register(const char *name)
{ return fault==3 ? NULL : tracked_alloc(sizeof(struct wakeup_source),2); }
static void wakeup_source_unregister(struct wakeup_source *source) { kfree(source); }
static int kfifo_alloc(struct kfifo *fifo, UINT32 size, int flags)
{
    fifo->size=size;
    if (fault==2) return -ENOMEM;
    fifo->data=tracked_alloc(size,1); return fifo->data ? 0 : -ENOMEM;
}
static void kfifo_init(struct kfifo *fifo, void *data, unsigned int size) { fifo->data=data; fifo->size=size; }
static void kfifo_free(struct kfifo *fifo) { kfree(fifo->data); fifo->data=NULL; }
static unsigned int kfifo_size(struct kfifo *fifo) { return fifo->size; }
static unsigned int kfifo_len(struct kfifo *fifo) { return fifo->in-fifo->out; }
static unsigned int kfifo_avail(struct kfifo *fifo) { return fifo->size-kfifo_len(fifo); }
static int kfifo_is_empty(struct kfifo *fifo) { return fifo->in==fifo->out; }
static int kfifo_is_full(struct kfifo *fifo) { return kfifo_len(fifo)==fifo->size; }
static unsigned int kfifo_in(struct kfifo *fifo, const void *data, unsigned int count) { return 0; }
static unsigned int kfifo_out(struct kfifo *fifo, void *data, unsigned int count) { return 0; }
static void kfifo_reset(struct kfifo *fifo) { fifo->in=fifo->out=0; }
static pthread_mutex_t timer_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_t timer_thread;
static bool timer_thread_valid;
static int osal_timer_create(OSAL_TIMER *timer)
{ timer->initialized=true; timer->pending=false; timer->running=false; return 0; }
static int osal_timer_modify(OSAL_TIMER *timer, unsigned int ms)
{
    REQUIRE(timer->initialized);
    if ((scenario==16 || scenario==102) && !modify_paused) {
        modify_paused=true;
        while (!release_modify) sched_yield();
    }
    timer->pending=true;
    return 0;
}
static int osal_timer_stop_sync(OSAL_TIMER *timer)
{
    REQUIRE(timer->initialized);
    timer_stop_entered=true;
    pthread_mutex_lock(&timer_lock);
    bool join=timer_thread_valid;
    pthread_t task=timer_thread;
    timer_thread_valid=false;
    pthread_mutex_unlock(&timer_lock);
    if (join) pthread_join(task,NULL);
    timer->pending=false;
    return 0;
}

/* OSAL */
/* PSM */

static void record_flag(void)
{
#if HAS_RECORD_RCU
    _stp_psm_dbg_dmp_in(0,1);
#else
    _stp_psm_dbg_dmp_in(g_stp_psm_dbg,0,1);
#endif
}

static void *external_reader(void *unused)
{
    int which = scenario >= 103 ? scenario - 103 : scenario - 18;
#if HAS_RECORD_RCU
    switch (which) {
    case 0: record_flag(); break;
    case 1: _stp_psm_dbg_out_printk(); break;
    case 2: _stp_psm_opid_dbg_dmp_in(0,1); break;
    case 3: _stp_psm_opid_dbg_out_printk(); break;
    }
#else
    switch (which) {
    case 0: record_flag(); break;
    case 1: _stp_psm_dbg_out_printk(g_stp_psm_dbg); break;
    case 2: _stp_psm_opid_dbg_dmp_in(g_stp_psm_opid_dbg,0,1); break;
    case 3: _stp_psm_opid_dbg_out_printk(g_stp_psm_opid_dbg); break;
    }
#endif
    return NULL;
}

static INT32 _stp_psm_proc(void *data)
{
    MTKSTP_PSM_T *psm=data;
    struct task_struct *task=psm->PSMd.pThread;
    records_ready_at_start=g_stp_psm_dbg && g_stp_psm_opid_dbg &&
                           g_stp_psm_dbg->lock.initialized && g_stp_psm_opid_dbg->lock.initialized;
    worker_entry_checked=true;
    if (scenario==14) {
        while (!request_reader && !task->stop) sched_yield();
        if (request_reader) record_flag();
    }
    while (!task->stop) sched_yield();
    return 0;
}
static void *timer_entry(void *unused)
{
    stp_psm->psm_timer.running=true;
    stp_psm->psm_timer.pending=false;
    stp_psm->psm_timer.timeoutHandler(stp_psm->psm_timer.timeroutHandlerData);
    stp_psm->psm_timer.running=false;
    return NULL;
}
static void launch_timer(void)
{
    pthread_mutex_lock(&timer_lock);
    timer_thread_valid=true;
    pthread_create(&timer_thread,NULL,timer_entry,NULL);
    pthread_mutex_unlock(&timer_lock);
}
static void *deinit_thread(void *unused)
{ stp_psm_deinit(stp_psm); deinit_done=true; return NULL; }
static void *monitor_thread(void *unused) { stp_psm_start_monitor(stp_psm); return NULL; }
static void *init_thread(void *unused) { REQUIRE(stp_psm_init()==stp_psm); return NULL; }
static void require_released(void)
{
    for (int i=0;i<6;i++) REQUIRE(allocations[i]==0);
    REQUIRE(thread_count==0 && !stp_psm->PSMd.pThread);
    REQUIRE(dynamic_locks==0 && event_count==0 && signal_count==0);
    REQUIRE(!g_stp_psm_dbg && !g_stp_psm_opid_dbg);
    REQUIRE(!stp_psm->psm_timer.pending && !stp_psm->psm_timer.running);
}
static void set_fault(int which) { fault=which; debug_alloc_calls=0; }
static void teardown_rearm_case(void)
{
    pthread_t starter, closer;
    REQUIRE(stp_psm_init()==stp_psm);
    pthread_create(&starter,NULL,monitor_thread,NULL);
    while (!modify_paused) sched_yield();
    pthread_create(&closer,NULL,deinit_thread,NULL);
    while (!monitor_contended && !timer_stop_entered) sched_yield();
    release_modify=true;
    pthread_join(starter,NULL); pthread_join(closer,NULL);
    REQUIRE(!stp_psm->psm_timer.pending);
    require_released();
}
int main(int argc, char **argv)
{
    REQUIRE(argc==2); scenario=atoi(argv[1]);
    if (scenario>=2 && scenario<=8) {
        set_fault(scenario-1);
        REQUIRE(stp_psm_init()==NULL);
        require_released(); return 0;
    }
    if (scenario==9) {
        for (int which=1;which<=7;which++) {
            set_fault(which); REQUIRE(stp_psm_init()==NULL); require_released();
            set_fault(0); REQUIRE(stp_psm_init()==stp_psm);
            REQUIRE(stp_psm_deinit(stp_psm)==0); require_released();
        }
        return 0;
    }
    if (scenario==10) {
        for (int which=8;which<=9;which++) {
            set_fault(which); REQUIRE(stp_psm_init()==stp_psm);
            REQUIRE(stp_psm->op_history.queue==NULL);
            REQUIRE(stp_psm_deinit(stp_psm)==0); require_released();
        }
        return 0;
    }
    if (scenario==16 || scenario==102) { teardown_rearm_case(); return 0; }
    if (scenario==17) {
        for (int i=0;i<40;i++) {
            REQUIRE(stp_psm_init()==stp_psm); REQUIRE(stp_psm_deinit(stp_psm)==0); require_released();
        }
        return 0;
    }
    if (scenario==100) {
        pthread_t first,second;
        pthread_create(&first,NULL,init_thread,NULL); pthread_create(&second,NULL,init_thread,NULL);
        pthread_join(first,NULL); pthread_join(second,NULL);
        REQUIRE(thread_count==1 && thread_create_calls==1);
        stp_psm_deinit(stp_psm); require_released(); return 0;
    }
    REQUIRE(stp_psm_init()==stp_psm);
    if (scenario==0) {
        REQUIRE(stp_psm->work_state==ACT && stp_psm->sleep_en==1);
        REQUIRE(stp_psm->idle_time_to_sleep==STP_PSM_IDLE_TIME_SLEEP);
        REQUIRE(stp_psm->rFreeOpQ.write==STP_OP_BUF_SIZE && stp_psm->rActiveOpQ.write==0);
        REQUIRE(stp_psm->wait_wmt_q.timeoutValue==STP_PSM_WAIT_EVENT_TIMEOUT);
        REQUIRE(thread_count==1 && stp_psm->wake_lock.init_flag==1);
        return 0;
    }
    if (scenario==11) { REQUIRE(records_ready_at_start); return 0; }
    if (scenario==12) {
        STP_PSM_RECORD_T *record=g_stp_psm_dbg;
        REQUIRE(stp_psm_deinit(NULL)==STP_PSM_OPERATION_FAIL);
        REQUIRE(g_stp_psm_dbg==record && thread_count==1);
    }
    if (scenario==13) {
        REQUIRE(stp_psm_init()==stp_psm);
        REQUIRE(thread_count==1 && thread_create_calls==1);
    }
    if (scenario==14 || scenario==15) {
        pthread_t closer;
        paused_record=&g_stp_psm_dbg->lock;
        if (scenario==15) launch_timer();
        else request_reader=true;
        while (!reader_paused) sched_yield();
        pthread_create(&closer,NULL,deinit_thread,NULL);
        if (scenario==14) while (!thread_stop_entered) sched_yield();
        else while (!timer_stop_entered) sched_yield();
        release_reader=true;
        pthread_join(closer,NULL); require_released(); return 0;
    }
    if ((scenario>=18 && scenario<=21) || (scenario>=103 && scenario<=106)) {
        pthread_t reader,closer;
        int which = scenario >= 103 ? scenario - 103 : scenario - 18;
        paused_record=which < 2 ? (void *)&g_stp_psm_dbg->lock : (void *)&g_stp_psm_opid_dbg->lock;
        pthread_create(&reader,NULL,external_reader,NULL);
        while (!reader_paused) sched_yield();
        pthread_create(&closer,NULL,deinit_thread,NULL);
        while (!synchronize_entered && !deinit_done) sched_yield();
#if HAS_RECORD_RCU
        REQUIRE(!deinit_done && allocations[3]==2);
#endif
        release_reader=true;
        pthread_join(reader,NULL); pthread_join(closer,NULL);
        require_released(); return 0;
    }
    if (scenario==101) {
        pthread_t first,second;
        pthread_create(&first,NULL,deinit_thread,NULL); pthread_create(&second,NULL,deinit_thread,NULL);
        pthread_join(first,NULL); pthread_join(second,NULL); require_released(); return 0;
    }
    REQUIRE(stp_psm_deinit(stp_psm)==0); require_released();
    return 0;
}
