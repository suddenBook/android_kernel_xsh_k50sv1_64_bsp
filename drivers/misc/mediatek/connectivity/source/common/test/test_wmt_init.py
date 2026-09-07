#!/usr/bin/env python3
"""Exercise production HIF decoding and initialization ioctl error paths."""

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
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void VOID;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef uint8_t UINT8;
typedef uint8_t *PUINT8;
typedef unsigned long ULONG;
typedef long LONG;
typedef void *PVOID;
typedef bool MTK_WCN_BOOL;
#define MTK_WCN_BOOL_FALSE false
#define MTK_WCN_BOOL_TRUE true
#define WMT_DBG_FUNC(...) ((void)0)
#define WMT_ERR_FUNC(...) ((void)0)
#define WMT_WARN_FUNC(...) ((void)0)
#define WMT_INFO_FUNC(...) ((void)0)
#define osal_memcpy memcpy
#define osal_strncpy(d, s, n) strncpy((char *)(d), (const char *)(s), n)
#define GFP_KERNEL 0
#define WMT_IOCTL_SET_PATCH_NAME 4
#define WMT_IOCTL_SET_STP_MODE 5
#define WMT_OPID_HIF_CONF 0
#define WMT_OP_HIF_BIT 1
#define DEFINE_MUTEX(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define mutex_lock(m) pthread_mutex_lock(m)
#define mutex_unlock(m) pthread_mutex_unlock(m)
#define READ_ONCE(value) __atomic_load_n(&(value), __ATOMIC_RELAXED)
#define WRITE_ONCE(value, input) __atomic_store_n(&(value), (input), __ATOMIC_RELAXED)
#define FB_EVENT_BLANK 0x09
#define FB_BLANK_UNBLANK 0
#define FB_BLANK_POWERDOWN 4
'''

STUBS = r'''
typedef struct { unsigned int timeoutValue; } OSAL_SIGNAL, *P_OSAL_SIGNAL;
typedef struct {
    struct { unsigned int opId, u4InfoBit; unsigned long au4OpData[16]; } op;
    OSAL_SIGNAL signal;
} OSAL_OP, *P_OSAL_OP;
static struct { WMT_HIF_CONF rWmtHifConf; UINT8 cPatchName[NAME_MAX + 1]; } gDevWmt;
static OSAL_OP operation;
static WMT_HIF_CONF queued_hif;
static int fail_alloc, fail_copy, no_operation, queue_failure;
static int allocations, pool_owned;
static atomic_int queued, release_calls, tx_calls, plat_calls;
static atomic_int tx_type = -1, plat_type = -1, scheduled;
static atomic_int g_late_pwr_on_for_blank, g_es_lr_flag_for_blank;
static atomic_int g_es_lr_flag_for_quick_sleep, g_es_lr_flag_for_lpbk_onoff;
static int gPwrOnOffWork;
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
struct notifier_block { int unused; };
struct fb_event { void *data; };
#define atomic_read(value) atomic_load(value)
#define atomic_set(value, input) atomic_store(value, input)
static void mtk_wcn_stp_set_if_tx_type(int type) { tx_type = type; tx_calls++; }
static void wmt_plat_set_comm_if_type(int type) { plat_type = type; plat_calls++; }
static void schedule_work(int *work) { (void)work; scheduled++; }
static void *kmalloc(size_t size, int flags)
{
    (void)flags;
    if (fail_alloc) return NULL;
    void *memory = malloc(size);
    if (memory) allocations++;
    return memory;
}
static void kfree(void *memory) { if (memory) allocations--; free(memory); }
static unsigned long copy_from_user(void *dest, const void *source, size_t count)
{
    if (fail_copy) return count;
    memcpy(dest, source, count);
    return 0;
}
static P_OSAL_OP wmt_lib_get_free_op(void)
{
    pthread_mutex_lock(&pool_lock);
    if (no_operation || pool_owned) { pthread_mutex_unlock(&pool_lock); return NULL; }
    pool_owned++;
    memset(&operation, 0, sizeof(operation));
    pthread_mutex_unlock(&pool_lock);
    return &operation;
}
static int wmt_lib_put_op_to_free_queue(P_OSAL_OP op)
{
    pthread_mutex_lock(&pool_lock);
    if (op != &operation || pool_owned != 1) abort();
    release_calls++;
    pool_owned--;
    pthread_mutex_unlock(&pool_lock);
    return 0;
}
static MTK_WCN_BOOL wmt_lib_put_act_op(P_OSAL_OP op)
{
    pthread_mutex_lock(&pool_lock);
    if (op != &operation || pool_owned != 1) abort();
    pool_owned--;
    if (queue_failure) { pthread_mutex_unlock(&pool_lock); return false; }
    memcpy(&queued_hif, op->op.au4OpData, sizeof(queued_hif));
    queued++;
    pthread_mutex_unlock(&pool_lock);
    return true;
}
'''

TESTS = r'''
#define REQUIRE(value) do { if (!(value)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #value); return 1; \
} } while (0)
int main(int argc, char **argv)
{
    REQUIRE(argc == 2);
    int test = atoi(argv[1]);
    memset(&gDevWmt.rWmtHifConf, 0x5a, sizeof(WMT_HIF_CONF));
    WMT_HIF_CONF before = gDevWmt.rWmtHifConf;
    char name[NAME_MAX + 1] = "patch.bin";
    int ret;
    if (test < 3) {
        unsigned int modes[] = {STP_UART_FULL, STP_SDIO, STP_BTIF_FULL};
        ret = apply_hif_argument(modes[test]); /* FM mode zero is invalid. */
        REQUIRE(ret != 0);
        REQUIRE(memcmp(&before, &gDevWmt.rWmtHifConf, sizeof(before)) == 0);
        REQUIRE(tx_calls == 0 && plat_calls == 0);
    } else if (test == 3) {
        REQUIRE(apply_hif_argument(0x22) != 0);
        REQUIRE(memcmp(&before, &gDevWmt.rWmtHifConf, sizeof(before)) == 0);
        REQUIRE(tx_calls == 0 && plat_calls == 0);
    } else if (test >= 4 && test <= 9) {
        unsigned int modes[] = {STP_UART_FULL, STP_SDIO, STP_BTIF_FULL};
        unsigned int types[] = {WMT_HIF_UART, WMT_HIF_SDIO, WMT_HIF_BTIF};
        int transport[] = {STP_UART_IF_TX, STP_SDIO_IF_TX, STP_BTIF_IF_TX};
        int index = (test - 4) / 2;
        unsigned int fm = (test & 1) ? WMT_FM_I2C : WMT_FM_COMM;
        ULONG value = modes[index] | (fm << 4) | (115200UL << 8);
        REQUIRE(apply_hif_argument(value) == 0);
        REQUIRE(gDevWmt.rWmtHifConf.hifType == types[index]);
        REQUIRE(gDevWmt.rWmtHifConf.au4StrapConf[0] == fm);
        REQUIRE(tx_type == transport[index] && tx_calls == 1);
        REQUIRE(plat_calls == (index == 2 ? 0 : 1));
        if (index == 0) {
            REQUIRE(gDevWmt.rWmtHifConf.uartFcCtrl == 0);
            REQUIRE(gDevWmt.rWmtHifConf.au4HifConf[0] == 115200);
            REQUIRE(gDevWmt.rWmtHifConf.au4HifConf[1] == 115200);
        }
    } else if (test == 10) {
        no_operation = 1;
        REQUIRE(run_ioctl(WMT_IOCTL_SET_STP_MODE, 0x23) == -ENOMEM);
        REQUIRE(memcmp(&before, &gDevWmt.rWmtHifConf, sizeof(before)) == 0);
        REQUIRE(tx_calls == 0 && hif_info == 0 && queued == 0);
    } else if (test == 11) {
        REQUIRE(run_ioctl(WMT_IOCTL_SET_STP_MODE, 0x03) != 0);
        REQUIRE(pool_owned == 0 && queued == 0 && hif_info == 0);
        REQUIRE(tx_calls == 0);
    } else if (test == 12) {
        queue_failure = 1;
        REQUIRE(run_ioctl(WMT_IOCTL_SET_STP_MODE, 0x23) < 0);
        REQUIRE(pool_owned == 0 && hif_info == 0 && queued == 0 && scheduled == 0);
        queue_failure = 0;
        REQUIRE(run_ioctl(WMT_IOCTL_SET_STP_MODE, 0x23) == 0);
        REQUIRE(hif_info == 1 && queued == 1);
    } else if (test == 13) {
        REQUIRE(run_ioctl(WMT_IOCTL_SET_STP_MODE, 0x23) == 0);
        REQUIRE(hif_info == 1 && queued == 1 && pool_owned == 0);
        REQUIRE(queued_hif.hifType == WMT_HIF_BTIF && queued_hif.au4StrapConf[0] == WMT_FM_COMM);
        int changes = tx_calls;
        REQUIRE(run_ioctl(WMT_IOCTL_SET_STP_MODE, 0x14) == 0);
        REQUIRE(queued == 1 && tx_calls == changes);
    } else if (test == 14) {
        g_late_pwr_on_for_blank = 1; g_es_lr_flag_for_blank = 1;
        REQUIRE(run_ioctl(WMT_IOCTL_SET_STP_MODE, 0x23) == 0);
        REQUIRE(scheduled == 1 && g_late_pwr_on_for_blank == 0);
    } else if (test == 15) {
        fail_alloc = 1;
        REQUIRE(run_ioctl(WMT_IOCTL_SET_PATCH_NAME, (ULONG)name) == -ENOMEM);
        REQUIRE(gDevWmt.cPatchName[0] == 0 && allocations == 0);
    } else if (test == 16) {
        fail_copy = 1;
        REQUIRE(run_ioctl(WMT_IOCTL_SET_PATCH_NAME, (ULONG)name) == -EFAULT);
        REQUIRE(gDevWmt.cPatchName[0] == 0 && allocations == 0);
    } else if (test == 17) {
        REQUIRE(run_ioctl(WMT_IOCTL_SET_PATCH_NAME, (ULONG)name) == 0);
        REQUIRE(strcmp((char *)gDevWmt.cPatchName, "patch.bin") == 0 && allocations == 0);
    } else return 2;
    puts("PASS");
    return 0;
}
'''

CASES = ['invalid-fm-uart', 'invalid-fm-sdio', 'invalid-fm-btif', 'invalid-transport',
         'uart-comm', 'uart-i2c', 'sdio-comm', 'sdio-i2c', 'btif-comm', 'btif-i2c',
         'operation-pool-exhausted', 'invalid-mode-preserves-state-and-pool',
         'queue-failure-retry', 'duplicate-configuration', 'pending-unblank',
         'patch-name-allocation-failure', 'patch-name-copy-failure', 'patch-name-success']


def capture(source, pattern):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise ValueError(f'Missing production declaration: {pattern}')
    return match[0] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[7])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitizer', choices=['address', 'thread'], default='address')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.kernel / 'drivers/misc/mediatek/connectivity/source/common/common_main'
    paths = [root / name for name in ['core/wmt_lib.c', 'linux/wmt_dev.c',
             'core/include/wmt_core.h', 'linux/include/wmt_dev.h', 'include/stp_exp.h']]
    lib, dev, core_h, dev_h, stp_h = [p.read_text() for p in paths]
    definitions = ''.join(capture(core_h, r'^#define ' + name + r'[^\n]*')
                          for name in ['DWCNT_HIF_CONF', 'DWCNT_STRAP_CONF'])
    for name in ['FM', 'HIF']:
        definitions += capture(core_h, r'^typedef enum _ENUM_WMT_' + name + r'_T \{.*?^\}[^;]*;')
    definitions += capture(core_h, r'^typedef struct _WMT_HIF_CONF \{.*?^\}[^;]*;')
    for name in ['STP_UART_FULL', 'STP_BTIF_FULL', 'STP_SDIO']:
        definitions += capture(dev_h, r'^#define ' + name + r'[^\n]*')
    for name in ['STP_UART_IF_TX', 'STP_SDIO_IF_TX', 'STP_BTIF_IF_TX']:
        value = re.search(r'\b' + name + r' = (\d+)', stp_h)[1]
        definitions += '#define ' + name + ' ' + value + '\n'
    globals_ = '\n'.join(re.findall(r'^(?:static )?(?:UINT32 hif_info|DEFINE_MUTEX\(g_hif_lock\));$', dev, re.M)) + '\n'
    parsed = function(lib, 'wmt_lib_parse_hif', required=False)
    helpers = parsed + function(lib, 'wmt_lib_set_hif')
    helpers += function(lib, 'wmt_lib_get_hif', required=False)
    helpers += function(lib, 'wmt_lib_set_patch_name')
    if parsed:
        helpers += ('static INT32 apply_hif_argument(ULONG value) {\n'
                    'WMT_HIF_CONF config; INT32 ret = wmt_lib_parse_hif(value, &config);\n'
                    'if (!ret) { wmt_lib_set_hif(&config); }\nreturn ret; }\n')
    else:
        helpers += 'static INT32 apply_hif_argument(ULONG value) { return wmt_lib_set_hif(value); }\n'
    helpers += function(dev, 'wmt_dev_set_hif', required=False)
    helpers += function(dev, 'wmt_fb_notifier_callback')
    cases = ''
    for name in ['SET_PATCH_NAME', 'SET_STP_MODE']:
        start = dev.index('\tcase WMT_IOCTL_' + name + ':')
        end = dev.index('\tcase ', start + 1)
        cases += dev[start:end]
    ioctl = ('static LONG run_ioctl(UINT32 cmd, ULONG arg) { INT32 iRet = 0; UINT8 *pBuffer = NULL;\n'
             'switch (cmd) {\n' + cases + '\n} return iRet; }\n')
    c_file = args.output / 'test.c'
    tests = (Path(__file__).with_name('wmt_init_threads.c').read_text()
             if args.sanitizer == 'thread' else TESTS)
    c_file.write_text(PREFIX + definitions + STUBS + globals_ + helpers + ioctl + tests)
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-function',
        '-Wno-pointer-sign', '-fno-omit-frame-pointer', '-no-pie', '-pthread',
        '-fsanitize=' + ('thread' if args.sanitizer == 'thread' else 'address,undefined'),
        '-Wno-unused-parameter', str(c_file), '-o', str(args.output / 'test'),
    ]
    subprocess.run(command, check=True)
    rows = []
    cases = ['concurrent-init-and-unblank'] if args.sanitizer == 'thread' else CASES
    for number, name in enumerate(cases):
        run = subprocess.run([str(args.output / 'test'), str(number)], capture_output=True, timeout=30,
                             env=dict(os.environ, TSAN_OPTIONS='halt_on_error=1'))
        (args.output / (name + '.txt')).write_bytes(run.stdout + run.stderr)
        rows.append({'case': name, 'exit_code': run.returncode, 'pass': run.returncode == 0})
        print(('PASS' if run.returncode == 0 else 'FAIL') + ': ' + name, flush=True)
    result = {'passed': sum(r['pass'] for r in rows), 'total': len(rows), 'cases': rows,
              'source_sha256': {str(p.relative_to(args.kernel)): hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in paths},
              'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'fixture_sha256': hashlib.sha256(c_file.read_bytes()).hexdigest(), 'compiler': command,
              'sanitizer': args.sanitizer}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(result['passed'] != result['total'])


if __name__ == '__main__':
    main()
