#!/usr/bin/env python3
"""Run the production outer WMT initialization and exit against tracked resources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


STAGES = {1: 'hif', 2: 'stp', 3: 'chrdev-region', 4: 'cdev',
          5: 'class', 6: 'device', 7: 'library'}
VARIANTS = {
    'module-dynamic': (0, 1, 0),
    'module-static': (0, 0, 1),
    'external-dynamic': (1, 1, 1),
    'external-static': (1, 0, 0),
}


def function(source, name):
    match = re.search(r'(?m)^[A-Za-z_][^;\n]*\b' + re.escape(name)
                      + r'\([^;{}]*\)\n\{', source)
    if not match:
        raise ValueError(f'Missing production function: {name}')
    return source[match.start():source.index('\n}', match.end()) + 2] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[7])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--legacy-library-step-cleanup', action='store_true',
                        help='Model the old library-owned STEP cleanup for baseline comparisons')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source_path = args.kernel / 'drivers/misc/mediatek/connectivity/source/common/common_main/linux/wmt_dev.c'
    source = source_path.read_text()
    debug_path = source_path.with_name('wmt_dbg.c')
    debug_source = debug_path.read_text()
    globals_ = re.search(r'enum wmt_init_status \{.*?\n\};', source, re.S)[0] + '\n'
    for name in ['gWmtMajor', 'gWmtInitStatus', 'gWmtInitWq', 'gWmtCdev']:
        globals_ += re.search(r'(?m)^static [^;\n]*\b' + name + r'\b[^;]*;', source)[0] + '\n'
    globals_ += re.search(r'struct wmt_dbg_work \{.*?\n\};', debug_source, re.S)[0] + '\n'
    production = '\n'.join(function(debug_source, name)
                           for name in ['wmt_dbg_func_ctrl', 'delay_work_func', 'wmt_dbg_delay_work',
                                        'wmt_dev_dbg_setup', 'wmt_dev_dbg_remove'])
    production += '\n'.join(function(source, name) for name in ['WMT_init', 'WMT_exit'])
    host_path = Path(__file__).with_name('wmt_outer_init_host.c')
    fixture = (host_path.read_text().replace('/* GLOBALS */', globals_)
               .replace('/* PRODUCTION */', production))
    c_file = args.output / 'test.c'
    c_file.write_text(fixture)
    rows = []
    commands = {}
    for variant, (external, dynamic, early) in VARIANTS.items():
        destination = args.output / variant
        destination.mkdir()
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-function', '-Wno-unused-parameter', '-fno-omit-frame-pointer',
            '-fno-sanitize-recover=all', '-no-pie', '-fsanitize=address,undefined',
            f'-DMTK_WCN_REMOVE_KO={external}', f'-DWMT_CREATE_NODE_DYNAMIC={dynamic}',
            f'-DLEGACY_LIBRARY_STEP_CLEANUP={int(args.legacy_library_step_cleanup)}']
        if early:
            command.append('-DCONFIG_EARLYSUSPEND')
        command += [str(c_file), '-o', str(destination / 'test')]
        commands[variant] = command
        compiled = subprocess.run(command, capture_output=True)
        (destination / 'compile.txt').write_bytes(compiled.stdout + compiled.stderr)
        compiled.check_returncode()
        cases = {0: 'success-soc', 1: 'success-combo', 2: 'duplicate-init',
                 3: 'exit-before-init', 27: 'sequential-failures-and-retry',
                 28: 'last-producer-work-during-exit', 29: 'poweroff-failure-during-exit',
                 30: 'pending-debug-commands-during-exit'}
        for stage, name in STAGES.items():
            if (stage == 1 and external) or (stage in (5, 6) and not dynamic):
                continue
            cases[9 + stage] = name + '-failure'
            cases[19 + stage] = name + '-failure-and-retry'
        for number, name in sorted(cases.items()):
            run = subprocess.run([str(destination / 'test'), str(number)],
                                 capture_output=True, timeout=10,
                                 env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))
            (destination / (name + '.txt')).write_bytes(run.stdout + run.stderr)
            row = {'variant': variant, 'case': name, 'exit_code': run.returncode,
                   'pass': run.returncode == 0}
            rows.append(row)
            print(('PASS' if row['pass'] else 'FAIL') + ': ' + variant + '/' + name,
                  flush=True)
    result = {
        'source_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(),
        'debug_source_sha256': hashlib.sha256(debug_path.read_bytes()).hexdigest(),
        'fixture_sha256': hashlib.sha256(c_file.read_bytes()).hexdigest(),
        'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'host_sha256': hashlib.sha256(host_path.read_bytes()).hexdigest(),
        'cases': rows, 'passed': sum(row['pass'] for row in rows), 'total': len(rows),
        'compiler': commands,
    }
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"{result['passed']}/{result['total']} passed", flush=True)
    raise SystemExit(0 if result['passed'] == result['total'] else 1)


if __name__ == '__main__':
    main()
