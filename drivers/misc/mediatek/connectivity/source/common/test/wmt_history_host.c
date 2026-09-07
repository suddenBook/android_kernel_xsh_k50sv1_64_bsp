/* Host substitutes; test_wmt_history.py injects unmodified production functions. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void VOID;
typedef void *PVOID;
typedef int INT32;
typedef unsigned int UINT32;
typedef unsigned char UINT8;
typedef unsigned long long UINT64;
typedef unsigned long ULONG;
typedef size_t SIZE_T;
typedef char *PINT8;
typedef atomic_int atomic_t;
#define atomic_read(p) atomic_load(p)
#define MAX_HISTORY_NAME_LEN 32
#define READ_ONCE(v) __atomic_load_n(&(v), __ATOMIC_ACQUIRE)
#define WRITE_ONCE(v, x) __atomic_store_n(&(v), (x), __ATOMIC_RELEASE)
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define GFP_KERNEL 1
#define GFP_ATOMIC 2
#define osal_snprintf snprintf
#define osal_memcpy memcpy
#define WARN_ON(v) ((v) ? fprintf(stderr, "WARN_ON: %s\n", #v) : 0)
typedef pthread_mutex_t spinlock_t;
static _Thread_local int held_locks;
static void spin_lock_init(spinlock_t *lock) { pthread_mutex_init(lock, NULL); }
static void lock_irq(spinlock_t *lock) { pthread_mutex_lock(lock); held_locks++; }
static void unlock_irq(spinlock_t *lock) { held_locks--; pthread_mutex_unlock(lock); }
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; lock_irq(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) unlock_irq(lock)
static atomic_int alloc_live, alloc_calls, alloc_locked, fail_alloc;
static void *allocate(size_t count, size_t size, int flags, bool clear)
{
    int call = atomic_fetch_add(&alloc_calls, 1) + 1;
    if (flags == GFP_KERNEL && held_locks) alloc_locked++;
    if (fail_alloc == call) return NULL;
    void *p = clear ? calloc(count, size) : malloc(count * size);
    if (p) alloc_live++;
    return p;
}
#define kzalloc(size, flags) allocate(1, size, flags, true)
#define kmalloc(size, flags) allocate(1, size, flags, false)
#define kcalloc(count, size, flags) allocate(count, size, flags, true)
static void kfree(void *p) { if (p) { alloc_live--; free(p); } }
struct work_struct { void (*fn)(struct work_struct *); bool pending, running; };
static pthread_mutex_t work_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER;
static atomic_bool worker_stop, pause_worker, work_entered, release_work;
static struct work_struct *work_item;
static void INIT_WORK(struct work_struct *work, void (*fn)(struct work_struct *))
{ work->fn = fn; work->pending = false; work->running = false; }
static bool schedule_work(struct work_struct *work)
{
    pthread_mutex_lock(&work_lock);
    bool accepted = !work->pending;
    if (accepted) { work->pending = true; work_item = work; }
    pthread_cond_broadcast(&work_cv);
    pthread_mutex_unlock(&work_lock);
    return accepted;
}
static bool cancel_work_sync(struct work_struct *work)
{
    pthread_mutex_lock(&work_lock);
    bool pending = work->pending;
    work->pending = false;
    while (work->running) pthread_cond_wait(&work_cv, &work_lock);
    pthread_mutex_unlock(&work_lock);
    return pending;
}
static bool run_work_once(void)
{
    pthread_mutex_lock(&work_lock);
    struct work_struct *work = work_item;
    if (!work || !work->pending || work->running || pause_worker) {
        pthread_mutex_unlock(&work_lock);
        return false;
    }
    work->pending = false; work->running = true;
    pthread_mutex_unlock(&work_lock);
    work->fn(work);
    pthread_mutex_lock(&work_lock);
    work->running = false;
    pthread_cond_broadcast(&work_cv);
    pthread_mutex_unlock(&work_lock);
    return true;
}
static void *worker_loop(void *unused)
{
    while (!worker_stop) if (!run_work_once()) sched_yield();
    while (run_work_once()) {}
    return NULL;
}
static atomic_int captured_count;
static UINT32 captured_ids[128];
static int scenario;
static void host_pr_info(const char *fmt, ...)
{
    if (!strncmp(fmt, "(%llu", 5)) {
        if (scenario == 3) {
            work_entered = true;
            while (!release_work) sched_yield();
        }
        va_list args;
        va_start(args, fmt);
        (void)va_arg(args, UINT64); (void)va_arg(args, ULONG);
        (void)va_arg(args, char *); (void)va_arg(args, void *);
        UINT32 id = va_arg(args, UINT32);
        int at = atomic_fetch_add(&captured_count, 1);
        if (scenario < 100 && at < 128) captured_ids[at] = id;
        va_end(args);
    }
}
#define pr_info host_pr_info
static void osal_get_local_time(UINT64 *sec, ULONG *usec) { *sec = 1; *usec = 2; }
typedef struct {
    struct { UINT32 opId, u4InfoBit; SIZE_T au4OpData[8]; } op;
    atomic_t ref_count;
} OSAL_OP, *P_OSAL_OP;

/* PRODUCTION */

#define REQUIRE(v) do { if (!(v)) { fprintf(stderr, "line %d: %s (alloc=%d locked=%d captured=%d)\n", \
    __LINE__, #v, alloc_live, alloc_locked, captured_count); exit(1); } } while (0)
static struct osal_op_history history;
static void save_id(UINT32 id)
{
    OSAL_OP op = {0}; op.op.opId = id; op.op.au4OpData[0] = id * 2;
    osal_op_history_save(&history, &op);
}
static void *deinit_thread(void *unused) { osal_op_history_deinit(&history); return NULL; }
static void *save_thread(void *unused)
{
    for (int i = 0; i < 20000; i++) save_id(i);
    return NULL;
}
static void *print_thread(void *unused)
{
    for (int i = 0; i < 20000; i++) osal_op_history_print(&history, "history");
    return NULL;
}
static void join_worker(pthread_t worker)
{ worker_stop = true; pthread_join(worker, NULL); }
int main(int argc, char **argv)
{
    REQUIRE(argc == 2); scenario = atoi(argv[1]);
    if (scenario == 4) {
        fail_alloc = 1;
        osal_op_history_init(&history, 16);
        save_id(1); osal_op_history_print(&history, "failure");
        osal_op_history_deinit(&history);
        REQUIRE(alloc_live == 0);
        fail_alloc = alloc_calls + 2;
        osal_op_history_init(&history, 16);
        osal_op_history_deinit(&history);
        REQUIRE(alloc_live == 0);
        return 0;
    }
    if (scenario == 5) {
        osal_op_history_init(&history, 3);
        REQUIRE(history.queue == NULL && alloc_live == 0);
        osal_op_history_deinit(&history);
        osal_op_history_init(&history, 0);
        REQUIRE(history.queue == NULL && alloc_live == 0);
        return 0;
    }
    if (scenario == 7) {
        for (int i = 0; i < 100; i++) {
            osal_op_history_init(&history, 16); save_id(i);
            osal_op_history_print(&history, "reinit");
            osal_op_history_deinit(&history);
            REQUIRE(!run_work_once() && alloc_live == 0);
        }
        return 0;
    }
    osal_op_history_init(&history, 16);
    if (scenario != 6) for (int i = 0; i < 20; i++) save_id(i);
    if (scenario == 0) {
        osal_op_history_print(&history, "first");
        for (int i = 20; i < 40; i++) save_id(i);
        osal_op_history_print(&history, "ignored-while-busy");
        REQUIRE(run_work_once());
        REQUIRE(captured_count == 16);
        for (int i = 0; i < 16; i++) REQUIRE(captured_ids[i] == (UINT32)(i + 4));
        osal_op_history_print(&history, "second"); REQUIRE(run_work_once());
        REQUIRE(captured_count == 32);
        for (int i = 0; i < 16; i++) REQUIRE(captured_ids[i + 16] == (UINT32)(i + 24));
        return 0;
    }
    if (scenario == 1) {
        osal_op_history_print(&history, "allocation");
        REQUIRE(alloc_locked == 0);
        return 0;
    }
    if (scenario == 2) {
        osal_op_history_print(&history, "teardown");
        osal_op_history_deinit(&history);
        REQUIRE(!run_work_once() && alloc_live == 0);
        save_id(99); osal_op_history_print(&history, "closed");
        REQUIRE(!run_work_once());
        return 0;
    }
    if (scenario == 3) {
        pthread_t worker, closer;
        osal_op_history_print(&history, "running");
        pthread_create(&worker, NULL, worker_loop, NULL);
        while (!work_entered) sched_yield();
        pthread_create(&closer, NULL, deinit_thread, NULL);
        usleep(10000); release_work = true;
        pthread_join(closer, NULL); join_worker(worker);
        REQUIRE(alloc_live == 0);
        return 0;
    }
    if (scenario == 6) {
        osal_op_history_print(&history, "empty");
        REQUIRE(!run_work_once() && captured_count == 0);
        osal_op_history_deinit(&history);
        REQUIRE(alloc_live == 0);
        return 0;
    }
    if (scenario >= 100) {
        pthread_t worker, writer, reader, closer;
        pthread_create(&worker, NULL, worker_loop, NULL);
        pthread_create(&writer, NULL, save_thread, NULL);
        pthread_create(&reader, NULL, print_thread, NULL);
        if (scenario == 101) {
            usleep(1000); pthread_create(&closer, NULL, deinit_thread, NULL);
        }
        pthread_join(writer, NULL); pthread_join(reader, NULL);
        if (scenario == 101) pthread_join(closer, NULL);
        join_worker(worker);
        if (scenario == 101) REQUIRE(alloc_live == 0);
        return 0;
    }
    return 2;
}
