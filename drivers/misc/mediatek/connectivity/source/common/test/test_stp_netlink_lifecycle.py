#!/usr/bin/env python3
"""Run actual STP netlink functions with controlled pthread interleavings."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

from test_wmt_outer_init import function


CASES = ['register-close-register-restores-ops', 'close-waits-for-send',
         'close-drains-admitted-bind', 'refused-send-cannot-corrupt-closed-count',
         'empty-recipient-and-error-exits-unlock',
         'refused-first-peer-preserves-three-listeners']


def fixture(source, host):
    state = []
    for marker in ['__STP_DBG_ATTR_INVALID', '__STP_DBG_COMMAND_INVALID']:
        state.append(re.search(r'enum \{\s*' + marker + r'.*?\n\};', source, re.S)[0])
    state.append(re.search(r'(?m)^#define MAX_BIND_PROCESS[^\n]*', source)[0])
    state.append(re.search(r'(?m)^static OSAL_SLEEPABLE_LOCK g_dbg_nl_lock(?:\s*=\s*\{.*?^\})?;', source, re.S)[0])
    for name in ['g_dbg_nl_lifecycle_lock', 'g_dbg_nl_registered', 'stp_dbg_seqnum',
                 'num_bind_process', 'bind_pid', 'g_core_dump']:
        declaration = re.search(r'(?m)^static [^\n]*\b' + name + r'\b[^\n]*;', source)
        if declaration:
            state.append(declaration[0])
    for name in ['stp_dbg_genl_policy', 'stp_dbg_gnl_ops_array', 'stp_dbg_gnl_family']:
        state.append(re.search(r'(?m)^static struct [^\n]*\b' + name + r'\b[^\n]*\{.*?^\};', source, re.S)[0])
    bodies = '\n'.join(function(source, name) for name in
                       ['stp_dbg_nl_init', 'stp_dbg_nl_deinit', 'stp_dbg_nl_bind', 'stp_dbg_nl_send'])
    return host.replace('/* SOURCE_STATE */', '\n'.join(state)).replace('/* SOURCE_FUNCTIONS */', bodies)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--revision', help='Read the exact production file at this git revision')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    relative = Path('drivers/misc/mediatek/connectivity/source/common/common_main/linux/stp_dbg.c')
    source = (subprocess.check_output(['git', 'show', args.revision + ':' + str(relative)], cwd=args.kernel)
              if args.revision else (args.kernel / relative).read_bytes())
    host = Path(__file__).with_name('stp_netlink_lifecycle_host.c')
    c_file = args.output / 'test.c'
    c_file.write_text(fixture(source.decode(), host.read_text()))
    binary = args.output / 'test'
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-but-set-variable',
        '-fno-omit-frame-pointer', '-fno-sanitize-recover=all', '-no-pie',
        '-fsanitize=address,undefined', str(c_file), '-o', str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True)
    (args.output / 'compile.txt').write_bytes(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    rows = []
    for scenario, name in enumerate(CASES):
        try:
            result = subprocess.run([str(binary), str(scenario)], capture_output=True, timeout=8,
                                    env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))
            output = result.stdout + result.stderr
            code = result.returncode
        except subprocess.TimeoutExpired as error:
            output = (error.stdout or b'') + (error.stderr or b'') + b'\nTimed out after 8 seconds.\n'
            code = 124
        (args.output / (name + '.txt')).write_bytes(output)
        rows.append({'case': name, 'exit_code': code, 'pass': code == 0})
        print(('PASS' if code == 0 else 'FAIL') + ': ' + name, flush=True)
    summary = {'source_sha256': hashlib.sha256(source).hexdigest(), 'revision': args.revision,
               'host_sha256': hashlib.sha256(host.read_bytes()).hexdigest(),
               'fixture_sha256': hashlib.sha256(c_file.read_bytes()).hexdigest(),
               'compiler': command, 'cases': rows, 'total': len(rows),
               'passed': sum(row['pass'] for row in rows)}
    (args.output / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f"{summary['passed']}/{summary['total']} passed", flush=True)
    raise SystemExit(0 if summary['passed'] == summary['total'] else 1)


if __name__ == '__main__':
    main()
