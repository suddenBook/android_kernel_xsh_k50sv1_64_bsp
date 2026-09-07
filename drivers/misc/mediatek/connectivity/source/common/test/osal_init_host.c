/* Host kernel substitutes; test_osal_init.py injects production OSAL functions. */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void VOID;
typedef void *PVOID;
typedef int INT32;
typedef unsigned int UINT32;
typedef unsigned char UINT8;
typedef unsigned char *PUINT8;
typedef char INT8;
typedef int spinlock_t;
#define MAX_THREAD_NAME_LEN 16
#define MAX_WAKE_LOCK_NAME_LEN 32
#define GFP_ATOMIC 1
#define pr_err(...) ((void)0)
#define pr_info(...) ((void)0)
#define osal_assert(v) do { if (!(v)) abort(); } while (0)
#define is_power_of_2(v) ((v) && !((v) & ((v) - 1)))
#define ERR_PTR(err) ((void *)(intptr_t)(err))
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
static int live_allocations, fail_descriptor, fail_fifo_data, fail_wake, thread_error;
static int create_calls, start_calls, stop_calls;
#define REQUIRE(v) do { if (!(v)) { fprintf(stderr, "line %d: %s (live=%d create=%d start=%d stop=%d)\n", \
    __LINE__, #v, live_allocations, create_calls, start_calls, stop_calls); exit(1); } } while (0)
static void *tracked_alloc(size_t bytes)
{ void *p = calloc(1, bytes); if (p) live_allocations++; return p; }
static void kfree(void *p) { if (p) { live_allocations--; free(p); } }
static void *kzalloc(size_t bytes, int flags)
{ return fail_descriptor ? NULL : tracked_alloc(bytes); }
struct task_struct { int started; };
static struct task_struct *kthread_create(void *fn, void *data, const char *name)
{ create_calls++; return thread_error ? ERR_PTR(thread_error) : tracked_alloc(sizeof(struct task_struct)); }
static int wake_up_process(struct task_struct *task)
{ REQUIRE(task && !IS_ERR(task)); task->started = 1; start_calls++; return 1; }
static int kthread_stop(struct task_struct *task)
{ REQUIRE(task && !IS_ERR(task)); stop_calls++; kfree(task); return 0; }
struct wakeup_source { int active; };
static struct wakeup_source *wakeup_source_register(const char *name)
{ return fail_wake ? NULL : tracked_alloc(sizeof(struct wakeup_source)); }
static void wakeup_source_unregister(struct wakeup_source *source) { kfree(source); }
struct kfifo { unsigned char *data; unsigned int size, in, out; };
static int kfifo_alloc(struct kfifo *fifo, UINT32 size, int flags)
{
    fifo->size = size; /* Like the kernel API, descriptor must already be valid. */
    if (fail_fifo_data) return -ENOMEM;
    fifo->data = tracked_alloc(size);
    return fifo->data ? 0 : -ENOMEM;
}
static void kfifo_init(struct kfifo *fifo, void *data, unsigned int size)
{ fifo->data = data; fifo->size = size; }
static void kfifo_free(struct kfifo *fifo) { kfree(fifo->data); fifo->data = NULL; }
static unsigned int kfifo_size(struct kfifo *fifo) { return fifo->size; }
static unsigned int kfifo_len(struct kfifo *fifo) { return fifo->in - fifo->out; }
static unsigned int kfifo_avail(struct kfifo *fifo) { return fifo->size - kfifo_len(fifo); }
static int kfifo_is_empty(struct kfifo *fifo) { return fifo->in == fifo->out; }
static int kfifo_is_full(struct kfifo *fifo) { return kfifo_len(fifo) == fifo->size; }
static unsigned int kfifo_in(struct kfifo *fifo, const void *data, unsigned int count)
{
    if (count > kfifo_avail(fifo)) count = kfifo_avail(fifo);
    for (unsigned int i = 0; i < count; i++) fifo->data[fifo->in++ % fifo->size] = ((unsigned char *)data)[i];
    return count;
}
static unsigned int kfifo_out(struct kfifo *fifo, void *data, unsigned int count)
{
    if (count > kfifo_len(fifo)) count = kfifo_len(fifo);
    for (unsigned int i = 0; i < count; i++) ((unsigned char *)data)[i] = fifo->data[fifo->out++ % fifo->size];
    return count;
}
static void kfifo_reset(struct kfifo *fifo) { fifo->in = fifo->out = 0; }

/* PRODUCTION */

int main(int argc, char **argv)
{
    REQUIRE(argc == 2);
    int scenario = atoi(argv[1]);
    OSAL_THREAD thread = {0}; OSAL_FIFO fifo = {0}; OSAL_WAKE_LOCK wake = {0};
    unsigned char external[8] = {0}, received[8] = {0};
    switch (scenario) {
    case 0:
        REQUIRE(osal_thread_create(&thread) == 0 && thread.pThread);
        REQUIRE(osal_thread_run(&thread) == 0 && start_calls == 1);
        REQUIRE(osal_thread_destroy(&thread) == 0 && !thread.pThread);
        REQUIRE(live_allocations == 0); break;
    case 1: case 2:
        thread_error = scenario == 1 ? -ENOMEM : -EINTR;
        REQUIRE(osal_thread_create(&thread) == thread_error);
        REQUIRE(!thread.pThread && live_allocations == 0); break;
    case 3:
        fail_descriptor = 1;
        REQUIRE(osal_fifo_init(&fifo, NULL, 8) == -ENOMEM);
        REQUIRE(!fifo.pFifoBody && live_allocations == 0); break;
    case 4:
        fail_fifo_data = 1;
        REQUIRE(osal_fifo_init(&fifo, NULL, 8) == -ENOMEM);
        REQUIRE(!fifo.pFifoBody && live_allocations == 0); break;
    case 5:
        REQUIRE(osal_fifo_init(&fifo, NULL, 8) == 0);
        REQUIRE(fifo.FifoDataIn(&fifo, (PUINT8)"abcdef", 6) == 6);
        REQUIRE(fifo.FifoDataOut(&fifo, received, 6) == 6 && !memcmp(received, "abcdef", 6));
        osal_fifo_deinit(&fifo); REQUIRE(!fifo.pFifoBody && live_allocations == 0); break;
    case 6:
        REQUIRE(osal_fifo_init(&fifo, NULL, 8) == 0);
        REQUIRE(osal_fifo_init(&fifo, NULL, 16) == 0);
        REQUIRE(live_allocations == 2);
        osal_fifo_deinit(&fifo); REQUIRE(live_allocations == 0); break;
    case 7:
        REQUIRE(osal_fifo_init(&fifo, external, 3) == -EINVAL);
        REQUIRE(!fifo.pFifoBody && live_allocations == 0); break;
    case 8:
        fail_wake = 1;
        REQUIRE(osal_wake_lock_init(&wake) == -ENOMEM);
        REQUIRE(!wake.init_flag && !wake.wake_lock && live_allocations == 0); break;
    case 9:
        REQUIRE(osal_wake_lock_init(&wake) == 0 && wake.init_flag);
        REQUIRE(osal_wake_lock_init(&wake) == 0 && live_allocations == 1);
        osal_wake_lock_deinit(&wake); REQUIRE(!wake.init_flag && live_allocations == 0); break;
    case 10:
        thread_error = -ENOMEM; REQUIRE(osal_thread_create(&thread) < 0);
        thread_error = 0; REQUIRE(osal_thread_create(&thread) == 0);
        osal_thread_destroy(&thread);
        fail_fifo_data = 1; REQUIRE(osal_fifo_init(&fifo, NULL, 8) < 0);
        fail_fifo_data = 0; REQUIRE(osal_fifo_init(&fifo, NULL, 8) == 0);
        osal_fifo_deinit(&fifo);
        fail_wake = 1; REQUIRE(osal_wake_lock_init(&wake) < 0);
        fail_wake = 0; REQUIRE(osal_wake_lock_init(&wake) == 0);
        osal_wake_lock_deinit(&wake); REQUIRE(live_allocations == 0); break;
    case 11:
        REQUIRE(osal_thread_create(NULL) < 0);
        REQUIRE(osal_fifo_init(NULL, NULL, 8) < 0);
        REQUIRE(osal_wake_lock_init(NULL) < 0);
        REQUIRE(live_allocations == 0); break;
    default: return 2;
    }
    return 0;
}
