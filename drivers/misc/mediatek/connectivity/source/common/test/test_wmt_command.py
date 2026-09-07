#!/usr/bin/env python3
"""Run extracted WMT command producer/read/response functions on the host.

The command functions are production code. The fixture substitutes kernel user
copies, completion scheduling, bit operations, mutexes and logging. It does not
claim hardware behavior or solve the legacy protocol's untagged late replies.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


PREFIX = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

typedef void VOID;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef uint8_t UINT8;
typedef uint8_t *PUINT8;
typedef bool MTK_WCN_BOOL;
#define MTK_WCN_BOOL_TRUE true
#define MTK_WCN_BOOL_FALSE false
#define __user
#define WMT_STAT_CMD 6
#define WMT_DBG_FUNC(...) ((void)0)
#define WMT_WARN_FUNC(...) ((void)0)
#define WMT_ERR_FUNC(...) ((void)0)
#define WMT_LOUD_FUNC(...) ((void)0)
#define osal_strlen(s) strlen((const char *)(s))
#define osal_strncpy(d, s, n) strncpy((char *)(d), (const char *)(s), (n))
#define osal_memcpy memcpy
#define mutex_lock(m) pthread_mutex_lock(m)
#define mutex_unlock(m) pthread_mutex_unlock(m)
#define DEFINE_MUTEX(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
struct file { int unused; };
typedef struct { atomic_int done; unsigned int timeoutValue; } OSAL_SIGNAL, *P_OSAL_SIGNAL;
typedef struct { int unused; } OSAL_EVENT, *P_OSAL_EVENT;
typedef struct {
    unsigned long state;
    UINT8 cCmd[NAME_MAX + 1];
    INT32 cmdResult;
    OSAL_SIGNAL cmdResp;
    OSAL_EVENT cmdReq;
} DEV_WMT, *P_DEV_WMT;
static DEV_WMT gDevWmt;
static atomic_int fail_copy, read_copies, event_calls;
static int scenario;
static pthread_mutex_t signal_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t signal_cv = PTHREAD_COND_INITIALIZER;
static int early_ready, early_read;
static char received[NAME_MAX + 1];
static const UINT8 command[] = "srh_patch";
static void on_copy(void);

static int osal_test_bit(unsigned int bit, const unsigned long *state)
{ return (__atomic_load_n(state, __ATOMIC_SEQ_CST) >> bit) & 1; }
static int osal_test_and_set_bit(unsigned int bit, unsigned long *state)
{ return (__atomic_fetch_or(state, 1UL << bit, __ATOMIC_SEQ_CST) >> bit) & 1; }
static int osal_test_and_clear_bit(unsigned int bit, unsigned long *state)
{ return (__atomic_fetch_and(state, ~(1UL << bit), __ATOMIC_SEQ_CST) >> bit) & 1; }
static void osal_set_bit(unsigned int bit, unsigned long *state)
{ (void)osal_test_and_set_bit(bit, state); }
static void osal_clear_bit(unsigned int bit, unsigned long *state)
{ (void)osal_test_and_clear_bit(bit, state); }
static unsigned long copy_to_user(void *dest, const void *source, size_t count)
{
    read_copies++;
    if (fail_copy) return count;
    on_copy();
    memcpy(dest, source, count);
    return 0;
}
static unsigned long copy_from_user(void *dest, const void *source, size_t count)
{
    if (fail_copy) return count;
    memcpy(dest, source, count);
    return 0;
}
ssize_t WMT_read(struct file *, char *, size_t, loff_t *);
ssize_t WMT_write(struct file *, const char *, size_t, loff_t *);
INT32 wmt_ctrl_ul_cmd(P_DEV_WMT, const PUINT8);
static void on_event(void);
static void on_wait(void);
static int osal_signal_init(P_OSAL_SIGNAL signal)
{
    if (scenario == 6 && osal_test_bit(WMT_STAT_CMD, &gDevWmt.state)) {
        early_ready++;
        early_read = WMT_read(NULL, received, sizeof(received), NULL);
        WMT_write(NULL, "ok", 2, NULL);
    }
    signal->done = 0;
    return 0;
}
static void osal_raise_signal(P_OSAL_SIGNAL signal)
{
    pthread_mutex_lock(&signal_lock);
    signal->done = 1;
    pthread_cond_broadcast(&signal_cv);
    pthread_mutex_unlock(&signal_lock);
}
static int osal_trigger_event(P_OSAL_EVENT event)
{ (void)event; event_calls++; on_event(); return 0; }
static int osal_wait_for_signal_timeout(P_OSAL_SIGNAL signal, void *thread)
{
    (void)thread;
    if (scenario >= 100) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 3;
        pthread_mutex_lock(&signal_lock);
        while (!signal->done) {
            if (pthread_cond_timedwait(&signal_cv, &signal_lock, &deadline) == ETIMEDOUT)
                break;
        }
        int done = signal->done;
        pthread_mutex_unlock(&signal_lock);
        return done;
    }
    on_wait();
    return signal->done;
}
'''

TESTS = r'''
static int inner_result, first_read, second_read, response_result, duplicate_result;
#define REQUIRE(value) do { if (!(value)) { \
    fprintf(stderr, "line %d: %s (first=%d second=%d response=%d inner=%d)\n", \
        __LINE__, #value, first_read, second_read, response_result, inner_result); \
    return 1; } } while (0)

static void on_copy(void) {}

static void on_event(void)
{
    if (scenario == 7 || scenario == 10 || scenario == 11) return;
    if (scenario == 1) first_read = WMT_read(NULL, received, 0, NULL);
    if (scenario == 2) {
        fail_copy = 1;
        first_read = WMT_read(NULL, received, sizeof(received), NULL);
        fail_copy = 0;
    }
    if (scenario == 3) first_read = WMT_read(NULL, received, 1, NULL);
    if (scenario == 4) response_result = WMT_write(NULL, "ok", 2, NULL);
    second_read = WMT_read(NULL, received, sizeof(received), NULL);
    if (scenario == 5) {
        int saved = scenario;
        scenario = 10;
        inner_result = wmt_ctrl_ul_cmd(&gDevWmt, (PUINT8)"second");
        scenario = saved;
    }
    if (scenario == 8) return;
    if (scenario == 12) {
        response_result = WMT_write(NULL, "bad", 3, NULL);
        return;
    }
    if (scenario == 13) {
        fail_copy = 1;
        first_read = WMT_write(NULL, "ok", 2, NULL);
        fail_copy = 0;
    }
    if (scenario == 14) first_read = WMT_write(NULL, "ok", 0, NULL);
    if (scenario != 4) response_result = WMT_write(NULL, "ok", 2, NULL);
    else WMT_write(NULL, "ok", 2, NULL);
    if (scenario == 9) duplicate_result = WMT_write(NULL, "bad", 3, NULL);
}

static void on_wait(void)
{
    if (scenario == 11) {
        wmt_lib_cancel_cmd();
    }
}

int main(int argc, char **argv)
{
    REQUIRE(argc == 2);
    scenario = atoi(argv[1]);
    strcpy((char *)gDevWmt.cCmd, "stale");
    int selected = scenario;
    UINT8 boundary[NAME_MAX + 2];
    memset(boundary, 'x', sizeof(boundary));
    if (scenario == 15) boundary[NAME_MAX] = 0;
    PUINT8 input = (PUINT8)command;
    if (scenario == 15 || scenario == 16) input = boundary;
    if (scenario == 17) input = NULL;
    if (scenario == 18) input = (PUINT8)"";
    int result = wmt_ctrl_ul_cmd(&gDevWmt, input);
    switch (selected) {
    case 0:
        REQUIRE(result == 0 && second_read == 9 && response_result == 2);
        REQUIRE(strcmp(received, "srh_patch") == 0);
        REQUIRE(WMT_read(NULL, received, sizeof(received), NULL) == 0);
        break;
    case 1:
        REQUIRE(first_read == 0 && second_read == 9 && result == 0);
        REQUIRE(read_copies == 1);
        break;
    case 2:
        REQUIRE(first_read == -EFAULT && second_read == 9 && result == 0);
        break;
    case 3:
        REQUIRE(first_read == -EMSGSIZE && second_read == 9 && result == 0);
        break;
    case 4:
        REQUIRE(response_result < 0 && second_read == 9 && result == 0);
        break;
    case 5:
        REQUIRE(inner_result < 0 && event_calls == 1 && result == 0);
        break;
    case 6:
        REQUIRE(early_ready == 0 && early_read == 0 && result == 0);
        break;
    case 7:
        REQUIRE(result < 0);
        REQUIRE(!osal_test_bit(WMT_STAT_CMD, &gDevWmt.state));
        scenario = 0;
        REQUIRE(wmt_ctrl_ul_cmd(&gDevWmt, (PUINT8)command) == 0);
        break;
    case 8:
        REQUIRE(result < 0 && second_read == 9);
        REQUIRE(WMT_write(NULL, "ok", 2, NULL) < 0);
        break;
    case 9:
        REQUIRE(result == 0 && duplicate_result < 0);
        break;
    case 11:
        REQUIRE(result == -ECANCELED);
        REQUIRE(!osal_test_bit(WMT_STAT_CMD, &gDevWmt.state));
        scenario = 0;
        REQUIRE(wmt_ctrl_ul_cmd(&gDevWmt, (PUINT8)command) == 0);
        break;
    case 12:
        REQUIRE(result == -1 && response_result == 3);
        break;
    case 13:
        REQUIRE(first_read == -EFAULT && response_result == 2 && result == 0);
        break;
    case 14:
        REQUIRE(first_read == 0 && response_result == 2 && result == 0);
        break;
    case 15:
        REQUIRE(second_read == NAME_MAX && result == 0);
        REQUIRE(memcmp(received, boundary, NAME_MAX) == 0);
        break;
    case 16: case 17: case 18:
        REQUIRE(result == -EINVAL && event_calls == 0);
        break;
    default: return 2;
    }
    puts("PASS");
    return 0;
}
'''

CASES = {
    0: 'complete-command', 1: 'zero-read-preserves-command',
    2: 'copy-fault-preserves-command', 3: 'short-read-preserves-command',
    4: 'response-before-read-rejected', 5: 'producer-busy-until-response',
    6: 'publish-after-initialization', 7: 'unread-timeout-releases-slot',
    8: 'response-after-timeout-rejected', 9: 'duplicate-response-rejected',
    11: 'reset-cancels-waiter',
    12: 'negative-response-propagates', 13: 'response-copy-fault-retry',
    14: 'zero-response-does-not-complete', 15: 'maximum-command-length',
    16: 'unterminated-command-rejected', 17: 'null-command-rejected',
    18: 'empty-command-rejected',
}

THREAD_CASES = {
    100: 'competing-producer', 101: 'two-readers-2000-commands',
    102: 'reset-during-user-copy',
}


def function(source, name, required=True):
    pattern = r'(?m)^[A-Za-z_][^;\n]*\b' + re.escape(name) + r'\([^;{}]*\)\n\{'
    match = re.search(pattern, source)
    if not match:
        if required:
            raise ValueError(f'Missing production function: {name}')
        return ''
    end = source.index('\n}', match.end()) + 2
    return source[match.start():end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[7])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitizer', choices=['address', 'thread'], default='address')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.kernel / 'drivers/misc/mediatek/connectivity/source/common/common_main'
    paths = {name: root / path for name, path in {
        'lib': 'core/wmt_lib.c', 'ctrl': 'core/wmt_ctrl.c', 'dev': 'linux/wmt_dev.c',
    }.items()}
    sources = {name: path.read_text() for name, path in paths.items()}
    lib = sources['lib']
    helpers = '\n'.join(re.findall(
        r'^static (?:DEFINE_MUTEX\(g_wmt_cmd_lock\)|bool g_wmt_cmd_\w+);$', lib, re.M)) + '\n'
    helpers += function(lib, 'wmt_lib_trigger_cmd_signal')
    cancel = function(lib, 'wmt_lib_cancel_cmd', required=False)
    if not cancel:
        reset = function(lib, 'wmt_lib_hw_rst')
        clear = re.search(r'^\tosal_clear_bit\(WMT_STAT_CMD, &pDevWmt->state\);$', reset, re.M)
        if not clear:
            raise ValueError('Missing original hardware-reset command cleanup')
        cancel = ('VOID wmt_lib_cancel_cmd(VOID) {\n'
                  'P_DEV_WMT pDevWmt = &gDevWmt;\n' + clear[0] + '\n}\n')
    helpers += cancel
    helpers += function(lib, 'wmt_lib_send_cmd', required=False)
    helpers += function(lib, 'wmt_lib_get_cmd', required=False)
    helpers += function(lib, 'wmt_lib_read_cmd', required=False)
    helpers += function(lib, 'wmt_lib_get_cmd_status')
    helpers += function(sources['ctrl'], 'wmt_ctrl_ul_cmd')
    helpers += function(sources['dev'], 'WMT_write')
    helpers += function(sources['dev'], 'WMT_read')
    tests = (Path(__file__).with_name('wmt_command_threads.c').read_text()
             if args.sanitizer == 'thread' else TESTS)
    fixture = PREFIX + helpers + tests
    c_file = args.output / 'test.c'
    c_file.write_text(fixture)
    binary = args.output / 'test'
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-pointer-sign',
        '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-but-set-variable',
        '-fno-omit-frame-pointer', '-no-pie', '-pthread',
        '-fsanitize=' + ('thread' if args.sanitizer == 'thread' else 'address,undefined'),
        str(c_file), '-o', str(binary),
    ]
    subprocess.run(command, check=True)
    rows = []
    cases = THREAD_CASES if args.sanitizer == 'thread' else CASES
    for number, name in cases.items():
        run = subprocess.run([str(binary), str(number)], capture_output=True, timeout=30,
                             env=dict(os.environ, TSAN_OPTIONS='halt_on_error=1'))
        (args.output / (name + '.txt')).write_bytes(run.stdout + run.stderr)
        rows.append({'case': name, 'exit_code': run.returncode, 'pass': run.returncode == 0})
        print(('PASS' if run.returncode == 0 else 'FAIL') + ': ' + name, flush=True)
    result = {
        'source_sha256': {str(path.relative_to(args.kernel)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in paths.values()},
        'fixture_sha256': hashlib.sha256(c_file.read_bytes()).hexdigest(),
        'cases': rows, 'passed': sum(row['pass'] for row in rows), 'total': len(rows),
        'compiler': command,
        'sanitizer': args.sanitizer,
        'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(0 if result['passed'] == result['total'] else 1)


if __name__ == '__main__':
    main()
