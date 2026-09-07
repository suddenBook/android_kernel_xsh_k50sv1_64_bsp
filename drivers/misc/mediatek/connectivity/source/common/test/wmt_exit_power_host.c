/* Actual library/core/control functions with tracked host platform adapters. */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef int INT32;
typedef void VOID;
typedef bool MTK_WCN_BOOL;
typedef unsigned long ULONG;
typedef struct { ULONG data[4]; } WMT_OP, *P_WMT_OP;
typedef struct { ULONG data[4]; } WMT_CTRL_DATA, *P_WMT_CTRL_DATA;
typedef struct { ULONG state; } DEV_WMT, *P_DEV_WMT;

enum { DRV_STS_POWER_OFF, DRV_STS_POWER_ON, DRV_STS_FUNC_ON };
#define WMTDRV_TYPE_WMT 0
#define WMT_STAT_PWR 0
#define WMT_CTRL_HW_PWR_OFF 1
#define FUNC_OFF 0
#define MTK_WCN_BOOL_FALSE false
#define WMT_WARN_FUNC(...) ((void)snprintf(NULL, 0, __VA_ARGS__))
#define WMT_DBG_FUNC(...) ((void)snprintf(NULL, 0, __VA_ARGS__))
#define osal_assert(condition) ((void)(condition))
#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

static DEV_WMT gDevWmt;
static struct { INT32 eDrvStatus[1]; } gMtkWmtCtx;
static bool g_pwr_off_flag = true;
static int hardware_result, stp_result;
static unsigned int hardware_calls, stp_closes;

static bool osal_test_and_clear_bit(unsigned int bit, ULONG *state)
{
    bool set = (*state & (1UL << bit)) != 0;

    *state &= ~(1UL << bit);
    return set;
}

static void osal_set_bit(unsigned int bit, ULONG *state) { *state |= 1UL << bit; }

static INT32 wmt_plat_pwr_ctrl(INT32 state)
{
    REQUIRE(state == FUNC_OFF);
    hardware_calls++;
    return hardware_result;
}

/* CTRL_FUNCTION */

static INT32 wmt_core_ctrl(INT32 control, ULONG *first, ULONG *second)
{
    WMT_CTRL_DATA data = {0};

    REQUIRE(control == WMT_CTRL_HW_PWR_OFF && *first == 0 && *second == 0);
    return wmt_ctrl_hw_pwr_off(&data);
}

static INT32 wmt_core_stp_deinit(void)
{
    stp_closes++;
    return stp_result;
}

/* CORE_FUNCTION */

#ifdef HOST_EXIT_HELPER
typedef struct { struct { int timeoutValue; } signal; struct { int opId; } op; } OSAL_OP, *P_OSAL_OP;
#define MAX_FUNC_OFF_TIME 1000
#define WMT_OPID_PWR_OFF 7
static OSAL_OP library_op;
static bool btm_alive = true, reset_pending = true, no_free_op, checked_out;
static int awake_refs, psm_result;
static INT32 stp_btm_stop_thread(void)
{
    if (btm_alive && reset_pending) {
        /* An admitted reset can turn hardware on before the BTM join returns. */
        gDevWmt.state = 1UL << WMT_STAT_PWR;
        gMtkWmtCtx.eDrvStatus[WMTDRV_TYPE_WMT] = DRV_STS_FUNC_ON;
    }
    btm_alive = false;
    return 0;
}
static int wmt_lib_get_drv_status(int type) { return gMtkWmtCtx.eDrvStatus[type]; }
static P_OSAL_OP wmt_lib_get_free_op(void)
{
    if (no_free_op) return NULL;
    REQUIRE(!checked_out);
    checked_out = true;
    return &library_op;
}
static void wmt_lib_put_op_to_free_queue(P_OSAL_OP op)
{ REQUIRE(op == &library_op && checked_out); checked_out = false; }
static bool wmt_lib_put_act_op(P_OSAL_OP op)
{
    WMT_OP core_op = {0};
    REQUIRE(!btm_alive && checked_out && op == &library_op);
    REQUIRE(op->signal.timeoutValue == MAX_FUNC_OFF_TIME && op->op.opId == WMT_OPID_PWR_OFF);
    int result = opfunc_pwr_off(&core_op);
    checked_out = false;
    return result == 0;
}
static void wmt_lib_host_awake_get(void) { ++awake_refs; }
static void wmt_lib_host_awake_put(void) { REQUIRE(awake_refs == 1); --awake_refs; }
#define DISABLE_PSM_MONITOR() (psm_result)
#define ENABLE_PSM_MONITOR() ((void)0)
/* LIBRARY_FUNCTION */
#endif

static void expect_off(void)
{
    REQUIRE(gMtkWmtCtx.eDrvStatus[WMTDRV_TYPE_WMT] == DRV_STS_POWER_OFF);
    REQUIRE(gDevWmt.state == 0);
}

static void failure_then_retry(int error, bool stp_was_open)
{
    WMT_OP operation = {0};

    gMtkWmtCtx.eDrvStatus[WMTDRV_TYPE_WMT] =
        stp_was_open ? DRV_STS_FUNC_ON : DRV_STS_POWER_ON;
    hardware_result = error;
    REQUIRE(opfunc_pwr_off(&operation) == error);
    REQUIRE(hardware_calls == 1);
    REQUIRE(gMtkWmtCtx.eDrvStatus[WMTDRV_TYPE_WMT] == DRV_STS_POWER_ON);
    REQUIRE(gDevWmt.state == (1UL << WMT_STAT_PWR));

    hardware_result = 0;
    REQUIRE(opfunc_pwr_off(&operation) == 0);
    /* Either old state bug would let the second attempt skip real hardware. */
    REQUIRE(hardware_calls == 2 && stp_closes == (unsigned int)stp_was_open);
    expect_off();
}

int main(int argc, char **argv)
{
    WMT_OP operation = {0};
    WMT_CTRL_DATA control = {0};
    int scenario;

    REQUIRE(argc == 2);
    scenario = atoi(argv[1]);
    gMtkWmtCtx.eDrvStatus[WMTDRV_TYPE_WMT] = DRV_STS_FUNC_ON;
    gDevWmt.state = 1UL << WMT_STAT_PWR;

    switch (scenario) {
    case 0:
        REQUIRE(opfunc_pwr_off(&operation) == 0);
        REQUIRE(hardware_calls == 1 && stp_closes == 1);
        expect_off();
        break;
    case 1:
        failure_then_retry(-ETIMEDOUT, true);
        break;
    case 2:
        failure_then_retry(9, true);
        break;
    case 3:
        failure_then_retry(-EIO, false);
        break;
    case 4:
        stp_result = -EIO;
        REQUIRE(opfunc_pwr_off(&operation) == 0);
        REQUIRE(hardware_calls == 1 && stp_closes == 1);
        expect_off();
        break;
    case 5:
        g_pwr_off_flag = false;
        REQUIRE(opfunc_pwr_off(&operation) == -2);
        REQUIRE(!hardware_calls && !stp_closes);
        REQUIRE(gMtkWmtCtx.eDrvStatus[WMTDRV_TYPE_WMT] == DRV_STS_FUNC_ON);
        REQUIRE(gDevWmt.state == (1UL << WMT_STAT_PWR));
        g_pwr_off_flag = true;
        REQUIRE(opfunc_pwr_off(&operation) == 0);
        REQUIRE(hardware_calls == 1 && stp_closes == 1);
        expect_off();
        break;
    case 6:
        gMtkWmtCtx.eDrvStatus[WMTDRV_TYPE_WMT] = DRV_STS_POWER_OFF;
        gDevWmt.state = 0;
        REQUIRE(opfunc_pwr_off(&operation) == -1);
        REQUIRE(!hardware_calls && !stp_closes);
        expect_off();
        break;
    case 7:
        gDevWmt.state = 0;
        REQUIRE(wmt_ctrl_hw_pwr_off(&control) == 0);
        REQUIRE(!hardware_calls);
        break;
#ifdef HOST_EXIT_HELPER
    case 8:
    case 9:
        if (scenario == 9) {
            gMtkWmtCtx.eDrvStatus[WMTDRV_TYPE_WMT] = DRV_STS_POWER_OFF;
            gDevWmt.state = 0;
        }
        REQUIRE(wmt_lib_power_off_for_exit() == 0);
        REQUIRE(!btm_alive && hardware_calls == 1);
        expect_off();
        break;
    case 10:
        reset_pending = false;
        gMtkWmtCtx.eDrvStatus[WMTDRV_TYPE_WMT] = DRV_STS_POWER_OFF;
        gDevWmt.state = 0;
        REQUIRE(wmt_lib_power_off_for_exit() == 0);
        REQUIRE(!btm_alive && hardware_calls == 0);
        break;
    case 11:
        no_free_op = true;
        REQUIRE(wmt_lib_power_off_for_exit() == -ESHUTDOWN);
        REQUIRE(!btm_alive && hardware_calls == 0);
        break;
    case 12:
        psm_result = -EIO;
        REQUIRE(wmt_lib_power_off_for_exit() == -EIO);
        REQUIRE(hardware_calls == 0);
        break;
    case 13:
        hardware_result = -ETIMEDOUT;
        REQUIRE(wmt_lib_power_off_for_exit() == -EIO);
        REQUIRE(hardware_calls == 1 && gDevWmt.state == (1UL << WMT_STAT_PWR));
        hardware_result = 0;
        REQUIRE(wmt_lib_power_off_for_exit() == 0);
        REQUIRE(hardware_calls == 2);
        expect_off();
        break;
#endif
    default:
        REQUIRE(false);
    }
#ifdef HOST_EXIT_HELPER
    REQUIRE(awake_refs == 0 && !checked_out);
#endif
    return 0;
}
