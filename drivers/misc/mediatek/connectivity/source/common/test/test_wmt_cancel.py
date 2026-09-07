#!/usr/bin/env python3
"""Exercise WMT reset cancellation against in-flight work and pointer reuse."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

from test_wmt_command import function
from test_wmt_operation import PREFIX, op_pool_stubs

STUBS = r'''
typedef struct {
    int rActiveOpQ, rFreeOpQ, rWorkerOpQ, rWmtdWq, rWmtdWorkerWq, worker_thread, thread;
    P_OSAL_OP pCurOP, pWorkerOP;
} DEVICE, *P_DEV_WMT;
static DEVICE gDevWmt;
static OSAL_OP operation;
static P_OSAL_OP active;
static int scenario, signal_calls, recycled;
static VOID wmt_lib_complete_op(P_OSAL_OP op, INT32 result);
static VOID wmt_lib_cancel_current_op(P_DEV_WMT dev);
static INT32 wmt_lib_set_current_op(P_DEV_WMT dev, P_OSAL_OP op);
#define atomic_set(p, v) (*(p) = (v))
#define atomic_inc(p) (++*(p))
#define atomic_dec(p) (--*(p))
#define atomic_dec_and_test(p) (--*(p) == 0)
static int mtk_wcn_stp_coredump_start_get(void) { return 0; }
static void osal_signal_init(P_OSAL_SIGNAL signal) { (void)signal; }
static void osal_trigger_event(int *event) { (void)event; }
static int osal_op_is_wait_for_signal(P_OSAL_OP op)
{ return op && op->signal.timeoutValue; }
static void osal_op_raise_signal(P_OSAL_OP op, int result)
{ op->result = result; signal_calls++; }
static int wmt_lib_put_op(int *queue, P_OSAL_OP op)
{
    if (queue == &gDevWmt.rActiveOpQ) {
        active = op;
        wmt_lib_set_current_op(&gDevWmt, op);
    } else {
        assert(queue == &gDevWmt.rFreeOpQ && op->ref_count == 0);
        assert(gDevWmt.pCurOP != op);
        recycled++;
    }
    return true;
}
static void finish(int result)
{
    wmt_lib_set_current_op(&gDevWmt, NULL);
    active->op.au4OpData[0] = 64;
    wmt_lib_complete_op(active, result);
}
static int osal_wait_for_signal_timeout(P_OSAL_SIGNAL signal, int *thread)
{
    (void)signal; (void)thread;
    if (scenario == 2) {
        /* Wi-Fi worker can finish while regular dispatch still owns/publishes it. */
        atomic_inc(&active->ref_count);
        active->op.au4OpData[0] = 64;
        wmt_lib_complete_op(active, 0);
        wmt_lib_cancel_current_op(&gDevWmt);
        wmt_lib_set_current_op(&gDevWmt, NULL);
        assert(!atomic_dec_and_test(&active->ref_count));
    } else {
        active->op.au4OpData[0] = 99; /* A partial, nonterminal consumer reply. */
        wmt_lib_cancel_current_op(&gDevWmt);
        if (scenario == 1 || scenario == 4) finish(scenario == 4 ? -3 : 0);
        if (scenario == 3) wmt_lib_cancel_current_op(&gDevWmt);
    }
    return 1;
}
'''

TESTS = r'''
#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "line %d: %s\n", __LINE__, #value); return 1; } } while (0)
int main(int argc, char **argv)
{
    REQUIRE(argc == 2);
    scenario = atoi(argv[1]);
    operation.signal.timeoutValue = 2000;
    operation.op.opId = WMT_OPID_LPBK;
    operation.op.au4OpData[0] = 17;
    if (scenario >= 5) {
        operation.ref_count = 1;
        if (scenario == 5) {
            wmt_lib_cancel_current_op(&gDevWmt);
            REQUIRE(signal_calls == 0);
            REQUIRE(wmt_lib_active_op_id(&gDevWmt, false) == (UINT32)-1);
        } else if (scenario == 6) {
            operation.signal.timeoutValue = 0;
            wmt_lib_set_current_op(&gDevWmt, &operation);
            wmt_lib_cancel_current_op(&gDevWmt);
            REQUIRE(signal_calls == 0);
            REQUIRE(wmt_lib_active_op_id(&gDevWmt, false) == WMT_OPID_LPBK);
            wmt_lib_set_current_op(&gDevWmt, NULL);
            wmt_lib_complete_op(&operation, 0);
            REQUIRE(recycled == 1 && signal_calls == 0);
        } else if (scenario == 7) {
            wmt_lib_set_current_op(&gDevWmt, &operation);
            wmt_lib_set_current_op(&gDevWmt, NULL);
            operation.op.opId = 123;
            operation.result = 47;
            wmt_lib_cancel_current_op(&gDevWmt);
            REQUIRE(operation.result == 47 && signal_calls == 0);
        } else {
            wmt_lib_set_worker_op(&gDevWmt, &operation);
            REQUIRE(wmt_lib_active_op_id(&gDevWmt, true) == WMT_OPID_LPBK);
            wmt_lib_set_worker_op(&gDevWmt, NULL);
            operation.op.opId = 123;
            REQUIRE(wmt_lib_active_op_id(&gDevWmt, true) == (UINT32)-1);
        }
    } else {
        OSAL_OP_DAT result;
        int success = wmt_lib_put_act_op_result(&operation, &result);
        REQUIRE(success == (scenario == 2));
        REQUIRE(result.au4OpData[0] == (scenario == 2 ? 64 : 17));
        REQUIRE(signal_calls == 1);
        if (scenario == 0 || scenario == 3) finish(0);
        REQUIRE(recycled == 1 && signal_calls == 1);
    }
    puts("PASS");
    return 0;
}
'''

CASES = ['reset-wakeup-preserves-submitted-fields', 'late-success-cannot-reverse-cancel',
         'completed-reply-survives-reset', 'repeated-reset-signals-once',
         'late-error-cannot-replace-cancelled-fields', 'no-current-operation',
         'asynchronous-operation-not-cancelled', 'withdrawn-operation-not-signalled',
         'worker-id-snapshot']


def production(lib):
    state = re.search(r'enum wmt_op_state \{.*?\};', lib, re.S)
    result = (state[0] + '\n') if state else ''
    result += function(lib, 'wmt_lib_get_current_op')
    result += function(lib, 'wmt_lib_set_current_op')
    result += function(lib, 'wmt_lib_set_worker_op')
    cancel = function(lib, 'wmt_lib_cancel_current_op', required=False)
    if not cancel:
        # Preserve the actual original reset wakeup fragment as the baseline.
        start = lib.index('\t/* wakeup blocked opid */')
        end = lib.index('\t/* wakeup blocked cmd */', start)
        cancel = ('static VOID wmt_lib_cancel_current_op(P_DEV_WMT pDevWmt) {\n'
                  'P_OSAL_OP pOp;\n' + lib[start:end] + '\n}\n')
    result += cancel
    snapshot = function(lib, 'wmt_lib_active_op_id', required=False)
    if not snapshot:
        result += function(lib, 'wmt_lib_get_worker_op')
        snapshot = ('static UINT32 wmt_lib_active_op_id(P_DEV_WMT dev, bool worker) {\n'
                    'P_OSAL_OP op = worker ? wmt_lib_get_worker_op(dev) : wmt_lib_get_current_op(dev);\n'
                    'return op ? op->op.opId : (UINT32)-1; }\n')
    result += snapshot
    if 'wmt_lib_alloc_op_data' in lib:
        result += ''.join(function(lib, name) for name in [
            'wmt_lib_put_op_to_free_queue', 'wmt_lib_put_op_ref', 'wmt_lib_queue_op', 'wmt_lib_submit_op_result'])
    for name in ['wmt_lib_put_act_op_result', 'wmt_lib_complete_op']:
        result += function(lib, name)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[7])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitizer', choices=['address', 'thread'], default='address')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    base = args.kernel / 'drivers/misc/mediatek/connectivity/source/common/common_main'
    sources = [base / 'core/wmt_lib.c', base / 'linux/include/osal.h']
    lib, osal = [p.read_text() for p in sources]
    data_size = re.search(r'^#define OSAL_OP_DATA_SIZE[^\n]*', osal, re.M)[0] + '\n'
    types = ''.join(re.search(r'^typedef struct ' + tag + r' \{.*?^\}[^;]*;', osal, re.M | re.S)[0] + '\n'
                    for tag in ['_OSAL_OP_DAT', '_OSAL_LXOP_'])
    prefix, stubs, tests, cases = PREFIX, STUBS, TESTS, CASES
    fixture = None
    if args.sanitizer == 'thread':
        fixture = Path(__file__).with_name('wmt_cancel_threads.c')
        prefix = '#include <stdatomic.h>\n' + prefix.replace('typedef int atomic_t;', 'typedef atomic_int atomic_t;')
        stubs, tests = fixture.read_text().split('/* TESTS */')
        cases = ['cancel-during-consumer-writes', 'active-pointer-reuse']
    if 'wmt_lib_queue_op' in lib:
        prefix += op_pool_stubs()
    c_file = args.output / 'test.c'
    c_file.write_text(prefix + data_size + types + stubs + production(lib) + tests)
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-pthread',
        '-fsanitize=' + ('thread' if args.sanitizer == 'thread' else 'address,undefined'),
        '-no-pie', '-fno-omit-frame-pointer', str(c_file), '-o', str(args.output / 'test')]
    subprocess.run(command, check=True)
    rows = []
    for number, name in enumerate(cases):
        run = subprocess.run([str(args.output / 'test'), str(number)], capture_output=True,
                             timeout=60, env=dict(os.environ, TSAN_OPTIONS='halt_on_error=1'))
        (args.output / (name + '.txt')).write_bytes(run.stdout + run.stderr)
        rows.append(dict(case=name, exit_code=run.returncode, passed=run.returncode == 0))
        print(('PASS: ' if run.returncode == 0 else 'FAIL: ') + name, flush=True)
    result = dict(passed=sum(row['passed'] for row in rows), total=len(rows), cases=rows,
                  source_sha256={str(p.relative_to(args.kernel)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  imported_runner_sha256=hashlib.sha256(Path(__file__).with_name('test_wmt_operation.py').read_bytes()).hexdigest(),
                  fixture_sha256=hashlib.sha256(c_file.read_bytes()).hexdigest(), compiler=command,
                  sanitizer=args.sanitizer)
    if fixture:
        result['thread_fixture_sha256'] = hashlib.sha256(fixture.read_bytes()).hexdigest()
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(result['passed'] != result['total'])


if __name__ == '__main__':
    main()
