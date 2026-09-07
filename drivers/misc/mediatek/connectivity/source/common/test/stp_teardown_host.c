/* Host resource adapters for complete production STP teardown functions. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef int INT32;
typedef void VOID;
#define _osal_inline_ inline
#define STP_DBG_PR_ERR(...) ((void)0)
#define STP_BTM_PR_INFO(...) ((void)0)
#define STP_BTM_PR_ERR(...) ((void)0)
#define STP_BTM_OPERATION_SUCCESS 0
#define STP_BTM_OPERATION_FAIL -1
#define STP_MAGIC_NUM 0xaabbccddUL
#define MTKSTP_MAX_TASK_NUM 3
#define mutex_lock(p) assert(pthread_mutex_lock(p) == 0)
#define mutex_unlock(p) assert(pthread_mutex_unlock(p) == 0)

struct timer { bool pending, running; unsigned callbacks; };
struct work_struct { bool pending, running; };
typedef struct { bool alive, callback; } OSAL_THREAD;
typedef struct { bool owned; } resource;
typedef struct { struct timer dmp_timer, dmp_emi_timer; resource dmp_lock; void *p_head; } WCN_CORE_DUMP_T;
typedef WCN_CORE_DUMP_T *P_WCN_CORE_DUMP_T;
typedef struct { int unused; } MTKSTP_LOG_ENTRY_T;
typedef struct { struct work_struct dump_work; MTKSTP_LOG_ENTRY_T *dump_queue; } MTKSTP_LOG_SYS_T;
typedef struct { MTKSTP_LOG_SYS_T *logsys; } MTKSTP_DBG_T;
typedef struct {
    OSAL_THREAD BTMd;
    struct timer trigger_assert_timer;
    resource op_history;
    void *wmt_notify, *pCurOP;
} MTKSTP_BTM_T;
static MTKSTP_BTM_T btm_storage, *stp_btm = &btm_storage;
static bool g_btm_initialized, g_stp_resources_initialized;
static bool psm_alive, expect_btm_callback, expect_debug_callback;
static unsigned btm_callbacks, timer_callbacks, work_callbacks;
static resource psm_resource;
static WCN_CORE_DUMP_T *g_core_dump, *core_allocation;
static MTKSTP_DBG_T *g_stp_dbg, *g_mtkstp_dbg;
static MTKSTP_LOG_SYS_T *log_allocation;
static void *g_stp_dbg_cpupcr, *g_stp_dbg_dmaregs;
static struct {
    void *psm;
    MTKSTP_BTM_T *btm;
    struct timer tx_timer;
    struct { resource mtx; } ring[MTKSTP_MAX_TASK_NUM];
    bool ready, enable;
    resource lock;
} stp_core_ctx;
static pthread_mutex_t g_stp_lifecycle_lock = PTHREAD_MUTEX_INITIALIZER;
static void *sys_if_tx, *sys_rx_has_pending_data, *sys_tx_has_pending_data;
static void *sys_rx_thread_get, *sys_event_set, *sys_event_tx_resume, *sys_check_function_status;
#define STP_BTM_CORE(c) ((c).btm)
#define STP_PSM_CORE(c) ((c).psm)
#define STP_SET_BTM_CORE(c, v) ((c).btm = (v))
#define STP_SET_PSM_CORE(c, v) ((c).psm = (v))
#define STP_SET_READY(c, v) ((c).ready = (v))
#define STP_SET_ENABLE(c, v) ((c).enable = (v))

static void *allocate(size_t bytes) { void *p = calloc(1, bytes); assert(p); return p; }
static bool timer_live(const struct timer *t) { return t->pending || t->running; }
static void osal_timer_stop(struct timer *t) { t->pending = false; }
static void osal_timer_stop_sync(struct timer *t)
{
    if (t->running) {
        /* A callback admitted before deletion retains its object dependencies. */
        if (core_allocation && (t == &core_allocation->dmp_timer || t == &core_allocation->dmp_emi_timer))
            assert(g_stp_dbg && core_allocation->dmp_lock.owned);
        if (t == &stp_core_ctx.tx_timer)
            assert(stp_core_ctx.lock.owned);
        ++t->callbacks;
        ++timer_callbacks;
    }
    t->running = t->pending = false;
}
static INT32 osal_thread_destroy(OSAL_THREAD *thread)
{
    if (thread->alive && thread->callback) {
        assert(psm_alive && sys_if_tx && stp_core_ctx.lock.owned);
        if (expect_debug_callback)
            assert(g_stp_dbg && g_core_dump);
        ++btm_callbacks;
    }
    thread->alive = false;
    return 0;
}
static void osal_op_history_deinit(resource *r)
{
    assert(!stp_btm->BTMd.alive && !timer_live(&stp_btm->trigger_assert_timer));
    assert(r->owned);
    r->owned = false;
}
static void osal_sleepable_lock_deinit(resource *r)
{
    if (core_allocation && r == &core_allocation->dmp_lock) {
        assert(!timer_live(&core_allocation->dmp_timer));
        assert(!timer_live(&core_allocation->dmp_emi_timer));
    }
    assert(r->owned);
    r->owned = false;
}
static void osal_free(void *p)
{
    if (core_allocation && (p == core_allocation || p == core_allocation->p_head)) {
        assert(!timer_live(&core_allocation->dmp_timer));
        assert(!timer_live(&core_allocation->dmp_emi_timer));
    }
    if (p == core_allocation)
        core_allocation = NULL;
    free(p);
}
static void vfree(void *p)
{
    if (p == log_allocation) {
        assert(!log_allocation->dump_work.pending && !log_allocation->dump_work.running);
        log_allocation = NULL;
    }
    free(p);
}
#define kfree free
static void cancel_work_sync(struct work_struct *work)
{
    assert(log_allocation && work == &log_allocation->dump_work);
    if (work->running) {
        free(log_allocation->dump_queue);
        log_allocation->dump_queue = NULL;
        ++work_callbacks;
    }
    work->pending = work->running = false;
}
static void stp_dbg_cpupcr_deinit(void *p) { free(p); }
static void stp_dbg_dmaregs_deinit(void *p) { free(p); }
static void stp_dbg_nl_deinit(void) { /* Separate actual-function netlink fixture. */ }
static void stp_psm_deinit(void *p)
{
    assert(p && psm_alive && !stp_btm->BTMd.alive);
    psm_alive = false;
}
static void osal_unsleepable_lock_deinit(resource *r)
{
    assert(r->owned && !stp_btm->BTMd.alive && !timer_live(&stp_core_ctx.tx_timer));
    r->owned = false;
}
static void stp_ctx_lock_deinit(void *p)
{
    assert(p == &stp_core_ctx && stp_core_ctx.lock.owned);
    stp_core_ctx.lock.owned = false;
}

/* PROTOTYPES */
/* PRODUCTION */

static void setup_debug(bool with_log)
{
    g_stp_dbg = g_mtkstp_dbg = allocate(sizeof(*g_stp_dbg));
    if (with_log) {
        log_allocation = g_stp_dbg->logsys = allocate(sizeof(*log_allocation));
        log_allocation->dump_work.pending = true;
        log_allocation->dump_queue = allocate(sizeof(*log_allocation->dump_queue));
    }
    g_core_dump = core_allocation = allocate(sizeof(*g_core_dump));
    g_core_dump->p_head = allocate(8);
    g_core_dump->dmp_lock.owned = true;
    g_core_dump->dmp_timer.pending = g_core_dump->dmp_emi_timer.pending = true;
    g_stp_dbg_cpupcr = allocate(8);
    g_stp_dbg_dmaregs = allocate(8);
}
static void setup_stp(bool with_btm, bool with_debug)
{
    g_stp_resources_initialized = true;
    stp_core_ctx.lock.owned = true;
    stp_core_ctx.ready = stp_core_ctx.enable = true;
    stp_core_ctx.tx_timer.pending = true;
    for (int i = 0; i < MTKSTP_MAX_TASK_NUM; ++i)
        stp_core_ctx.ring[i].mtx.owned = true;
    sys_if_tx = sys_rx_has_pending_data = sys_tx_has_pending_data = &stp_core_ctx;
    sys_rx_thread_get = sys_event_set = sys_event_tx_resume = sys_check_function_status = &stp_core_ctx;
    psm_alive = true;
    stp_core_ctx.psm = &psm_resource;
    if (with_btm) {
        stp_core_ctx.btm = stp_btm;
        g_btm_initialized = true;
        stp_btm->BTMd.alive = true;
        stp_btm->BTMd.callback = expect_btm_callback = true;
        expect_debug_callback = with_debug;
        stp_btm->op_history.owned = true;
        stp_btm->trigger_assert_timer.pending = true;
        stp_btm->wmt_notify = stp_btm->pCurOP = stp_btm;
    }
    if (with_debug)
        setup_debug(true);
}
static void assert_debug_released(void)
{
    assert(!core_allocation && !log_allocation);
    assert(!g_core_dump && !g_stp_dbg && !g_stp_dbg_cpupcr && !g_stp_dbg_dmaregs);
}
static void assert_stp_released(void)
{
    assert_debug_released();
    assert(!g_mtkstp_dbg && !g_stp_resources_initialized && !g_btm_initialized);
    assert(!psm_alive && !stp_core_ctx.psm && !stp_core_ctx.btm);
    assert(!stp_core_ctx.lock.owned && !stp_core_ctx.ready && !stp_core_ctx.enable);
    assert(!timer_live(&stp_core_ctx.tx_timer));
    assert(!sys_if_tx && !sys_rx_has_pending_data && !sys_tx_has_pending_data);
    assert(!sys_rx_thread_get && !sys_event_set && !sys_event_tx_resume && !sys_check_function_status);
    if (expect_btm_callback)
        assert(btm_callbacks == 1);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    int scenario = atoi(argv[1]);
    if (scenario < 4) {
        setup_debug(scenario != 3);
        if (scenario == 1)
            g_core_dump->dmp_timer.running = g_core_dump->dmp_emi_timer.running = true;
        if (scenario == 2)
            log_allocation->dump_work.running = true;
        assert(stp_dbg_deinit(g_stp_dbg) == 0);
        assert_debug_released();
        if (scenario == 1) assert(timer_callbacks == 2);
        if (scenario == 2) assert(work_callbacks == 1);
    } else if (scenario == 9) {
        assert(mtk_wcn_stp_deinit() == 0);
        assert_stp_released();
    } else {
        setup_stp(scenario != 6, scenario != 6 && scenario != 7);
        if (scenario == 4) {
            assert(stp_btm_deinit(stp_btm) == 0);
            assert(psm_alive && g_stp_dbg && btm_callbacks == 1);
        }
        if (scenario == 8)
            stp_core_ctx.tx_timer.running = true;
        assert(mtk_wcn_stp_deinit() == 0);
        assert_stp_released();
        assert(mtk_wcn_stp_deinit() == 0);
        assert_stp_released();
        if (scenario == 8) assert(timer_callbacks == 1);
    }
    puts("PASS");
    return 0;
}
