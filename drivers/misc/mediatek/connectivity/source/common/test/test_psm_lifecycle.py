#!/usr/bin/env python3
"""Exercise production PSM init/deinit, diagnostic readers and monitor shutdown on the host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

CASES = {
    0: 'successful-initial-state', 1: 'successful-teardown-releases-resources',
    2: 'fifo-descriptor-failure-unwind', 3: 'fifo-buffer-failure-unwind',
    4: 'wake-source-failure-unwind', 5: 'first-debug-record-failure-unwind',
    6: 'second-debug-record-failure-unwind', 7: 'kthread-enomem-unwind',
    8: 'kthread-eintr-unwind', 9: 'retry-after-each-failure',
    10: 'optional-history-allocation-failure', 11: 'records-ready-before-worker-start',
    12: 'null-deinit-preserves-live-instance', 13: 'duplicate-init-does-not-replace-thread',
    14: 'worker-reader-completes-before-free', 15: 'timer-reader-completes-before-free',
    16: 'concurrent-monitor-rearm-and-deinit', 17: 'repeated-init-and-deinit',
    18: 'external-flag-writer-before-free', 19: 'external-flag-printer-before-free',
    20: 'external-opid-writer-before-free', 21: 'external-opid-printer-before-free',
}
THREAD_CASES = {100: 'concurrent-singleton-initialization', 101: 'concurrent-singleton-teardown',
                102: 'monitor-rearm-shutdown-order', 103: 'external-flag-writer-before-free',
                104: 'external-flag-printer-before-free', 105: 'external-opid-writer-before-free',
                106: 'external-opid-printer-before-free'}


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
    base = args.kernel/'drivers/misc/mediatek/connectivity/source/common/common_main'
    paths = [base/'core/psm_core.c', base/'core/include/psm_core.h', base/'linux/osal.c', base/'linux/include/osal.h']
    psm, psm_header, osal, osal_header = [path.read_text() for path in paths]
    types = '\n'.join(re.search(r'typedef struct '+name+r' \{.*?\n\} [^;]+;', osal_header, re.S)[0]
                      for name in ['_OSAL_FIFO_', '_OSAL_THREAD_', '_OSAL_WAKE_LOCK_'])
    types += '\n' + re.sub(r'^#include .*\n', '', psm_header, flags=re.M)
    globals_ = '\n'.join(line for line in psm.splitlines() if re.match(
        r'^(?:MTKSTP_PSM_T stp_psm_i;|MTKSTP_PSM_T \*stp_psm =|STP_PSM_RECORD_T \*g_stp_psm_dbg;|'
        r'P_STP_PSM_OPID_RECORD g_stp_psm_opid_dbg;|static STP_PSM_\w+ __rcu \*g_stp_psm_\w+;|'
        r'static UINT32 (?:g_record_num|g_opid_record_num|stp_traffic_start|stp_traffic_current);|'
        r'static DEFINE_(?:MUTEX|SPINLOCK)\(g_psm_\w+\);|static bool g_psm_\w+)', line)) + '\n'
    osal_names = ['osal_malloc', 'osal_free', 'osal_thread_create', 'osal_thread_run', 'osal_thread_destroy',
                  '_osal_fifo_init', '_osal_fifo_deinit', '_osal_fifo_size', '_osal_fifo_avail_size',
                  '_osal_fifo_len', '_osal_fifo_is_empty', '_osal_fifo_is_full', '_osal_fifo_data_in',
                  '_osal_fifo_data_out', '_osal_fifo_reset', 'osal_fifo_deinit', 'osal_fifo_init',
                  'osal_fifo_reset', 'osal_wake_lock_init', 'osal_wake_lock_deinit']
    psm_names = ['_stp_psm_dbg_dmp_in', '_stp_psm_opid_dbg_dmp_in', '_stp_psm_dbg_out_printk',
                 '_stp_psm_opid_dbg_out_printk', '_stp_psm_start_monitor',
                 'stp_psm_start_monitor', '_stp_psm_stp_is_idle', '_stp_psm_init_monitor',
                 '_stp_psm_deinit_monitor', 'stp_psm_set_sleep_enable', '_stp_psm_free_resources',
                 'stp_psm_init', 'stp_psm_deinit']
    osal_code = '\n'.join(function(osal, name) for name in osal_names)
    psm_code = '\n'.join(function(psm, name, required=name != '_stp_psm_free_resources') for name in psm_names)
    host_path = Path(__file__).with_name('psm_lifecycle_host.c')
    fixture = '#define HAS_MONITOR_LOCK '+str(int('g_psm_monitor_lock' in psm))+'\n'
    fixture += '#define HAS_RECORD_RCU '+str(int('rcu_read_lock' in psm))+'\n'
    fixture += (host_path.read_text().replace('/* TYPES */', types).replace('/* GLOBALS */', globals_)
                .replace('/* OSAL */', osal_code).replace('/* PSM */', psm_code))
    c_file = args.output/'test.c'
    c_file.write_text(fixture)
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-pointer-sign',
        '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-but-set-variable',
        '-Wno-sign-compare', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all', '-no-pie', '-pthread',
        '-fsanitize='+('thread' if args.sanitizer == 'thread' else 'address,undefined'),
        str(c_file), '-o', str(args.output/'test')]
    compiled = subprocess.run(command, capture_output=True)
    (args.output/'compile.txt').write_bytes(compiled.stdout+compiled.stderr)
    compiled.check_returncode()
    rows = []
    for number, name in (THREAD_CASES if args.sanitizer == 'thread' else CASES).items():
        run = subprocess.run([str(args.output/'test'), str(number)], capture_output=True, timeout=30,
                             env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', TSAN_OPTIONS='halt_on_error=1'))
        (args.output/(name+'.txt')).write_bytes(run.stdout+run.stderr)
        rows.append({'case':name, 'exit_code':run.returncode, 'pass':run.returncode==0})
        print(('PASS' if run.returncode==0 else 'FAIL')+': '+name, flush=True)
    result = {'source_sha256':{str(path.relative_to(args.kernel)):hashlib.sha256(path.read_bytes()).hexdigest() for path in paths},
              'fixture_sha256':hashlib.sha256(c_file.read_bytes()).hexdigest(),
              'runner_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'host_sha256':hashlib.sha256(host_path.read_bytes()).hexdigest(),
              'cases':rows, 'passed':sum(row['pass'] for row in rows), 'total':len(rows), 'compiler':command,
              'sanitizer':args.sanitizer}
    (args.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    raise SystemExit(0 if result['passed']==result['total'] else 1)


if __name__ == '__main__':
    main()
