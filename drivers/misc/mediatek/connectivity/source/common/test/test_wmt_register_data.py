#!/usr/bin/env python3
"""Exercise extracted WMT register/efuse callers and firmware command consumers."""
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void VOID;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef uint32_t *PUINT32;
typedef uint8_t UINT8;
typedef uint8_t *PUINT8;
typedef size_t SIZE_T;
typedef bool MTK_WCN_BOOL;
#define MTK_WCN_BOOL_FALSE false
#define MTK_WCN_BOOL_TRUE true
#define WMT_OPID_REG_RW 5
#define WMT_OPID_EFUSE_RW 16
#define WMTDRV_TYPE_WMT 0
#define DRV_STS_FUNC_ON 1
#define MAX_EACH_WMT_CMD 100
#define WMT_ERR_FUNC(...) ((void)0)
#define WMT_WARN_FUNC(...) ((void)0)
#define WMT_DBG_FUNC(...) ((void)0)
#define WMT_INFO_FUNC(...) ((void)0)
#define osal_memcpy memcpy
#define osal_sizeof sizeof
#define DISABLE_PSM_MONITOR() (scenario == 5)
#define ENABLE_PSM_MONITOR() ((void)0)
typedef struct { unsigned int timeoutValue; } OSAL_SIGNAL, *P_OSAL_SIGNAL;
typedef struct { OSAL_OP_DAT op; OSAL_SIGNAL signal; } OSAL_OP, *P_OSAL_OP;
typedef OSAL_OP_DAT *P_WMT_OP;
static struct { int eDrvStatus[1]; } gMtkWmtCtx = {{1}};
static UINT8 WMT_EFUSE_CMD[12];
static UINT8 WMT_EFUSE_EVT[12];
static OSAL_OP operation;
static int scenario, kind, executed, recycled;
static const UINT32 input = 0x12345678, reply = 0x89abcdef;
static void wmt_core_dump_data(void *data, const char *name, size_t size)
{ (void)data; (void)name; (void)size; }
static INT32 wmt_core_reg_rw_raw(UINT32 write, UINT32 offset, PUINT32 value, UINT32 mask)
{
    assert(offset == 0x80 && mask == 0xff);
    assert(*value == input);
    if (!write) *value = reply;
    executed++;
    return scenario == 4 ? -1 : 0;
}
static INT32 wmt_core_tx(PUINT8 data, UINT32 length, PUINT32 written, MTK_WCN_BOOL raw)
{
    UINT32 value;
    (void)raw;
    assert(length == sizeof(WMT_EFUSE_CMD));
    memcpy(&value, &data[8], 4);
    assert(value == input && data[6] == 0x80 && data[7] == 0);
    assert(data[4] == (scenario == 1 ? 1 : 2));
    *written = length;
    executed++;
    return scenario == 4 ? -1 : 0;
}
static INT32 wmt_core_rx(PUINT8 data, UINT32 length, PUINT32 read)
{ memset(data, 0, length); *read = length; return 0; }
static P_OSAL_OP wmt_lib_get_free_op(void)
{ memset(&operation, 0, sizeof(operation)); return &operation; }
static void wmt_lib_put_op_to_free_queue(P_OSAL_OP op)
{ assert(op == &operation); recycled++; }
static INT32 opfunc_reg_rw(P_WMT_OP op);
static INT32 opfunc_efuse_rw(P_WMT_OP op);
static INT32 consume(void)
{ return kind ? opfunc_efuse_rw(&operation.op) : opfunc_reg_rw(&operation.op); }
static MTK_WCN_BOOL wmt_lib_put_act_op_result(P_OSAL_OP op, P_OSAL_OP_DAT result)
{
    if (result) *result = op->op;
    if (scenario == 2 || scenario == 3) return false;
    int ret = consume();
    if (result) *result = op->op;
    memset(op, 0, sizeof(*op)); /* Force reuse before the caller resumes. */
    recycled++;
    return ret == 0;
}
static MTK_WCN_BOOL wmt_lib_put_act_op(P_OSAL_OP op)
{ return wmt_lib_put_act_op_result(op, NULL); }
'''

TEST = r'''
int main(int argc, char **argv)
{
    assert(argc == 3);
    kind = atoi(argv[1]); scenario = atoi(argv[2]);
    UINT32 value = input;
    int ret = kind ? wmt_lib_efuse_rw(scenario == 1, 0x80, scenario == 6 ? NULL : &value, 0xff) :
                     wmt_lib_reg_rw(scenario == 1, 0x80, scenario == 6 ? NULL : &value, 0xff);
    if (scenario == 2 || scenario == 3) {
        assert(ret == -1 && value == input && !recycled);
        /* The actual consumer first touches its value after the caller returned. */
        assert(consume() == 0);
        assert(value == input);
    } else if (scenario >= 4) {
        assert(ret == -1 && value == input);
    } else {
        assert(ret == 0 && recycled == 1);
        assert(value == ((!kind && scenario == 0) ? reply : input));
    }
    assert(executed == (scenario >= 5 ? 0 : 1));
    puts("PASS");
    return 0;
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    base = args.kernel / 'drivers/misc/mediatek/connectivity/source/common/common_main'
    paths = [base / p for p in ['core/wmt_lib.c', 'core/wmt_core.c', 'linux/include/osal.h']]
    lib, core, osal = [p.read_text() for p in paths]
    types = re.search(r'^#define OSAL_OP_DATA_SIZE[^\n]*', osal, re.M)[0] + '\n'
    types += re.search(r'^typedef struct _OSAL_OP_DAT \{.*?^\}[^;]*;', osal, re.M | re.S)[0] + '\n'
    prefix = PREFIX.replace('typedef struct { unsigned int timeoutValue;', types + 'typedef struct { unsigned int timeoutValue;', 1)
    production = ''.join(function(core, n) for n in ['opfunc_reg_rw', 'opfunc_efuse_rw'])
    production += ''.join(function(lib[lib.rindex('INT32 ' + n + '('):], n) for n in ['wmt_lib_reg_rw', 'wmt_lib_efuse_rw'])
    cfile = args.output / 'test.c'
    cfile.write_text(prefix + production + TEST)
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie', str(cfile), '-o', str(args.output / 'test')]
    subprocess.run(command, check=True)
    rows = []
    names = ['read', 'write', 'queued-timeout', 'cancelled-late-consumer', 'consumer-error', 'wake-failure', 'null-value']
    for kind in range(2):
        for scenario, label in enumerate(names):
            name = ('efuse-' if kind else 'register-') + label
            run = subprocess.run([str(args.output / 'test'), str(kind), str(scenario)], capture_output=True, timeout=10,
                env=dict(os.environ, ASAN_OPTIONS='detect_stack_use_after_return=1:halt_on_error=1'))
            (args.output / (name + '.txt')).write_bytes(run.stdout + run.stderr)
            rows.append(dict(case=name, exit_code=run.returncode, passed=run.returncode == 0))
            print(('PASS: ' if run.returncode == 0 else 'FAIL: ') + name)
    result = dict(passed=sum(r['passed'] for r in rows), total=len(rows), cases=rows, compiler=command,
        source_sha256={str(p.relative_to(args.kernel)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
        runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), fixture_sha256=hashlib.sha256(cfile.read_bytes()).hexdigest())
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(result['passed'] != result['total'])

if __name__ == '__main__':
    main()
