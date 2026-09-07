#!/usr/bin/env python3
"""Exercise operation result ownership and completion/reference ordering."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

from test_wmt_command import function

PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
static pthread_mutex_t g_wmt_op_lock __attribute__((unused)) = PTHREAD_MUTEX_INITIALIZER;
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; pthread_mutex_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); pthread_mutex_unlock(lock); } while (0)
#define READ_ONCE(value) __atomic_load_n(&(value), __ATOMIC_RELAXED)
#define WRITE_ONCE(value, data) __atomic_store_n(&(value), (data), __ATOMIC_RELAXED)
typedef void VOID;
typedef int INT32;
typedef unsigned int UINT32;
typedef unsigned char UINT8;
typedef unsigned char *PUINT8;
typedef void *PVOID;
typedef unsigned long ULONG;
typedef unsigned long SIZE_T;
typedef bool MTK_WCN_BOOL;
#define MTK_WCN_BOOL_FALSE false
#define MTK_WCN_BOOL_TRUE true
#define WMT_OPID_HW_RST 1
#define WMT_OPID_SW_RST 2
#define WMT_OPID_GPIO_STATE 3
#define WMT_OPID_FUNC_ON 4
#define WMTDRV_TYPE_WIFI 5
#define WMT_OPID_LPBK 6
#define WMT_OPID_ADIE_LPBK_TEST 7
#define WMT_OPID_BGW_DS 8
#define WMT_OPID_WLAN_PROBE 9
#define WMT_OPID_WLAN_REMOVE 10
#define WMT_OPID_EXIT 11
#define WMT_OPID_FUNC_OFF 12
#define WMT_STAT_RST_ON 0
#define WMT_IOCTL_LPBK_TEST 1
#define WMT_IOCTL_ADIE_LPBK_TEST 2
#define WMT_IOCTL_SEND_BGW_DS_CMD 3
#define MAX_EACH_WMT_CMD 2000
#define osal_assert assert
#define osal_free free
#define osal_malloc malloc
#define osal_memset memset
#define osal_memcpy memcpy
#define WMT_ERR_FUNC(...) ((void)0)
#define WMT_WARN_FUNC(...) ((void)0)
#define WMT_DBG_FUNC(...) ((void)0)
#define WMT_INFO_FUNC(...) ((void)0)
typedef int atomic_t;
typedef struct { unsigned int timeoutValue; } OSAL_SIGNAL, *P_OSAL_SIGNAL;
typedef struct { unsigned int timeoutValue; } OSAL_EVENT, *P_OSAL_EVENT;
'''

STUBS = r'''
typedef struct {
    int rActiveOpQ, rFreeOpQ, rWorkerOpQ, worker_thread, thread, wmtd_op_history;
    OSAL_EVENT rWmtdWq, rWmtdWorkerWq;
    unsigned long state;
} DEVICE, *P_DEV_WMT;
static DEVICE gDevWmt;
static OSAL_OP operation;
static P_OSAL_OP active;
static int recycled, signal_calls, access_after_release;
static int queue_failure, coredump, timeout, completion_error, no_result_pointer;
static int release_sender_in_signal, release_sender_after_consumer_drop;
static int handoff_mode, worker_queue_failure, worker_published, wait_calls;
static int last_signal_result, mutate_submit_id, waited_worker;
static atomic_t g_wifi_on_off_ready;
static P_OSAL_OP current_op;
static UINT8 gLpbkBuf[2048];
static UINT32 gLpbkBufLog;
static VOID wmt_lib_complete_op(P_OSAL_OP op, INT32 result);
static INT32 wmt_core_opid(P_OSAL_OP_DAT data);

static void recycle(P_OSAL_OP op)
{
    assert(op->ref_count == 0);
    memset(op, 0, sizeof(*op));
    recycled++;
}
static int atomic_dec_and_test(atomic_t *value)
{
    int was_last = --*value == 0;
    if (release_sender_after_consumer_drop && value == &active->ref_count && !was_last) {
        release_sender_after_consumer_drop = 0;
        assert(--*value == 0);
        recycle(active);
    }
    return was_last;
}
#define atomic_set(p, v) (*(p) = (v))
#define atomic_inc(p) (++*(p))
#define atomic_dec(p) (--*(p))
static int wmt_lib_put_op(int *queue, P_OSAL_OP op)
{
    if (queue == &gDevWmt.rActiveOpQ) {
        if (queue_failure) return false;
        active = op;
        if (mutate_submit_id) op->op.opId = WMT_OPID_WLAN_PROBE;
        return true;
    }
    if (queue == &gDevWmt.rWorkerOpQ) {
        if (worker_queue_failure) return false;
        worker_published++;
        if (handoff_mode == 1) wmt_lib_complete_op(op, 0);
        return true;
    }
    assert(queue == &gDevWmt.rFreeOpQ);
    recycle(op);
    return true;
}
static int mtk_wcn_stp_coredump_start_get(void) { return coredump; }
static void osal_signal_init(P_OSAL_SIGNAL signal) { (void)signal; }
static void osal_trigger_event(void *event) { (void)event; }
static int osal_wait_for_signal_timeout(P_OSAL_SIGNAL signal, int *thread)
{
    (void)signal;
    waited_worker = thread == &gDevWmt.worker_thread;
    if (timeout == 1) return 0;
    active->op.au4OpData[0] = 64;
    active->op.au4OpData[5] = 42;
    if ((active->op.opId == WMT_OPID_LPBK || active->op.opId == WMT_OPID_ADIE_LPBK_TEST) &&
        active->op.au4OpData[1])
        memset((void *)active->op.au4OpData[1], 0x6c, 64);
    wmt_lib_complete_op(active, completion_error ? -3 : 0);
    assert(active->ref_count == 1);
    /* A completion can arrive after the wait deadline but before return. */
    return timeout == 2 ? 0 : 1;
}
static int osal_op_is_wait_for_signal(P_OSAL_OP op)
{
    if (recycled) access_after_release++;
    return op->signal.timeoutValue != 0;
}
static void osal_op_raise_signal(P_OSAL_OP op, int result)
{
    if (recycled) access_after_release++;
    op->result = result;
    last_signal_result = result;
    signal_calls++;
    if (release_sender_in_signal) {
        if (--op->ref_count == 0) recycle(op);
        /* Model the completion implementation still using its object. */
        if (recycled) access_after_release++;
    }
}
static P_OSAL_OP wmt_lib_get_free_op(void) { return &operation; }
static int wmt_lib_put_op_to_free_queue(P_OSAL_OP op) { (void)op; return 0; }
static int mtk_wcn_stp_is_ready(void) { return true; }
#define DISABLE_PSM_MONITOR() 0
#define ENABLE_PSM_MONITOR() ((void)0)
static int copy_from_user(void *to, const void *from, size_t count) { memcpy(to, from, count); return 0; }
static int copy_to_user(void *to, const void *from, size_t count) { memcpy(to, from, count); return 0; }
static void osal_thread_wait_for_event(int *thread, P_OSAL_EVENT event, UINT32 (*check)(void))
{ (void)thread; (void)event; (void)check; wait_calls++; }
static int osal_thread_should_stop(int *thread) { (void)thread; return wait_calls > 1; }
static UINT32 wmt_lib_wait_event_checker(void) { return 0; }
static P_OSAL_OP wmt_lib_get_op(int *queue) { assert(queue == &gDevWmt.rActiveOpQ); return active; }
static void osal_op_history_save(int *history, P_OSAL_OP op) { (void)history; (void)op; }
static int osal_test_bit(int bit, unsigned long *value) { (void)bit; (void)value; return 0; }
static void wmt_lib_set_current_op(P_DEV_WMT dev, P_OSAL_OP op) { (void)dev; current_op = op; }
static P_OSAL_OP wmt_lib_get_current_op(P_DEV_WMT dev) { (void)dev; return current_op; }
'''

TESTS = r'''
#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "line %d: %s\n", __LINE__, #value); return 1; } } while (0)
int main(int argc, char **argv)
{
    REQUIRE(argc == 2);
    int test = atoi(argv[1]);
    operation.op.opId = WMT_OPID_LPBK;
    operation.op.au4OpData[0] = 17;
    operation.signal.timeoutValue = 2000;
    if (test >= 7 && test <= 9) {
        active = &operation;
        operation.ref_count = test == 9 ? 1 : 2;
        if (test == 7) release_sender_in_signal = 1;
        if (test == 8) release_sender_after_consumer_drop = 1;
        if (test == 9) operation.signal.timeoutValue = 0;
        wmt_lib_complete_op(&operation, 0);
        REQUIRE(recycled == 1 && access_after_release == 0);
        REQUIRE(signal_calls == (test == 9 ? 0 : 1));
    } else if ((test >= 14 && test <= 17) || test == 19) {
        operation.ref_count = 2; /* submitting thread and regular worker */
        operation.op.opId = test == 17 ? WMT_OPID_EXIT : WMT_OPID_FUNC_ON;
        if (test == 19) operation.op.opId = WMT_OPID_FUNC_OFF;
        operation.op.au4OpData[0] = WMTDRV_TYPE_WIFI;
        active = &operation;
        release_sender_in_signal = 1;
        handoff_mode = test == 17 ? 0 : test - 13;
        if (test == 19) handoff_mode = 1;
        worker_queue_failure = handoff_mode == 3;
        REQUIRE(wmtd_thread(&gDevWmt) == 0);
        if (test == 15) {
            REQUIRE(signal_calls == 0 && recycled == 0 && operation.ref_count == 2);
            wmt_lib_complete_op(&operation, 0);
        }
        REQUIRE(signal_calls == 1 && recycled == 1 && access_after_release == 0);
        if (test == 16) REQUIRE(last_signal_result == -4 && worker_published == 0);
        if (test == 17) REQUIRE(wait_calls == 1);
    } else if (test >= 11 && test <= 13) {
        unsigned char userspace[4096] = {0};
        UINT32 length = 64;
        memcpy(userspace, &length, sizeof(length));
        memset(userspace + sizeof(length), 0x6c, length);
        int commands[] = {WMT_IOCTL_SEND_BGW_DS_CMD, WMT_IOCTL_LPBK_TEST, WMT_IOCTL_ADIE_LPBK_TEST};
        REQUIRE(run_ioctl(commands[test - 11], (ULONG)userspace) == 64);
        REQUIRE(recycled == 1);
        if (test != 11) {
            size_t offset = test == 12 ? sizeof(UINT32) + 2048 : sizeof(SIZE_T);
            for (size_t i = 0; i < 64; i++) REQUIRE(userspace[offset + i] == 0x6c);
        }
    } else {
        OSAL_OP_DAT result;
        memset(&result, 0xa5, sizeof(result));
        if (test == 1) completion_error = 1;
        if (test == 2) queue_failure = 1;
        if (test == 3) coredump = 1;
        if (test == 4) timeout = 1;
        if (test == 5) timeout = 2;
        if (test == 6) operation.signal.timeoutValue = 0;
        if (test == 18) {
            operation.op.opId = WMT_OPID_FUNC_ON;
            operation.op.au4OpData[0] = WMTDRV_TYPE_WIFI;
            mutate_submit_id = 1;
        }
        no_result_pointer = test == 10;
        int success = no_result_pointer ? wmt_lib_put_act_op(&operation) :
                      wmt_lib_put_act_op_result(&operation, &result);
        REQUIRE(success == (test == 0 || test == 6 || test == 10 || test == 18));
        if (test == 18) REQUIRE(waited_worker);
        REQUIRE(recycled == (test == 4 || test == 6 ? 0 : 1));
        if (test == 4 || test == 6) REQUIRE(operation.ref_count == 1);
        if (!no_result_pointer) {
            REQUIRE(result.au4OpData[0] == (test == 0 || test == 1 || test == 18 ? 64 : 17));
            if (test == 1) REQUIRE(result.au4OpData[5] == 42);
        }
    }
    puts("PASS");
    return 0;
}
'''

CASES = ['completed-result-survives-reuse', 'completed-error-details-survive-reuse',
         'queue-failure-keeps-submitted-fields', 'coredump-rejection-keeps-submitted-fields',
         'timeout-retains-consumer-reference', 'late-completion-does-not-reverse-timeout',
         'async-operation-keeps-consumer-reference', 'consumer-held-through-signal',
         'waiter-times-out-during-consumer-release', 'async-consumer-final-release',
         'legacy-no-result-wrapper', 'bgw-ioctl-result', 'loopback-ioctl-result', 'adie-ioctl-result',
         'wifi-worker-finishes-before-dispatch-returns', 'wifi-worker-delayed-completion',
         'wifi-worker-queue-failure', 'worker-exit-after-final-release', 'wait-target-before-id-mutation',
         'wifi-remove-finishes-before-dispatch-returns']


def op_pool_stubs(checked_out=1):
    # Admission is open in these operation tests; dedicated pool tests own shutdown.
    return ('\n#define HAS_OP_POOL 1\n'
            'static pthread_mutex_t g_wmt_op_pool_lock = PTHREAD_MUTEX_INITIALIZER;\n'
            'static bool g_wmt_op_pool_stopping;\n'
            'static atomic_t g_wmt_ops_checked_out = '+str(checked_out)+';\n'
            'static int g_wmt_op_pool_idle;\n'
            '#define mutex_lock pthread_mutex_lock\n'
            '#define mutex_unlock pthread_mutex_unlock\n'
            '#define wake_up(queue) ((void)(queue))\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[7])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitizer', choices=['address', 'thread'], default='address')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    base = args.kernel / 'drivers/misc/mediatek/connectivity/source/common/common_main'
    paths = [base / name for name in ['core/wmt_lib.c', 'linux/wmt_dev.c', 'linux/include/osal.h', 'core/wmt_core.c']]
    lib, dev, osal, core = [p.read_text() for p in paths]
    data_size = re.search(r'^#define OSAL_OP_DATA_SIZE[^\n]*', osal, re.M)[0] + '\n'
    types = ''.join(re.search(r'^typedef struct ' + tag + r' \{.*?^\}[^;]*;', osal, re.M | re.S)[0] + '\n'
                    for tag in ['_OSAL_OP_DAT', '_OSAL_LXOP_'])
    submit = function(lib, 'wmt_lib_put_act_op_result', required=False)
    owned_helpers = ''.join(function(lib, name, required=False) for name in [
        'wmt_lib_put_op_to_free_queue', 'wmt_lib_alloc_op_data', 'wmt_lib_put_op_ref',
        'wmt_lib_queue_op', 'wmt_lib_submit_op_result']) if 'wmt_lib_alloc_op_data' in lib else ''
    production = owned_helpers + submit + function(lib, 'wmt_lib_put_act_op')
    if not submit:
        # Existing callers access pOp->op after the consuming submission returns.
        scheduling_point = 'sched_yield(); ' if args.sanitizer == 'thread' else ''
        production += ('static MTK_WCN_BOOL wmt_lib_put_act_op_result(P_OSAL_OP op, P_OSAL_OP_DAT result) {\n'
                       'int ret = wmt_lib_put_act_op(op); ' + scheduling_point +
                       '*result = op->op; return ret; }\n')
    complete = function(lib, 'wmt_lib_complete_op', required=False)
    if not complete:
        worker = function(lib, 'wmtd_worker_thread')
        start = worker.index('\t\tif (atomic_dec_and_test(&pOp->ref_count))')
        end = worker.index('\n\t}', start)
        complete = ('static VOID wmt_lib_complete_op(P_OSAL_OP pOp, INT32 iResult) {\n'
                    'P_DEV_WMT pWmtDev = &gDevWmt;\n' + worker[start:end] + '\n}\n')
    branches = ''
    for name in ['WMT_IOCTL_LPBK_TEST', 'WMT_IOCTL_ADIE_LPBK_TEST', 'WMT_IOCTL_SEND_BGW_DS_CMD']:
        start = dev.index('\tcase ' + name + ':')
        branches += dev[start:dev.index('\tcase ', start + 1)]
    ioctl = ('static INT32 run_ioctl(UINT32 cmd, ULONG arg) { INT32 iRet = 0; switch (cmd) {\n'
             + branches + '\n} return iRet; }\n')
    c_file = args.output / 'test.c'
    prefix, stubs, tests = PREFIX, STUBS, TESTS
    if owned_helpers:
        stubs = stubs.replace('static int wmt_lib_put_op_to_free_queue(P_OSAL_OP op) { (void)op; return 0; }', '')
        stubs = stubs.replace('static UINT8 gLpbkBuf[2048];', '').replace('static UINT32 gLpbkBufLog;', '')
        core_header = (base/'core/include/wmt_core.h').read_text()
        prefix += ''.join(re.search(r'^#define '+name+r'[^\n]*', core_header, re.M)[0]+'\n'
                          for name in ['WMT_LPBK_CMD_LEN', 'WMT_LPBK_BUF_LEN'])
    dispatch = function(lib, 'wmt_lib_put_worker_op')
    # The actual Wi-Fi branch changes the ID and publishes the worker request.
    marker = '\t\t\tif (drvType == WMTDRV_TYPE_WIFI) {'
    start = core.index(marker) + len(marker)
    end = core.index('\n\t\t\t}', start)
    off_start = core.index(marker, end) + len(marker)
    off_end = core.index('\n\t\t\t}', off_start)
    dispatch += ('static INT32 wmt_core_opid(P_OSAL_OP_DAT data) {\n'
                 'if (!handoff_mode) return 0;\n'
                 'if (data->opId == WMT_OPID_FUNC_OFF) {\n' + core[off_start:off_end] + '\n}\n'
                 + core[start:end] + '\n}\n')
    dispatch += function(lib, 'wmtd_thread')
    if args.sanitizer == 'thread':
        prefix = '#include <stdatomic.h>\n' + prefix.replace('typedef int atomic_t;', 'typedef atomic_int atomic_t;')
        stubs, tests = Path(__file__).with_name('wmt_operation_threads.c').read_text().split('/* TESTS */')
        ioctl = ''
        dispatch = ''
    if 'wmt_lib_queue_op' in lib:
        prefix += op_pool_stubs(0 if args.sanitizer == 'thread' else 1)
    else:
        prefix += '#define HAS_OP_POOL 0\n'
    state_enum = re.search(r'enum wmt_op_state \{.*?\};', lib, re.S)
    state_enum = state_enum[0] + '\n' if state_enum else ''
    c_file.write_text(prefix + data_size + types + state_enum + stubs + production + complete + dispatch + ioctl + tests)
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-Wno-unused-but-set-variable', '-Wno-sign-compare', '-pthread',
        '-fsanitize=' + ('thread' if args.sanitizer == 'thread' else 'address,undefined'), '-no-pie',
        '-fno-omit-frame-pointer', str(c_file), '-o', str(args.output / 'test')]
    subprocess.run(command, check=True)
    rows = []
    cases = ['two-senders-one-reused-slot'] if args.sanitizer == 'thread' else CASES
    for number, name in enumerate(cases):
        run = subprocess.run([str(args.output / 'test'), str(number)], capture_output=True, timeout=45,
                             env=dict(os.environ, TSAN_OPTIONS='halt_on_error=1'))
        (args.output / (name + '.txt')).write_bytes(run.stdout + run.stderr)
        rows.append(dict(case=name, exit_code=run.returncode, passed=run.returncode == 0))
        print(('PASS: ' if run.returncode == 0 else 'FAIL: ') + name, flush=True)
    result = dict(passed=sum(row['passed'] for row in rows), total=len(rows), cases=rows,
                  source_sha256={str(p.relative_to(args.kernel)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  fixture_sha256=hashlib.sha256(c_file.read_bytes()).hexdigest(), compiler=command,
                  sanitizer=args.sanitizer)
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(result['passed'] != result['total'])


if __name__ == '__main__':
    main()
