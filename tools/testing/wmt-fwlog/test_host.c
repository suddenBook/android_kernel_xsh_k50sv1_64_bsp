/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef int INT32;
typedef unsigned int UINT32;
typedef unsigned char UINT8;
typedef UINT8 *PUINT8;
typedef UINT32 *PUINT32;
typedef void VOID;
typedef bool MTK_WCN_BOOL;
#define MTK_WCN_BOOL_TRUE true
#define GFP_KERNEL 0
#define EXP_APMEM_CTRL_CHIP_FW_DBGLOG_MODE 0x40
#define min_t(type, a, b) ({ type va_ = (a); type vb_ = (b); va_ < vb_ ? va_ : vb_; })
#define osal_memset memset
#define osal_assert(condition) do { (void)(condition); } while (0)
#define WMT_INFO_FUNC host_log
#define WMT_WARN_FUNC host_log
#define WMT_ERR_FUNC host_log

struct mutex { pthread_mutex_t native; };
typedef struct { struct mutex lock; } OSAL_SLEEPABLE_LOCK, *P_OSAL_SLEEPABLE_LOCK;
typedef struct { UINT32 paged_trace_off; } CONSYS_EMI_ADDR_INFO, *P_CONSYS_EMI_ADDR_INFO;

enum sleep_action { SLEEP_NONE, SLEEP_DISABLE, SLEEP_SIGNAL, SLEEP_UNMAP, SLEEP_HOLD };
struct test_task {
    atomic_bool pending;
    atomic_bool lock_attempted;
    atomic_bool done;
    enum sleep_action sleep_action;
    INT32 result;
    INT32 par1;
};
static _Thread_local struct test_task *current;
static _Thread_local jmp_buf legacy_escape;
static _Thread_local unsigned legacy_sleeps;
static atomic_int live_allocations, lock_balance, allocation_calls, max_live_allocations;
static atomic_uint sleep_calls;
static atomic_bool fail_alloc, fail_info, fail_control, fail_trace, signal_after_map;
static pthread_mutex_t model_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t model_changed = PTHREAD_COND_INITIALIZER;
static bool sleep_entered, resume_sleep;
static bool advance_index_on_copy, disable_on_copy, signal_on_copy;
static union { UINT32 words[64]; UINT8 bytes[256]; } control_storage;
#define control_memory (control_storage.bytes)
static UINT8 trace_memory[32768];
static CONSYS_EMI_ADDR_INFO emi_info = {.paged_trace_off = 0x400};
struct copied_range { UINT32 offset; size_t size; };
static struct copied_range copied[64];
static size_t copied_count, trace_bytes, control_bytes, largest_copy;

static void host_log(const char *format, ...);
static int signal_pending(struct test_task *task);
static UINT32 readl(const void *address);
static void *kmalloc(size_t size, int flags);
static void kfree(void *pointer);
static int mutex_lock_interruptible(struct mutex *lock);
static int mutex_lock_killable(struct mutex *lock);
static void mutex_unlock(struct mutex *lock);
static unsigned msleep_interruptible(unsigned milliseconds);
static void msleep(unsigned milliseconds);
static PUINT8 wmt_plat_get_emi_virt_add(UINT32 offset);
static P_CONSYS_EMI_ADDR_INFO wmt_plat_get_emi_phy_add(void);
static void osal_memcpy_fromio(void *destination, const void *source, size_t size);

/* Generated only from complete source functions and declarations, byte-for-byte. */
#include "fixture.h"

static void set_word(unsigned offset, UINT32 value)
{
    assert(offset % sizeof(UINT32) == 0 && offset < sizeof(control_memory));
    __atomic_store_n(&control_storage.words[offset / sizeof(UINT32)], value, __ATOMIC_SEQ_CST);
}

static UINT32 readl(const void *address)
{
    return __atomic_load_n((const UINT32 *)address, __ATOMIC_SEQ_CST);
}

static void host_log(const char *format, ...)
{
    if (!strcmp(format, "%s")) {
        va_list arguments;
        va_start(arguments, format);
        const char *text = va_arg(arguments, const char *);
        assert(strnlen(text, BUF_LEN_MAX) < BUF_LEN_MAX);
        va_end(arguments);
    }
}

static int signal_pending(struct test_task *task)
{
    return atomic_load(&task->pending);
}

static void *kmalloc(size_t size, int flags)
{
    int live, previous;
    (void)flags;
    atomic_fetch_add(&allocation_calls, 1);
    if (atomic_load(&fail_alloc))
        return NULL;
    void *pointer = malloc(size);
    assert(pointer);
    live = atomic_fetch_add(&live_allocations, 1) + 1;
    previous = atomic_load(&max_live_allocations);
    while (live > previous && !atomic_compare_exchange_weak(&max_live_allocations, &previous, live))
        ;
    return pointer;
}

static void kfree(void *pointer)
{
    if (pointer) {
        assert(atomic_fetch_sub(&live_allocations, 1) > 0);
        free(pointer);
    }
}

static int mutex_lock_interruptible(struct mutex *lock)
{
    atomic_store(&current->lock_attempted, true);
    for (;;) {
        if (signal_pending(current))
            return -EINTR;
        int result = pthread_mutex_trylock(&lock->native);
        if (!result) {
            assert(atomic_fetch_add(&lock_balance, 1) == 0);
            return 0;
        }
        assert(result == EBUSY);
        usleep(1000);
    }
}

static int mutex_lock_killable(struct mutex *lock)
{
    return mutex_lock_interruptible(lock);
}

static void mutex_unlock(struct mutex *lock)
{
    assert(atomic_fetch_sub(&lock_balance, 1) == 1);
    assert(pthread_mutex_unlock(&lock->native) == 0);
}

static unsigned msleep_interruptible(unsigned milliseconds)
{
    assert(milliseconds == 100);
    assert(atomic_load(&lock_balance) == 0);
    atomic_fetch_add(&sleep_calls, 1);
    if (current->sleep_action == SLEEP_DISABLE)
        set_word(0x40, 0);
    else if (current->sleep_action == SLEEP_SIGNAL)
        atomic_store(&current->pending, true);
    else if (current->sleep_action == SLEEP_UNMAP)
        atomic_store(&fail_control, true);
    else if (current->sleep_action == SLEEP_HOLD) {
        pthread_mutex_lock(&model_lock);
        sleep_entered = true;
        pthread_cond_broadcast(&model_changed);
        while (!resume_sleep)
            pthread_cond_wait(&model_changed, &model_lock);
        pthread_mutex_unlock(&model_lock);
    } else {
        assert(!"unexpected unbounded stream in host test");
    }
    return signal_pending(current) ? 1 : 0;
}

static void msleep(unsigned milliseconds)
{
    assert(milliseconds == 100);
    /* Baseline collector has no exit condition. Bound only the host control;
     * report ELOOP instead of leaving an uncontrolled test process running.
     */
    if (++legacy_sleeps == 3)
        longjmp(legacy_escape, 1);
}

static P_CONSYS_EMI_ADDR_INFO wmt_plat_get_emi_phy_add(void)
{
    return atomic_load(&fail_info) ? NULL : &emi_info;
}

static PUINT8 wmt_plat_get_emi_virt_add(UINT32 offset)
{
    if (atomic_load(&signal_after_map))
        atomic_store(&current->pending, true);
    if (offset < sizeof(control_memory))
        return atomic_load(&fail_control) ? NULL : control_memory + offset;
    if (offset >= emi_info.paged_trace_off && offset < emi_info.paged_trace_off + sizeof(trace_memory))
        return atomic_load(&fail_trace) ? NULL : trace_memory + offset - emi_info.paged_trace_off;
    return NULL;
}

static void osal_memcpy_fromio(void *destination, const void *source, size_t size)
{
    uintptr_t src = (uintptr_t)source, dst = (uintptr_t)destination;
    uintptr_t ring = (uintptr_t)trace_memory, buffer = (uintptr_t)gEmiBuf;
    assert(atomic_load(&lock_balance) == 1);
    assert(dst >= buffer && size <= sizeof(gEmiBuf) && dst - buffer <= sizeof(gEmiBuf) - size);
    if (src >= ring && src < ring + sizeof(trace_memory)) {
        assert(size <= sizeof(trace_memory) - (src - ring));
        assert(copied_count < sizeof(copied) / sizeof(copied[0]));
        copied[copied_count++] = (struct copied_range){(UINT32)(src - ring), size};
        trace_bytes += size;
        if (size > largest_copy)
            largest_copy = size;
        memcpy(destination, source, size);
        if (advance_index_on_copy) {
            set_word(0x24, 100);
            advance_index_on_copy = false;
        }
        if (disable_on_copy)
            set_word(0x40, 0);
        if (signal_on_copy)
            atomic_store(&current->pending, true);
    } else {
        assert(src >= (uintptr_t)control_memory &&
               src + size <= (uintptr_t)control_memory + sizeof(control_memory));
        control_bytes += size;
        memcpy(destination, source, size);
    }
}

static void clear_copies(void)
{
    copied_count = trace_bytes = control_bytes = largest_copy = 0;
}

static INT32 invoke(INT32 par1, INT32 par2, INT32 par3)
{
    legacy_sleeps = 0;
    if (setjmp(legacy_escape))
        return -ELOOP;
    return wmt_dbg_fwinfor_from_emi(par1, par2, par3);
}

static bool result_is(INT32 actual, INT32 expected)
{
    if (actual != expected) {
        fprintf(stderr, "expected result %d, observed %d\n", expected, actual);
        return false;
    }
    return true;
}

#define REQUIRE(expression) do { if (!(expression)) { \
    fprintf(stderr, "failed line %d: %s\n", __LINE__, #expression); return false; \
} } while (0)

static bool healthy(void)
{
    REQUIRE(atomic_load(&lock_balance) == 0);
    REQUIRE(atomic_load(&live_allocations) == 0);
    REQUIRE(buf_emi == NULL);
    return true;
}

static bool fresh_success(void)
{
    atomic_store(&current->pending, false);
    atomic_store(&fail_alloc, false);
    atomic_store(&fail_info, false);
    atomic_store(&fail_control, false);
    atomic_store(&fail_trace, false);
    atomic_store(&signal_after_map, false);
    disable_on_copy = signal_on_copy = advance_index_on_copy = false;
    set_word(0x40, 1);
    set_word(0x24, 0);
    REQUIRE(result_is(invoke(0, 1, 0), 0));
    clear_copies();
    set_word(0x24, 5);
    REQUIRE(result_is(invoke(0, 1, 0), 0));
    REQUIRE(trace_bytes == 5 && copied_count == 1 && copied[0].offset == 0);
    return healthy();
}

static void *worker(void *argument)
{
    struct test_task *task = argument;
    current = task;
    task->result = invoke(task->par1, 1, 0);
    atomic_store(&task->done, true);
    return NULL;
}

static void wait_for_sleep(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 2;
    pthread_mutex_lock(&model_lock);
    while (!sleep_entered)
        assert(pthread_cond_timedwait(&model_changed, &model_lock, &deadline) == 0);
    pthread_mutex_unlock(&model_lock);
}

static void release_sleep(void)
{
    pthread_mutex_lock(&model_lock);
    resume_sleep = true;
    pthread_cond_broadcast(&model_changed);
    pthread_mutex_unlock(&model_lock);
}

static bool run_case(const char *name)
{
    if (!strcmp(name, "ioctl-empty")) {
        REQUIRE(result_is(invoke(0, 1, 0), 0));
        REQUIRE(trace_bytes == 0 && atomic_load(&sleep_calls) == 0);
    } else if (!strcmp(name, "ioctl-progress")) {
        set_word(0x24, 123);
        REQUIRE(result_is(invoke(0, 1, 0), 0));
        REQUIRE(trace_bytes == 123);
        clear_copies();
        set_word(0x24, 211);
        REQUIRE(result_is(invoke(0, 1, 0), 0));
        REQUIRE(trace_bytes == 88 && copied[0].offset == 123);
        clear_copies();
        REQUIRE(result_is(invoke(0, 1, 0), 0));
        REQUIRE(trace_bytes == 0);
    } else if (!strcmp(name, "one-index-snapshot")) {
        set_word(0x24, 10);
        advance_index_on_copy = true;
        REQUIRE(result_is(invoke(0, 1, 0), 0));
        REQUIRE(trace_bytes == 10);
        clear_copies();
        REQUIRE(result_is(invoke(0, 1, 0), 0));
        REQUIRE(trace_bytes == 90 && copied[0].offset == 10);
    } else if (!strcmp(name, "full-ring-chunks")) {
        set_word(0x24, 32767);
        REQUIRE(result_is(invoke(0, 1, 0), 0));
        REQUIRE(trace_bytes == 32767 && largest_copy <= sizeof(gEmiBuf));
        REQUIRE(copied_count == (32767 + sizeof(gEmiBuf) - 1) / sizeof(gEmiBuf));
    } else if (!strcmp(name, "wrap-reserved-byte")) {
        set_word(0x24, 32764);
        REQUIRE(result_is(invoke(0, 1, 0), 0));
        clear_copies();
        set_word(0x24, 7);
        REQUIRE(result_is(invoke(0, 1, 0), 0));
        REQUIRE(trace_bytes == 10 && copied_count == 2);
        REQUIRE(copied[0].offset == 32764 && copied[0].size == 3);
        REQUIRE(copied[1].offset == 0 && copied[1].size == 7);
    } else if (!strcmp(name, "invalid-producer-indices")) {
        const UINT32 values[] = {32768, 65536, UINT_MAX};
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            set_word(0x24, values[i]);
            REQUIRE(result_is(invoke(0, 1, 0), -ERANGE));
            REQUIRE(trace_bytes == 0 && healthy());
        }
    } else if (!strcmp(name, "allocation-failure")) {
        atomic_store(&fail_alloc, true);
        REQUIRE(result_is(invoke(0, 1, 0), -ENOMEM));
        REQUIRE(atomic_load(&lock_balance) == 0);
    } else if (!strcmp(name, "missing-info") || !strcmp(name, "missing-control") || !strcmp(name, "missing-trace")) {
        atomic_store(!strcmp(name, "missing-info") ? &fail_info :
                     !strcmp(name, "missing-control") ? &fail_control : &fail_trace, true);
        REQUIRE(result_is(invoke(0, 1, 0), -ENODEV));
    } else if (!strcmp(name, "invalid-manual-arguments")) {
        REQUIRE(result_is(invoke(0x19, -1, 0), -EINVAL));
        REQUIRE(result_is(invoke(0x19, 32768, 0), -EINVAL));
        REQUIRE(result_is(invoke(0x19, 10, -1), -EINVAL));
        REQUIRE(atomic_load(&allocation_calls) == 0);
    } else if (!strcmp(name, "manual-clamped-length")) {
        REQUIRE(result_is(invoke(0x19, 100, INT_MAX), 0));
        REQUIRE(control_bytes == 256 && trace_bytes == 4096 && copied[0].offset == 100);
    } else if (!strcmp(name, "manual-wrap")) {
        REQUIRE(result_is(invoke(0x19, 32764, 10), 0));
        REQUIRE(control_bytes == 256 && trace_bytes == 10 && copied_count == 2);
        REQUIRE(copied[0].offset == 32764 && copied[0].size == 3);
        REQUIRE(copied[1].offset == 0 && copied[1].size == 7);
    } else if (!strcmp(name, "manual-end-offset")) {
        REQUIRE(result_is(invoke(0x19, 32767, 5), 0));
        REQUIRE(trace_bytes == 5 && copied[0].offset == 0);
    } else if (!strcmp(name, "disabled-at-entry")) {
        set_word(0x40, 0);
        set_word(0x24, 100);
        REQUIRE(result_is(invoke(0x19, 1, 0), 0));
        REQUIRE(trace_bytes == 0 && atomic_load(&sleep_calls) == 0);
    } else if (!strcmp(name, "pending-signal") || !strcmp(name, "pending-fatal-signal")) {
        atomic_store(&current->pending, true);
        REQUIRE(result_is(invoke(0, 1, 0), -EINTR));
        REQUIRE(atomic_load(&allocation_calls) == 0);
    } else if (!strcmp(name, "signal-during-mapping")) {
        atomic_store(&signal_after_map, true);
        REQUIRE(result_is(invoke(0, 1, 0), -EINTR));
        REQUIRE(trace_bytes == 0);
    } else if (!strcmp(name, "disable-between-chunks") || !strcmp(name, "signal-between-chunks")) {
        bool signal = !strcmp(name, "signal-between-chunks");
        disable_on_copy = !signal;
        signal_on_copy = signal;
        set_word(0x24, 32767);
        INT32 expected = signal && sizeof(gEmiBuf) < 32767 ? -EINTR : 0;
        REQUIRE(result_is(invoke(0, 1, 0), expected));
        REQUIRE(trace_bytes == (sizeof(gEmiBuf) < 32767 ? sizeof(gEmiBuf) : 32767));
    } else if (!strcmp(name, "signal-during-sleep") || !strcmp(name, "disable-during-sleep") || !strcmp(name, "unmap-between-passes")) {
        bool signal = !strcmp(name, "signal-during-sleep");
        bool unmap = !strcmp(name, "unmap-between-passes");
        current->sleep_action = signal ? SLEEP_SIGNAL : unmap ? SLEEP_UNMAP : SLEEP_DISABLE;
        set_word(0x24, 7);
        REQUIRE(result_is(invoke(0x19, 1, 0), signal ? -EINTR : unmap ? -ENODEV : 0));
        REQUIRE(atomic_load(&sleep_calls) == 1 && trace_bytes == 7);
    } else if (!strcmp(name, "interrupt-waiting-lock")) {
        struct test_task waiting = {0};
        pthread_t thread;
        REQUIRE(mutex_lock_interruptible(&g_dbg_emi_lock.lock) == 0);
        REQUIRE(pthread_create(&thread, NULL, worker, &waiting) == 0);
        for (unsigned i = 0; !atomic_load(&waiting.lock_attempted) && i < 2000; i++)
            usleep(1000);
        REQUIRE(atomic_load(&waiting.lock_attempted));
        atomic_store(&waiting.pending, true);
        REQUIRE(pthread_join(thread, NULL) == 0);
        REQUIRE(waiting.result == -EINTR && atomic_load(&live_allocations) == 0);
        mutex_unlock(&g_dbg_emi_lock.lock);
    } else if (!strcmp(name, "ioctl-during-proc-stream") || !strcmp(name, "signal-held-stream")) {
        struct test_task stream = {.sleep_action = SLEEP_HOLD, .par1 = 0x19};
        pthread_t thread;
        bool signal = !strcmp(name, "signal-held-stream");
        set_word(0x24, 8);
        REQUIRE(pthread_create(&thread, NULL, worker, &stream) == 0);
        wait_for_sleep();
        REQUIRE(atomic_load(&lock_balance) == 0 && !atomic_load(&stream.done));
        if (signal) {
            atomic_store(&stream.pending, true);
        } else {
            clear_copies();
            set_word(0x24, 11);
            REQUIRE(result_is(invoke(0, 1, 0), 0));
            REQUIRE(trace_bytes == 3 && copied[0].offset == 8);
            REQUIRE(atomic_load(&max_live_allocations) == 2);
            set_word(0x40, 0);
        }
        release_sleep();
        REQUIRE(pthread_join(thread, NULL) == 0);
        REQUIRE(stream.result == (signal ? -EINTR : 0));
    } else {
        fprintf(stderr, "unknown case %s\n", name);
        return false;
    }
    REQUIRE(healthy());
    return fresh_success();
}

int main(int argc, char **argv)
{
    struct test_task main_task = {0};
    assert(argc == 2);
    current = &main_task;
    assert(pthread_mutex_init(&g_dbg_emi_lock.lock.native, NULL) == 0);
    for (size_t i = 0; i < sizeof(trace_memory); i++)
        trace_memory[i] = i % 79 == 0 ? '\n' : 'a' + i % 26;
    set_word(0x40, 1);
    bool passed = run_case(argv[1]);
    printf("{\"case\":\"%s\",\"buffer_size\":%zu,\"passed\":%s,"
           "\"live_allocations\":%d,\"held_locks\":%d}\n", argv[1], sizeof(gEmiBuf),
           passed ? "true" : "false", atomic_load(&live_allocations), atomic_load(&lock_balance));
    return passed ? 0 : 1;
}
