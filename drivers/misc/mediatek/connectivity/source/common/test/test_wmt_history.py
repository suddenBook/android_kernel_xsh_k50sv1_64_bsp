#!/usr/bin/env python3
"""Exercise actual OSAL history/ring functions with host allocation and workqueue substitutes."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


CASES = {
    0: 'snapshot-preserves-owned-records',
    1: 'no-sleeping-allocation-under-spinlock',
    2: 'queued-dump-teardown',
    3: 'running-dump-teardown',
    4: 'allocation-failure-cleanup',
    5: 'invalid-size-rejected',
    6: 'empty-history',
    7: 'repeated-init-deinit',
}
THREAD_CASES = {100: 'concurrent-save-print-and-worker', 101: 'concurrent-teardown'}


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
    base = args.kernel / 'drivers/misc/mediatek/connectivity/source/common'
    paths = [base / name for name in ['common_main/linux/osal.c',
             'common_main/linux/include/osal.h', 'debug_utility/ring.h', 'debug_utility/ring.c']]
    osal, header, ring_header, ring_source = [path.read_text() for path in paths]
    structs = '\n'.join(re.search(r'struct ' + name + r' \{.*?\n\};', header, re.S)[0]
                        for name in ['osal_op_history_entry', 'osal_op_history'])
    production = ring_header + re.sub(r'^#include .*\n', '', ring_source, flags=re.M) + structs + '\n'
    for name in ['osal_op_history_print_work', 'osal_op_history_init',
                 'osal_op_history_deinit', 'osal_op_history_print', 'osal_op_history_save']:
        body = function(osal, name, required=name != 'osal_op_history_deinit')
        if not body:
            # The baseline has no teardown API or equivalent owner cleanup.
            body = 'VOID osal_op_history_deinit(struct osal_op_history *history) { (void)history; }\n'
        production += body
    host_path = Path(__file__).with_name('wmt_history_host.c')
    fixture = host_path.read_text().replace('/* PRODUCTION */', production)
    c_file = args.output / 'test.c'
    c_file.write_text(fixture)
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-pointer-sign',
        '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-but-set-variable',
        '-fno-omit-frame-pointer', '-no-pie', '-pthread',
        '-fsanitize=' + ('thread' if args.sanitizer == 'thread' else 'address,undefined'),
        str(c_file), '-o', str(args.output / 'test')]
    compiled = subprocess.run(command, capture_output=True)
    (args.output / 'compile.txt').write_bytes(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    rows = []
    for number, name in (THREAD_CASES if args.sanitizer == 'thread' else CASES).items():
        run = subprocess.run([str(args.output / 'test'), str(number)], capture_output=True,
                             timeout=30, env=dict(os.environ, TSAN_OPTIONS='halt_on_error=1',
                             ASAN_OPTIONS='detect_leaks=0'))
        (args.output / (name + '.txt')).write_bytes(run.stdout + run.stderr)
        rows.append({'case': name, 'exit_code': run.returncode, 'pass': run.returncode == 0})
        print(('PASS' if run.returncode == 0 else 'FAIL') + ': ' + name, flush=True)
    result = {
        'source_sha256': {str(path.relative_to(args.kernel)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in paths},
        'fixture_sha256': hashlib.sha256(c_file.read_bytes()).hexdigest(),
        'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'host_sha256': hashlib.sha256(host_path.read_bytes()).hexdigest(),
        'cases': rows, 'passed': sum(row['pass'] for row in rows), 'total': len(rows),
        'compiler': command, 'sanitizer': args.sanitizer,
    }
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(0 if result['passed'] == result['total'] else 1)


if __name__ == '__main__':
    main()
