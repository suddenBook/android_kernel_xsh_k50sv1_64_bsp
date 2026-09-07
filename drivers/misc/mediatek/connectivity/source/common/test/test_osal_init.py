#!/usr/bin/env python3
"""Exercise actual OSAL thread/FIFO/wake-source initialization with injected kernel failures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

CASES = {
    0: 'thread-create-run-destroy', 1: 'thread-enomem-clears-handle',
    2: 'thread-preserves-eintr', 3: 'fifo-descriptor-allocation-failure',
    4: 'fifo-data-allocation-failure', 5: 'fifo-roundtrip-and-release',
    6: 'fifo-reinitialization-releases-descriptor', 7: 'fifo-invalid-external-capacity',
    8: 'wake-source-allocation-failure', 9: 'wake-source-idempotent-init',
    10: 'retry-after-allocation-failures', 11: 'invalid-initializer-arguments',
}


def function(source, name):
    pattern = r'(?m)^[A-Za-z_][^;\n]*\b' + re.escape(name) + r'\([^;{}]*\)\n\{'
    match = re.search(pattern, source)
    if not match:
        raise ValueError(f'Missing production function: {name}')
    end = source.index('\n}', match.end()) + 2
    return source[match.start():end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[7])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    base = args.kernel / 'drivers/misc/mediatek/connectivity/source/common/common_main/linux'
    paths = [base/'osal.c', base/'include/osal.h']
    source, header = [path.read_text() for path in paths]
    structs = '\n'.join(re.search(r'typedef struct ' + name + r' \{.*?\n\} [^;]+;', header, re.S)[0]
                        for name in ['_OSAL_FIFO_', '_OSAL_THREAD_', '_OSAL_WAKE_LOCK_'])
    names = ['osal_thread_create', 'osal_thread_run', 'osal_thread_destroy',
             '_osal_fifo_init', '_osal_fifo_deinit', '_osal_fifo_size', '_osal_fifo_avail_size',
             '_osal_fifo_len', '_osal_fifo_is_empty', '_osal_fifo_is_full', '_osal_fifo_data_in',
             '_osal_fifo_data_out', '_osal_fifo_reset', 'osal_fifo_deinit', 'osal_fifo_init',
             'osal_wake_lock_init', 'osal_wake_lock_deinit']
    production = structs + '\n' + '\n'.join(function(source, name) for name in names)
    host_path = Path(__file__).with_name('osal_init_host.c')
    fixture = host_path.read_text().replace('/* PRODUCTION */', production)
    c_file = args.output/'test.c'
    c_file.write_text(fixture)
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-pointer-sign',
        '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-sign-compare', '-fno-omit-frame-pointer',
        '-no-pie', '-fsanitize=address,undefined', str(c_file), '-o', str(args.output/'test')]
    compiled = subprocess.run(command, capture_output=True)
    (args.output/'compile.txt').write_bytes(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    rows = []
    for number, name in CASES.items():
        run = subprocess.run([str(args.output/'test'), str(number)], capture_output=True,
                             timeout=15, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
        (args.output/(name+'.txt')).write_bytes(run.stdout + run.stderr)
        rows.append({'case': name, 'exit_code': run.returncode, 'pass': run.returncode == 0})
        print(('PASS' if run.returncode == 0 else 'FAIL') + ': ' + name, flush=True)
    result = {
        'source_sha256': {str(path.relative_to(args.kernel)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in paths},
        'fixture_sha256': hashlib.sha256(c_file.read_bytes()).hexdigest(),
        'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'host_sha256': hashlib.sha256(host_path.read_bytes()).hexdigest(),
        'cases': rows, 'passed': sum(row['pass'] for row in rows), 'total': len(rows),
        'compiler': command,
    }
    (args.output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    raise SystemExit(0 if result['passed'] == result['total'] else 1)


if __name__ == '__main__':
    main()
