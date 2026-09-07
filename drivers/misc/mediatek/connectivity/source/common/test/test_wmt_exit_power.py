#!/usr/bin/env python3
"""Exercise actual WMT core and control power-off functions with hardware errors."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

from test_wmt_outer_init import function


CASES = [
    'poweroff-success', 'negative-hardware-error-and-retry',
    'positive-hardware-error-and-retry', 'retry-with-stp-already-closed',
    'stp-error-still-powers-off', 'poweroff-disabled-then-enabled',
    'core-already-off', 'control-already-off',
]
LIBRARY_CASES = [
    'library-joins-btm-before-off', 'library-checks-state-after-btm-reset',
    'library-idle-already-off', 'library-no-operation', 'library-psm-failure',
    'library-hardware-error-and-retry',
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[7])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    core_dir = args.kernel / 'drivers/misc/mediatek/connectivity/source/common/common_main/core'
    core_path = core_dir / 'wmt_core.c'
    ctrl_path = core_dir / 'wmt_ctrl.c'
    lib_path = core_dir / 'wmt_lib.c'
    host_path = Path(__file__).with_name('wmt_exit_power_host.c')
    fixture = host_path.read_text().replace(
        '/* CTRL_FUNCTION */', function(ctrl_path.read_text(), 'wmt_ctrl_hw_pwr_off'))
    fixture = fixture.replace('/* CORE_FUNCTION */', function(core_path.read_text(), 'opfunc_pwr_off'))
    source_paths = [core_path, ctrl_path, host_path, Path(__file__)]
    cases = CASES
    if lib_path.exists() and 'INT32 wmt_lib_power_off_for_exit(VOID)' in lib_path.read_text():
        fixture = '#define HOST_EXIT_HELPER 1\n' + fixture.replace(
            '/* LIBRARY_FUNCTION */', function(lib_path.read_text(), 'wmt_lib_power_off_for_exit'))
        source_paths.append(lib_path)
        cases = CASES + LIBRARY_CASES
    c_file = args.output / 'test.c'
    c_file.write_text(fixture)
    binary = args.output / 'test'
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-Wno-unused-function', '-fno-omit-frame-pointer',
        '-fno-sanitize-recover=all', '-no-pie', '-fsanitize=address,undefined',
        str(c_file), '-o', str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True)
    (args.output / 'compile.txt').write_bytes(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    rows = []
    for scenario, name in enumerate(cases):
        result = subprocess.run([str(binary), str(scenario)], capture_output=True,
                                timeout=10, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))
        (args.output / (name + '.txt')).write_bytes(result.stdout + result.stderr)
        passed = result.returncode == 0
        rows.append({'case': name, 'pass': passed, 'exit_code': result.returncode})
        print(('PASS' if passed else 'FAIL') + ': ' + name, flush=True)
    summary = {
        'source_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in source_paths},
        'fixture_sha256': hashlib.sha256(c_file.read_bytes()).hexdigest(),
        'compiler': command, 'cases': rows, 'total': len(rows),
        'passed': sum(row['pass'] for row in rows),
    }
    (args.output / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f"{summary['passed']}/{summary['total']} passed", flush=True)
    raise SystemExit(0 if summary['passed'] == summary['total'] else 1)


if __name__ == '__main__':
    main()
