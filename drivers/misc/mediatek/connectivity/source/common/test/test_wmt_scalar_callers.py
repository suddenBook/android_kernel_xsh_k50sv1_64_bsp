#!/usr/bin/env python3
"""Exercise production thermal/desense callers across consumed operation slots."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

from test_wmt_command import function
from test_wmt_operation import op_pool_stubs


PREFIX = r'''
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void VOID;
typedef void *PVOID;
typedef int8_t INT8;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef size_t SIZE_T;
typedef int MTK_WCN_BOOL;
typedef int atomic_t;
typedef struct { unsigned int timeoutValue; } OSAL_SIGNAL, *P_OSAL_SIGNAL;
#define MTK_WCN_BOOL_FALSE 0
#define MTK_WCN_BOOL_TRUE 1
#define MAX_EACH_WMT_CMD 2000
#define osal_assert assert
#define osal_free free
#define atomic_set(p, v) (*(p) = (v))
#define atomic_inc(p) (++*(p))
#define atomic_dec(p) (--*(p))
#define atomic_dec_and_test(p) (--*(p) == 0)
static pthread_mutex_t g_wmt_op_lock = PTHREAD_MUTEX_INITIALIZER;
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; pthread_mutex_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); pthread_mutex_unlock(lock); } while (0)
#define WRITE_ONCE(value, data) ((value) = (data))
#define WMT_STEP_DO_ACTIONS_FUNC(point) ((void)0)
static void host_log(const char *format, ...);
#define WMT_ERR_FUNC(...) host_log(__VA_ARGS__)
#define WMT_WARN_FUNC(...) host_log(__VA_ARGS__)
#define WMT_DBG_FUNC(...) host_log(__VA_ARGS__)
#define WMT_INFO_FUNC(...) host_log(__VA_ARGS__)
#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "line %d: %s\n", __LINE__, #value); return 1; } } while (0)
'''

STUBS = r'''
typedef P_OSAL_OP_DAT P_WMT_OP;
typedef struct {
    int rActiveOpQ, rFreeOpQ, rWorkerOpQ, rWmtdWq, rWmtdWorkerWq, worker_thread, thread;
    P_OSAL_OP pCurOP;
} DEVICE, *P_DEV_WMT;
static DEVICE gDevWmt;
static OSAL_OP operation;
static OSAL_OP_DAT retained_data;
enum scenario {
    SUCCESS, COMPLETION_ERROR, CANCELLED, WAKE_ERROR, POOL_EMPTY,
    UNSUPPORTED, INVALID_COMMAND, ENQUEUE_ERROR, COREDUMP, TIMEOUT
};
static int scenario, kind, acquired, submitted, recycled, returned_unsubmitted;
static int signal_calls, monitor_disabled, monitor_enabled, reply_log_count;
static bool caller_resumed;
static char reply_log[256];
static VOID wmt_lib_complete_op(P_OSAL_OP op, INT32 result);
static VOID wmt_lib_cancel_current_op(P_DEV_WMT dev);
static INT32 wmt_lib_set_current_op(P_DEV_WMT dev, P_OSAL_OP op);

/* Evaluate and format every argument, including diagnostics after submission. */
static void host_log(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (caller_resumed && !strncmp(line, "OPID(", 5)) {
        snprintf(reply_log, sizeof(reply_log), "%.255s", line);
        reply_log_count++;
    }
}

static P_OSAL_OP wmt_lib_get_free_op(void)
{
    if (scenario == POOL_EMPTY) return NULL;
    memset(&operation, 0, sizeof(operation));
    acquired++;
    return &operation;
}
static int wmt_lib_put_op_to_free_queue(P_OSAL_OP op)
{
    assert(op == &operation && !submitted && !op->ref_count);
    returned_unsubmitted++;
    return 0;
}
static int wmt_lib_is_therm_ctrl_support(ENUM_WMTTHERM_TYPE_T type)
{ (void)type; return scenario != UNSUPPORTED; }
static int wmt_lib_is_dsns_ctrl_support(void) { return scenario != UNSUPPORTED; }
static int disable_monitor(void) { monitor_disabled++; return scenario == WAKE_ERROR; }
static void enable_monitor(void) { monitor_enabled++; caller_resumed = true; }
#define DISABLE_PSM_MONITOR() disable_monitor()
#define ENABLE_PSM_MONITOR() enable_monitor()
static int mtk_wcn_stp_coredump_start_get(void) { return scenario == COREDUMP; }
static void osal_signal_init(P_OSAL_SIGNAL signal) { (void)signal; }
static void osal_trigger_event(int *event) { (void)event; }
static int osal_op_is_wait_for_signal(P_OSAL_OP op)
{ return op && op->signal.timeoutValue; }
static void osal_op_raise_signal(P_OSAL_OP op, int result)
{ op->result = result; signal_calls++; }

static int wmt_lib_put_op(int *queue, P_OSAL_OP op)
{
    assert(op == &operation);
    if (queue == &gDevWmt.rActiveOpQ) {
        if (scenario == ENQUEUE_ERROR) return false;
        submitted++;
        wmt_lib_set_current_op(&gDevWmt, op);
    } else {
        assert(queue == &gDevWmt.rFreeOpQ && !op->ref_count && !gDevWmt.pCurOP);
        if (scenario == WAKE_ERROR) { returned_unsubmitted++; return true; }
        recycled++;
        /* Another caller obtains the released slot before this caller resumes. */
        memset(op, 0, sizeof(*op));
        op->op.opId = 222;
        for (size_t i = 0; i < OSAL_OP_DATA_SIZE; i++)
            op->op.au4OpData[i] = 0xfeed0000UL + i;
        op->ref_count = 1;
        retained_data = op->op;
    }
    return true;
}

static int osal_wait_for_signal_timeout(P_OSAL_SIGNAL signal, int *thread)
{
    (void)signal; (void)thread;
    assert(operation.ref_count == 2 && gDevWmt.pCurOP == &operation);
    if (scenario == CANCELLED || scenario == TIMEOUT) {
        if (kind != 2) operation.op.au4OpData[1] = 0x55; /* Partial thermal reply. */
        retained_data = operation.op;
        if (scenario == CANCELLED) wmt_lib_cancel_current_op(&gDevWmt);
        return scenario == CANCELLED ? 1 : 0;
    }
    if (kind != 2) operation.op.au4OpData[1] = kind == 0 ? 0xd2 : 1;
    wmt_lib_set_current_op(&gDevWmt, NULL);
    wmt_lib_complete_op(&operation, scenario == COMPLETION_ERROR ? -3 : 0);
    assert(operation.ref_count == 1);
    return 1;
}
'''

TESTS = r'''
int main(int argc, char **argv)
{
    REQUIRE(argc == 3);
    kind = atoi(argv[1]); scenario = atoi(argv[2]);
    int command = kind == 2 ? WMTDSNS_FM_GPS_ENABLE :
                  (kind == 0 ? WMTTHERM_READ : WMTTHERM_ENABLE);
    if (scenario == INVALID_COMMAND)
        command = kind == 2 ? WMTDSNS_MAX : WMTTHERM_MAX + 1;
    int result = kind == 2 ? mtk_wcn_wmt_dsns_ctrl(command) : mtk_wcn_wmt_therm_ctrl(command);
    bool early = scenario >= WAKE_ERROR && scenario <= INVALID_COMMAND;
    int expected = 0;
    if (scenario == SUCCESS) expected = kind == 0 ? (INT8)0xd2 : 1;
    else if (scenario == WAKE_ERROR && kind != 2) expected = -1;
    else if (!early && kind == 0) expected = -1;

    if (early) {
        REQUIRE(!submitted && !recycled && !signal_calls && !monitor_enabled && !reply_log_count);
        REQUIRE(acquired == (scenario == WAKE_ERROR));
        REQUIRE(returned_unsubmitted == (scenario == WAKE_ERROR));
        REQUIRE(monitor_disabled == (scenario == WAKE_ERROR));
    } else {
        REQUIRE(acquired == 1 && !returned_unsubmitted && monitor_disabled == 1 && monitor_enabled == 1);
        /* Check corruption independently of return value or log correctness. */
        REQUIRE(!memcmp(&operation.op, &retained_data, sizeof(retained_data)));
        char expected_log[96];
        snprintf(expected_log, sizeof(expected_log), "OPID(%u) type(%zu)",
                 kind == 2 ? WMT_OPID_DSNS : WMT_OPID_THERM_CTRL,
                 (size_t)(kind == 2 ? WMTDRV_TYPE_FM : command));
        REQUIRE(reply_log_count == 1);
        REQUIRE(!strncmp(reply_log, expected_log, strlen(expected_log)));
        if (scenario == CANCELLED || scenario == TIMEOUT) {
            REQUIRE(submitted == 1 && !recycled && operation.ref_count == 1);
            REQUIRE(gDevWmt.pCurOP == &operation);
            REQUIRE(signal_calls == (scenario == CANCELLED));
            /* Consumer remains free to finish after the caller has returned. */
            operation.op.au4OpData[1] = 0x42;
            wmt_lib_set_current_op(&gDevWmt, NULL);
            wmt_lib_complete_op(&operation, 0);
            REQUIRE(recycled == 1 && signal_calls == 1);
        } else {
            REQUIRE(recycled == 1 && operation.ref_count == 1 && !gDevWmt.pCurOP);
            REQUIRE(submitted == (scenario == SUCCESS || scenario == COMPLETION_ERROR));
            REQUIRE(signal_calls == submitted);
        }
    }
    REQUIRE(result == expected);
    printf("PASS kind=%d scenario=%d result=%d recycled=%d signals=%d\n",
           kind, scenario, result, recycled, signal_calls);
    return 0;
}
'''

CASES = [
    'success-slot-reuse', 'failed-completion-slot-reuse', 'cancel-retains-consumer-data',
    'wake-failure', 'pool-empty', 'unsupported', 'invalid-command',
    'enqueue-failure', 'coredump-rejection', 'timeout-retains-consumer-data',
]
KINDS = ['thermal-read', 'thermal-enable', 'desense']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[7])
    parser.add_argument('--revision', help='Extract a fixed Git revision instead of the worktree')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.kernel = args.kernel.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    revision = None
    if args.revision:
        revision = subprocess.check_output(
            ['git', '-C', str(args.kernel), 'rev-parse', '--verify', args.revision + '^{commit}'],
            text=True).strip()
    base = Path('drivers/misc/mediatek/connectivity/source/common/common_main')
    paths = [base / name for name in ['core/wmt_exp.c', 'core/wmt_lib.c', 'linux/include/osal.h',
                                     'include/wmt_exp.h', 'core/include/wmt_core.h']]
    sources = {str(path): (subprocess.check_output(
        ['git', '-C', str(args.kernel), 'show', revision + ':' + str(path)])
        if revision else (args.kernel / path).read_bytes()) for path in paths}
    exp, lib, osal, exp_header, core_header = [sources[str(path)].decode() for path in paths]
    types = re.search(r'^#define OSAL_OP_DATA_SIZE[^\n]*', osal, re.M)[0] + '\n'
    for tag in ['_OSAL_OP_DAT', '_OSAL_LXOP_']:
        types += re.search(r'^typedef struct ' + tag + r' \{.*?^\}[^;]*;', osal, re.M | re.S)[0] + '\n'
    for header, tags in [(exp_header, ['_ENUM_WMTDRV_TYPE_T', '_ENUM_WMTTHERM_TYPE_T', '_ENUM_WMTDSNS_TYPE_T']),
                         (core_header, ['_ENUM_WMT_OPID_T'])]:
        for tag in tags:
            types += re.search(r'^typedef enum ' + tag + r' \{.*?^\}[^;]*;', header, re.M | re.S)[0] + '\n'
    types += re.search(r'enum wmt_op_state \{.*?\};', lib, re.S)[0] + '\n'
    owned = 'wmt_lib_alloc_op_data' in lib
    extra = ''.join(function(lib, name) for name in [
        'wmt_lib_put_op_to_free_queue', 'wmt_lib_put_op_ref', 'wmt_lib_queue_op', 'wmt_lib_submit_op_result']) if owned else ''
    production = extra + ''.join(function(lib, name) for name in [
        'wmt_lib_set_current_op', 'wmt_lib_cancel_current_op', 'wmt_lib_complete_op',
        'wmt_lib_put_act_op_result', 'wmt_lib_put_act_op',
    ])
    production += ''.join(function(exp, name) for name in ['mtk_wcn_wmt_therm_ctrl', 'mtk_wcn_wmt_dsns_ctrl'])
    c_file = args.output / 'test.c'
    stubs = STUBS
    if owned:
        stubs = re.sub(r'static int wmt_lib_put_op_to_free_queue\(.*?\n\}', '', stubs, flags=re.S)
    prefix = PREFIX + (op_pool_stubs() if 'wmt_lib_queue_op' in lib else '')
    c_file.write_text(prefix + types + stubs + production + TESTS)
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie', '-pthread',
        str(c_file), '-o', str(args.output / 'test'),
    ]
    compiled = subprocess.run(command, capture_output=True)
    (args.output / 'compile.txt').write_bytes(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    rows = []
    for kind, label in enumerate(KINDS):
        for scenario, case in enumerate(CASES):
            name = label + '-' + case
            run = subprocess.run([str(args.output / 'test'), str(kind), str(scenario)],
                                 capture_output=True, timeout=10,
                                 env=dict(os.environ, ASAN_OPTIONS='halt_on_error=1'))
            (args.output / (name + '.txt')).write_bytes(run.stdout + run.stderr)
            rows.append(dict(case=name, exit_code=run.returncode, passed=run.returncode == 0))
            print(('PASS: ' if run.returncode == 0 else 'FAIL: ') + name, flush=True)
    result = dict(
        passed=sum(row['passed'] for row in rows), total=len(rows), cases=rows,
        source_revision=revision, source_sha256={name: hashlib.sha256(data).hexdigest() for name, data in sources.items()},
        runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        imported_runner_sha256=hashlib.sha256(Path(__file__).with_name('test_wmt_command.py').read_bytes()).hexdigest(),
        fixture_sha256=hashlib.sha256(c_file.read_bytes()).hexdigest(), compiler=command,
    )
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(result['passed'] != result['total'])


if __name__ == '__main__':
    main()
