/* Host-only scheduling, allocator, transport and user-copy substitutes. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void VOID;
typedef void *PVOID;
typedef int32_t INT32;
typedef int32_t *PINT32;
typedef uint32_t UINT32;
typedef uint32_t *PUINT32;
typedef uint16_t UINT16;
typedef uint8_t UINT8;
typedef uint8_t *PUINT8;
typedef unsigned long ULONG;
typedef size_t SIZE_T;
typedef bool MTK_WCN_BOOL;
typedef uint8_t u8;
typedef uint16_t u16;
typedef int atomic_t;
typedef struct { UINT32 timeoutValue; int done; } OSAL_SIGNAL, *P_OSAL_SIGNAL;
typedef struct { int unused; } OSAL_EVENT;
typedef struct { int unused; } OSAL_OP_Q, *P_OSAL_OP_Q;
#define __packed __attribute__((packed))
#define MTK_WCN_BOOL_FALSE false
#define MTK_WCN_BOOL_TRUE true
#define CONFIG_MTK_COMBO_ANT 1
#define CFG_WMT_LTE_COEX_HANDLING 1
#define WMTDRV_TYPE_WIFI 3
#define WMTDRV_TYPE_WMT 4
#define WMT_IOCTL_LPBK_TEST 1
#define WMT_IOCTL_ADIE_LPBK_TEST 2
#define WMT_IOCTL_SEND_BGW_DS_CMD 3
#define MAX_EACH_WMT_CMD 2000
#define MAX_FUNC_ON_TIME 20000
#define WMT_ERR_FUNC(...) ((void)0)
#define WMT_WARN_FUNC(...) ((void)0)
#define WMT_DBG_FUNC(...) ((void)0)
#define WMT_INFO_FUNC(...) ((void)0)
#define osal_assert assert
#define osal_sizeof sizeof
#define osal_memcpy memcpy
#define osal_memset memset
#define osal_memcmp memcmp
#define min(a, b) ((a) < (b) ? (a) : (b))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define atomic_set(p, value) (*(p) = (value))
#define atomic_inc(p) (++*(p))
#define atomic_dec(p) (--*(p))
#define atomic_read(p) (*(p))
#define atomic_dec_and_test(p) (--*(p) == 0)
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, data) ((value) = (data))
static pthread_mutex_t g_wmt_op_lock = PTHREAD_MUTEX_INITIALIZER;
#define DEFINE_MUTEX(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; pthread_mutex_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); pthread_mutex_unlock(lock); } while (0)
#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "line %d: %s\n", __LINE__, #value); return 1; } } while (0)

/* SOURCE_TYPES */

typedef struct {
    OSAL_OP_Q rFreeOpQ, rActiveOpQ, rWorkerOpQ;
    OSAL_EVENT rWmtdWq, rWmtdWorkerWq;
    int thread, worker_thread;
    P_OSAL_OP pCurOP;
    UINT8 msg_local_buffer[WMT_IDC_MSG_BUFFER];
} DEV_WMT, *P_DEV_WMT;
/* SOURCE_PROTOTYPES */
static DEV_WMT gDevWmt;
static UINT8 gLpbkBuf[WMT_LPBK_BUF_LEN] __attribute__((unused));
static UINT32 gLpbkBufLog __attribute__((unused));
static COEX_BUF gCoexBuf;
/* SOURCE_LOCKS */
#if HAVE_OP_POOL
static DEFINE_MUTEX(g_wmt_op_pool_lock);
static bool g_wmt_op_pool_stopping;
static atomic_t g_wmt_ops_checked_out;
static int g_wmt_op_pool_idle;
#define wake_up(q) ((void)(q))
#endif

enum wait_schedule { COMPLETE, TIMEOUT, CANCEL, LATE_COMPLETE, CANCEL_THEN_COMPLETE };
static enum wait_schedule wait_schedule;
static int psm_failure, queue_failure, coredump, no_free_op, stp_ready = 1;
static int fail_copy_from_at, fail_copy_to_at, copy_from_calls, copy_to_calls;
static int idc_lock_failure, idc_locked;
static int wire_tx_error, wire_tx_short, wire_rx_error, wire_bad_opcode;
static int wire_short_header, wire_short_payload, wire_announced_overrun, wire_reported_overrun, wire_empty;
static int bad_loopback_length, nested_cmd, nested_result;
static int nested_coex;
static UINT8 nested_user[4096];
static UINT8 tx_wire[4096];
static size_t tx_wire_len;
static int tx_calls, rx_calls;

struct allocation { void *pointer; size_t size; bool live; };
static struct allocation allocations[32];
static int alloc_attempts, allocations_used, allocations_live, frees, fail_alloc_at;
static void *osal_malloc(size_t size)
{
    if (++alloc_attempts == fail_alloc_at) return NULL;
    assert(size > 0 && size <= 65536);
    assert(allocations_used < 32);
    void *pointer = malloc(size);
    assert(pointer);
    memset(pointer, 0xa5, size);
    allocations[allocations_used++] = (struct allocation){pointer, size, true};
    allocations_live++;
    return pointer;
}
static void osal_free(void *pointer)
{
    if (!pointer) return;
    for (int i = 0; i < allocations_used; i++) {
        if (allocations[i].pointer == pointer && allocations[i].live) {
            allocations[i].live = false;
            allocations_live--;
            frees++;
            free(pointer);
            return;
        }
    }
    assert(!"free of unowned or already-freed allocation");
}

static OSAL_OP pool[4];
static bool slot_used[4];
static int acquired, recycled;
struct request {
    P_OSAL_OP operation;
    bool pending;
    UINT8 observed[2048];
    size_t observed_len;
};
static struct request requests[8];
static int submitted;
static P_OSAL_OP wmt_lib_get_op(P_OSAL_OP_Q queue)
{
    assert(queue == &gDevWmt.rFreeOpQ);
    if (no_free_op) return NULL;
    for (size_t i = 0; i < 4; i++) {
        if (!slot_used[i]) {
            slot_used[i] = true;
            acquired++;
            return &pool[i];
        }
    }
    return NULL;
}
static MTK_WCN_BOOL wmt_lib_put_op(P_OSAL_OP_Q queue, P_OSAL_OP operation)
{
    ptrdiff_t slot = operation - pool;
    assert(slot >= 0 && slot < 4 && slot_used[slot]);
    if (queue == &gDevWmt.rActiveOpQ) {
        if (queue_failure) return false;
        assert(submitted < 8);
        requests[submitted++] = (struct request){.operation = operation, .pending = true};
        return true;
    }
    assert(queue == &gDevWmt.rFreeOpQ);
    assert(operation->ref_count == 0);
    slot_used[slot] = false;
    recycled++;
    /* Immediate reuse makes stale operation-field reads deterministic. */
    memset(operation, 0xcc, sizeof(*operation));
    return true;
}
static int mtk_wcn_stp_coredump_start_get(void) { return coredump; }
static int mtk_wcn_stp_is_ready(void) { return stp_ready; }
static void osal_signal_init(P_OSAL_SIGNAL signal) { signal->done = 0; }
static void osal_trigger_event(OSAL_EVENT *event) { (void)event; }
static int osal_op_is_wait_for_signal(P_OSAL_OP operation) { return operation->signal.timeoutValue != 0; }
static void osal_op_raise_signal(P_OSAL_OP operation, int result)
{
    assert(slot_used[operation - pool]);
    operation->result = result;
    operation->signal.done = 1;
}
static int copy_from_user(void *dest, const void *source, size_t size)
{
    if (++copy_from_calls == fail_copy_from_at) return size ? (int)size : 1;
    memcpy(dest, source, size);
    return 0;
}
static int copy_to_user(void *dest, const void *source, size_t size)
{
    if (++copy_to_calls == fail_copy_to_at) return size ? (int)size : 1;
    memcpy(dest, source, size);
    return 0;
}
static int wmt_lib_idc_lock_aquire(void)
{
    if (idc_lock_failure) return -1;
    assert(!idc_locked);
    idc_locked = 1;
    return 0;
}
static void wmt_lib_idc_lock_release(void) { assert(idc_locked); idc_locked = 0; }
static int disable_psm(void) { return psm_failure; }
static int wmt_lib_set_host_assert_info(UINT32 type, UINT32 reason, UINT32 trigger)
{ (void)type; (void)reason; (void)trigger; return 0; }
static void enable_psm(void);
#define DISABLE_PSM_MONITOR() disable_psm()
#define ENABLE_PSM_MONITOR() enable_psm()
static INT32 run_ioctl(UINT32 cmd, ULONG arg);

static INT32 wmt_core_tx(PUINT8 bytes, UINT32 size, PUINT32 written, MTK_WCN_BOOL raw)
{
    (void)raw;
    assert(size <= sizeof(tx_wire));
    memcpy(tx_wire, bytes, size);
    tx_wire_len = size;
    tx_calls++;
    *written = wire_tx_short ? size - 1 : size;
    return wire_tx_error ? -EIO : 0;
}
static INT32 wmt_core_rx(PUINT8 bytes, UINT32 capacity, PUINT32 read_size)
{
    /* A complete literal GPS firmware response: five data bytes. */
    static const UINT8 response[] = {0x02, 0x32, 0x05, 0x00, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5};
    size_t size = min(capacity, sizeof(response));
    if (wire_short_header) size = 2;
    if (wire_short_payload) size = 7;
    if (wire_empty) size = 4;
    memcpy(bytes, response, size);
    if (wire_bad_opcode && size >= 2) bytes[1] = 0x31;
    if (wire_announced_overrun) bytes[2] = 13;
    if (wire_empty) bytes[2] = 0;
    *read_size = wire_reported_overrun ? capacity + 1 : size;
    rx_calls++;
    return wire_rx_error ? -EIO : 0;
}

static void consume_request(int index)
{
    assert(index >= 0 && index < submitted && requests[index].pending);
    struct request *request = &requests[index];
    P_OSAL_OP operation = request->operation;
    P_WMT_OP data = &operation->op;
    assert(slot_used[operation - pool]);
    PUINT8 input = NULL;
    size_t input_len = 0;
    INT32 result = 0;
    switch (data->opId) {
    case WMT_OPID_GPS_MCU_CTRL:
        result = opfunc_gps_mcu_ctrl(data);
        break;
    case WMT_OPID_LPBK:
    case WMT_OPID_BGW_DS:
        input = (PUINT8)data->au4OpData[1];
        input_len = data->au4OpData[0];
        break;
    case WMT_OPID_ADIE_LPBK_TEST:
        break;
    case WMT_OPID_ANT_RAM_DOWN:
    case WMT_OPID_FLASH_PATCH_DOWN:
        input = (PUINT8)data->au4OpData[0];
        input_len = data->au4OpData[1];
        break;
    case WMT_OPID_IDC_MSG_HANDLING: {
        UINT16 wire_length;
        input = (PUINT8)data->au4OpData[0];
        memcpy(&wire_length, input, sizeof(wire_length));
        assert(wire_length > 0);
        input_len = sizeof(wire_length) + wire_length - 1;
        break;
    }
    case WMT_OPID_ANT_RAM_STA_GET:
        data->au4OpData[2] = 1;
        break;
    case WMT_OPID_FLASH_PATCH_VER_GET:
        data->au4OpData[4] = 0x10203040;
        data->au4OpData[6] = 0;
        break;
    case WMT_OPID_CMD_TEST: {
        static const UINT8 first_reply[] = {0x41, 0x42, 0x43, 0x31, 0x32, 0x33};
        static const UINT8 second_reply[] = {0x7a, 0x39, 0x38, 0x37, 0x36};
        assert(data->au4OpData[0] == 2);
        size_t length = index == 0 ? 6 : 5;
        memcpy((void *)data->au4OpData[2], index == 0 ? first_reply : second_reply, length);
        data->au4OpData[3] = length;
        break;
    }
    default:
        assert(!"unexpected operation submitted by fixture");
    }
    if (input_len) {
        assert(input_len <= sizeof(request->observed));
        memcpy(request->observed, input, input_len);
        request->observed_len = input_len;
    }
    if (data->opId == WMT_OPID_LPBK || data->opId == WMT_OPID_ADIE_LPBK_TEST) {
        static const UINT8 first_reply[] = {0x41, 0x42, 0x43, 0x31, 0x32, 0x33};
        static const UINT8 second_reply[] = {0x7a, 0x39, 0x38, 0x37, 0x36};
        size_t length = index == 0 ? sizeof(first_reply) : sizeof(second_reply);
        memcpy((void *)data->au4OpData[1], index == 0 ? first_reply : second_reply, length);
        data->au4OpData[0] = bad_loopback_length ? 2049 : length;
    }
    request->pending = false;
    wmt_lib_complete_op(operation, result);
}
static int osal_wait_for_signal_timeout(P_OSAL_SIGNAL signal, int *thread)
{
    (void)thread;
    P_OSAL_OP operation = (P_OSAL_OP)((char *)signal - offsetof(OSAL_OP, signal));
    int request = -1;
    for (int i = 0; i < submitted; i++)
        if (requests[i].operation == operation && requests[i].pending) request = i;
    assert(request >= 0);
    if (wait_schedule == TIMEOUT) return 0;
    if (wait_schedule == CANCEL || wait_schedule == CANCEL_THEN_COMPLETE) {
        gDevWmt.pCurOP = operation;
        wmt_lib_cancel_current_op(&gDevWmt);
        gDevWmt.pCurOP = NULL;
        if (wait_schedule == CANCEL) return 1;
    }
    consume_request(request);
    return wait_schedule == LATE_COMPLETE ? 0 : 1;
}
static void enable_psm(void)
{
    if (nested_coex) {
        nested_coex = 0;
        nested_result = wmt_dbg_cmd_test_api(WMTDRV_CMD_COEXDBG_04);
    }
    if (nested_cmd) {
        int command = nested_cmd;
        nested_cmd = 0;
        nested_result = run_ioctl(command, (ULONG)nested_user);
    }
}

/* PRODUCTION */

static const UINT8 original_input[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
static const UINT8 gps_reply[] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5};
static const UINT8 gps_request[] = {0x01, 0x32, 0x08, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
static const UINT8 first_loopback_reply[] = {0x41, 0x42, 0x43, 0x31, 0x32, 0x33};

static int all_released(void)
{
#if HAVE_OP_POOL
    REQUIRE(g_wmt_ops_checked_out == 0);
#endif
    REQUIRE(allocations_live == 0);
    REQUIRE(frees == allocations_used);
    REQUIRE(acquired == recycled);
    REQUIRE(!idc_locked);
    for (size_t i = 0; i < 4; i++) REQUIRE(!slot_used[i]);
    for (int i = 0; i < submitted; i++) REQUIRE(!requests[i].pending);
    return 0;
}

static int gps_case(int test)
{
    UINT8 tx[8], rx[12];
    UINT32 length = 0x13572468;
    memcpy(tx, original_input, sizeof(tx));
    memset(rx, 0xd4, sizeof(rx));
    if (test == 1 || test == 2) wait_schedule = TIMEOUT;
    if (test == 3 || test == 4) wait_schedule = CANCEL;
    if (test == 5) wait_schedule = LATE_COMPLETE;
    if (test == 6) wait_schedule = CANCEL_THEN_COMPLETE;
    if (test == 7) wire_tx_error = 1;
    if (test == 8) wire_tx_short = 1;
    if (test == 9) wire_rx_error = 1;
    if (test == 10) wire_bad_opcode = 1;
    if (test >= 11 && test <= 13) {
        if (!HAVE_PAYLOAD) return 77;
        fail_alloc_at = test - 10;
    }
    if (test == 14) psm_failure = 1;
    if (test == 15) queue_failure = 1;
    if (test == 16) coredump = 1;
    if (test == 50) wire_short_header = 1;
    if (test == 51) wire_short_payload = 1;
    if (test == 52) wire_announced_overrun = 1;
    if (test == 53) wire_reported_overrun = 1;
    if (test == 54) wire_empty = 1;
    int result = wmt_lib_gps_mcu_ctrl(tx, sizeof(tx), rx, sizeof(rx), &length);
    REQUIRE(result == (test == 0 || test == 54 ? 0 : -1));
    if (test == 0) {
        REQUIRE(length == 5);
        REQUIRE(memcmp(rx, gps_reply, sizeof(gps_reply)) == 0);
        for (size_t i = 5; i < sizeof(rx); i++) REQUIRE(rx[i] == 0xd4);
        REQUIRE(tx_wire_len == 12 && memcmp(tx_wire, gps_request, 12) == 0);
        REQUIRE(tx_calls == 1 && rx_calls == 1);
    } else if (test == 54) {
        REQUIRE(length == 0);
        for (size_t i = 0; i < sizeof(rx); i++) REQUIRE(rx[i] == 0xd4);
    } else {
        REQUIRE(length == 0x13572468);
        for (size_t i = 0; i < sizeof(rx); i++) REQUIRE(rx[i] == 0xd4);
    }
    if (test >= 1 && test <= 4) {
        REQUIRE(submitted == 1 && requests[0].pending);
        REQUIRE(requests[0].operation->ref_count == 1);
        if (HAVE_PAYLOAD) REQUIRE(allocations_live == 1 && recycled == 0);
        memset(tx, 0xee, sizeof(tx));
        memset(rx, 0xa6, sizeof(rx));
        length = 0x24681357;
        consume_request(0);
        if (test == 1 || test == 3)
            REQUIRE(tx_wire_len == 12 && memcmp(tx_wire, gps_request, 12) == 0);
        else {
            REQUIRE(length == 0x24681357);
            for (size_t i = 0; i < sizeof(rx); i++) REQUIRE(rx[i] == 0xa6);
        }
    }
    return all_released();
}

static void initialize_loopback(UINT8 *user, UINT8 fill)
{
    UINT32 length = 8;
    memset(user, 0xa6, 4096);
    memcpy(user, &length, sizeof(length));
    memset(user + sizeof(length), fill, length);
}
static int loopback_case(int test)
{
    UINT8 user[4096];
    initialize_loopback(user, 0x11);
    initialize_loopback(nested_user, 0x22);
    bool adie = test == 18 || test == 20 || test == 41 || test == 43 || test == 45;
    int command = adie ? WMT_IOCTL_ADIE_LPBK_TEST : WMT_IOCTL_LPBK_TEST;
    if (test == 19 || test == 20) nested_cmd = command;
    if (test == 21) wait_schedule = TIMEOUT;
    if (test == 40 || test == 41) fail_copy_to_at = 1;
    if (test == 42 || test == 43) bad_loopback_length = 1;
    if (test == 44 || test == 45) {
        if (!HAVE_PAYLOAD) return 77;
        fail_alloc_at = 1;
    }
    int result = run_ioctl(command, (ULONG)user);
    if (test == 21) {
        REQUIRE(result == -1 && submitted == 1 && requests[0].pending);
        P_OSAL_OP first = requests[0].operation;
        REQUIRE(first->ref_count == 1);
        REQUIRE(run_ioctl(command, (ULONG)nested_user) == -1);
        REQUIRE(submitted == 2);
        consume_request(0);
        consume_request(1);
        REQUIRE(requests[0].observed_len == 8);
        for (size_t i = 0; i < 8; i++) REQUIRE(requests[0].observed[i] == 0x11);
        for (size_t i = 0; i < 8; i++) REQUIRE(requests[1].observed[i] == 0x22);
    } else if (test >= 40) {
        REQUIRE(result < 0);
    } else {
        REQUIRE(result == 6);
        size_t offset = adie ? sizeof(SIZE_T) : sizeof(UINT32) + 2048;
        REQUIRE(memcmp(user + offset, first_loopback_reply, 6) == 0);
        REQUIRE(user[offset + 6] == 0xa6);
        if (test == 19 || test == 20) {
            static const UINT8 second_reply[] = {0x7a, 0x39, 0x38, 0x37, 0x36};
            REQUIRE(nested_result == 5);
            REQUIRE(memcmp(nested_user + offset, second_reply, 5) == 0);
        }
    }
    return all_released();
}

static int bgw_case(int test)
{
    UINT8 user[14] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    static const UINT8 expected[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    if (test == 23) wait_schedule = TIMEOUT;
    if (test == 46) {
        if (!HAVE_PAYLOAD) return 77;
        fail_alloc_at = 1;
    }
    int result = run_ioctl(WMT_IOCTL_SEND_BGW_DS_CMD, (ULONG)user);
    if (test == 46) REQUIRE(result < 0);
    else {
        REQUIRE(result == (test == 22 ? 14 : -1));
        if (test == 23) {
            REQUIRE(requests[0].operation->ref_count == 1);
            memset(user, 0x42, sizeof(user));
            /* ASan diagnoses the baseline's returned desense_buf stack object. */
            consume_request(0);
        }
        REQUIRE(requests[0].observed_len == 14);
        REQUIRE(memcmp(requests[0].observed, expected, 14) == 0);
    }
    return all_released();
}

static int patch_case(int test)
{
    UINT8 input[8];
    UINT32 version = 0x12345678;
    memcpy(input, original_input, sizeof(input));
    bool ant = test == 24 || test == 25 || test == 26 || test == 47;
    if (test == 25 || test == 28) wait_schedule = TIMEOUT;
    if (test == 26 || test == 29) wait_schedule = CANCEL;
    if (test == 47 || test == 48) {
        if (!HAVE_PAYLOAD) return 77;
        fail_alloc_at = 1;
    }
    int result = ant ? mtk_wcn_wmt_ant_ram_ctrl(WMT_ANT_RAM_DOWNLOAD, input, 8, WMT_ANT_RAM_START_PKT) :
        mtk_wcn_wmt_flash_patch_ctrl(WMT_FLASH_PATCH_DOWNLOAD, input, 8, WMT_FLASH_PATCH_START_PKT,
                                    WMT_FLASH_PATCH_FLP1, &version, 0x02030405);
    if (test == 47 || test == 48) REQUIRE(result != 2);
    else {
        REQUIRE(result == (test == 24 || test == 27 ? 2 : 5));
        if (wait_schedule != COMPLETE) {
            REQUIRE(requests[0].operation->ref_count == 1);
            memset(input, 0xee, sizeof(input));
            version = 0x87654321;
            consume_request(0);
        }
        REQUIRE(requests[0].observed_len == 8);
        REQUIRE(memcmp(requests[0].observed, original_input, 8) == 0);
    }
    return all_released();
}

static int idc_case(int test)
{
    /* Preserve the production packed local_para header and its msg_len - 1 copy. */
    union { uint64_t alignment; UINT8 bytes[sizeof(struct local_para) + 5]; } first, second;
    struct local_para *one = (struct local_para *)first.bytes;
    struct local_para *two = (struct local_para *)second.bytes;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    one->msg_len = sizeof(struct local_para) + 5;
    two->msg_len = sizeof(struct local_para) + 5;
    static const UINT8 first_data[] = {0x11, 0x22, 0x33, 0x44, 0x99};
    static const UINT8 second_data[] = {0xaa, 0xbb, 0xcc, 0xdd, 0x99};
    memcpy(one->data, first_data, 5);
    memcpy(two->data, second_data, 5);
    conn_md_ipc_ilm_t first_message = {.msg_id = IPC_MSG_ID_EL1_LTE_DEFAULT_PARAM_IND, .local_para_ptr = one};
    conn_md_ipc_ilm_t second_message = {.msg_id = IPC_MSG_ID_EL1_LTE_DEFAULT_PARAM_IND, .local_para_ptr = two};
    if (test == 31) wait_schedule = TIMEOUT;
    if (test == 32) wait_schedule = CANCEL;
    if (test == 49) {
        if (!HAVE_PAYLOAD) return 77;
        fail_alloc_at = 1;
    }
    int result = wmt_lib_handle_idc_msg(&first_message);
    REQUIRE(result == (test == 30 ? 1 : 0));
    if (test == 49) return all_released();
    if (test != 30) {
        REQUIRE(requests[0].operation->ref_count == 1);
        memset(one->data, 0xee, 5);
        REQUIRE(wmt_lib_handle_idc_msg(&second_message) == 0);
        REQUIRE(submitted == 2);
        consume_request(0);
        consume_request(1);
    }
    static const UINT8 expected[] = {0x05, 0x00, 0x11, 0x22, 0x33, 0x44};
    REQUIRE(requests[0].observed_len == 6);
    REQUIRE(memcmp(requests[0].observed, expected, 6) == 0);
    return all_released();
}

static int payload_case(int test)
{
#if HAVE_PAYLOAD
    P_OSAL_OP operation = wmt_lib_get_free_op();
    REQUIRE(operation != NULL);
    UINT8 *payload = wmt_lib_alloc_op_data(operation, 16);
    REQUIRE(payload != NULL && allocations_live == 1);
    if (test == 36) {
        for (size_t i = 0; i < 16; i++) REQUIRE(payload[i] == 0);
        REQUIRE(wmt_lib_put_op_to_free_queue(operation) == 0);
    } else if (test == 37) {
        REQUIRE(wmt_lib_alloc_op_data(operation, 8) == NULL);
        REQUIRE(allocations_live == 1 && allocations_used == 1);
        REQUIRE(wmt_lib_put_op_to_free_queue(operation) == 0);
    } else if (test == 38) {
        REQUIRE(wmt_lib_put_op_to_free_queue(operation) == 0);
    } else {
        memset(payload, 0x11, 16);
        operation->op.opId = WMT_OPID_BGW_DS;
        operation->op.au4OpData[0] = 16;
        operation->op.au4OpData[1] = (SIZE_T)payload;
        operation->signal.timeoutValue = test == 39 ? 0 : 2000;
        OSAL_OP_DAT result;
        if (test == 34) wait_schedule = TIMEOUT;
        if (test == 35) wait_schedule = CANCEL;
        if (test == 39) {
            REQUIRE(wmt_lib_put_act_op(operation));
        } else {
            REQUIRE(wmt_lib_submit_op_result(operation, &result) == (test == 33));
            REQUIRE(allocations_live == 1 && recycled == 0);
            REQUIRE(operation->ref_count == (test == 33 ? 1 : 2));
            for (size_t i = 0; i < 16; i++) REQUIRE(payload[i] == 0x11);
            wmt_lib_put_op_ref(operation);
        }
        if (test != 33) {
            REQUIRE(allocations_live == 1 && recycled == 0 && operation->ref_count == 1);
            consume_request(0);
        }
    }
    return all_released();
#else
    (void)test;
    puts("SKIP: baseline has no operation-owned payload API");
    return 77;
#endif
}

static int coex_case(int test)
{
    memset(gCoexBuf.buffer, 0xa6, sizeof(gCoexBuf.buffer));
    gCoexBuf.availSize = 11;
    if (test == 56) wait_schedule = TIMEOUT;
    if (test == 57) wait_schedule = CANCEL;
    if (test == 58) nested_coex = 1;
    if (test == 59) {
        if (!HAVE_PAYLOAD) return 77;
        fail_alloc_at = 1;
    }
    int result = wmt_dbg_cmd_test_api(WMTDRV_CMD_COEXDBG_03);
    if (test == 59) {
        REQUIRE(result < 0 || submitted == 0);
        return all_released();
    }
    REQUIRE(result == 0);
    if (test == 56 || test == 57) {
        REQUIRE(requests[0].operation->ref_count == 1);
        wait_schedule = COMPLETE;
        REQUIRE(wmt_dbg_cmd_test_api(WMTDRV_CMD_COEXDBG_04) == 0);
        REQUIRE(gCoexBuf.availSize == 5);
        consume_request(0);
        static const UINT8 second_reply[] = {0x7a, 0x39, 0x38, 0x37, 0x36};
        REQUIRE(gCoexBuf.availSize == 5);
        REQUIRE(memcmp(gCoexBuf.buffer, second_reply, 5) == 0);
    } else {
        REQUIRE(gCoexBuf.availSize == 6);
        REQUIRE(memcmp(gCoexBuf.buffer, first_loopback_reply, 6) == 0);
        if (test == 58) REQUIRE(nested_result == 0);
    }
    return all_released();
}

static int boundary_case(int test)
{
    UINT8 user[4096], rx[16];
    UINT32 length = 0x12345678;
    initialize_loopback(user, 0x11);
    memset(rx, 0xa6, sizeof(rx));
    if (test >= 60 && test <= 62) {
        fail_copy_from_at = test == 61 ? 2 : 1;
        int command = test == 62 ? WMT_IOCTL_SEND_BGW_DS_CMD : WMT_IOCTL_LPBK_TEST;
        REQUIRE(run_ioctl(command, (ULONG)user) == -EFAULT);
    } else if (test >= 63 && test <= 65) {
        REQUIRE(wmt_lib_gps_mcu_ctrl(user, test == 64 ? 65536 : 8, rx,
                                    test == 65 ? 65536 : sizeof(rx), test == 63 ? NULL : &length) < 0);
        REQUIRE(length == 0x12345678);
        for (size_t i = 0; i < sizeof(rx); i++) REQUIRE(rx[i] == 0xa6);
    } else if (test >= 66 && test <= 69) {
        struct local_para header = {.msg_len = test == 67 ? sizeof(header) - 1 :
            test == 68 ? sizeof(header) : sizeof(header) + WMT_IDC_MSG_MAX_SIZE + 1};
        conn_md_ipc_ilm_t message = {.msg_id = IPC_MSG_ID_EL1_LTE_DEFAULT_PARAM_IND,
                                    .local_para_ptr = &header};
        REQUIRE(wmt_lib_handle_idc_msg(test == 66 ? NULL : &message) == MTK_WCN_BOOL_FALSE);
    } else if (test == 70) {
        REQUIRE(mtk_wcn_wmt_flash_patch_ctrl(WMT_FLASH_PATCH_VERSION_GET, NULL, 0,
                    WMT_FLASH_PATCH_START_PKT, WMT_FLASH_PATCH_FLP1, NULL, 0) == WMT_FLASH_PATCH_PARA_ERR);
    } else if (test == 71) {
#if HAVE_PAYLOAD
        P_OSAL_OP op = wmt_lib_get_free_op();
        REQUIRE(wmt_lib_alloc_op_data(op, (SIZE_T)UINT32_MAX + 1) == NULL);
        REQUIRE(alloc_attempts == 0);
        wmt_lib_put_op_to_free_queue(op);
#else
        return 77;
#endif
    }
    REQUIRE(submitted == 0);
    return all_released();
}

int main(int argc, char **argv)
{
    REQUIRE(argc == 2);
    int test = atoi(argv[1]);
    int result;
    if ((test >= 0 && test <= 16) || (test >= 50 && test <= 54)) result = gps_case(test);
    else if ((test >= 17 && test <= 21) || (test >= 40 && test <= 45)) result = loopback_case(test);
    else if (test == 22 || test == 23 || test == 46) result = bgw_case(test);
    else if ((test >= 24 && test <= 29) || test == 47 || test == 48) result = patch_case(test);
    else if ((test >= 30 && test <= 32) || test == 49) result = idc_case(test);
    else if (test >= 33 && test <= 39) result = payload_case(test);
    else if (test >= 55 && test <= 59) result = coex_case(test);
    else if (test >= 60 && test <= 71) result = boundary_case(test);
    else return 2;
    if (result == 0) puts("PASS");
    return result;
}
