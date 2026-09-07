#!/usr/bin/env python3
"""Exercise extracted WMT v2 broker, VFS dispatch, metadata and consumer code."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

from test_wmt_buffers import function


ROOT = 'drivers/misc/mediatek/connectivity/'
BASE = ROOT + 'source/common/common_main/'
PATHS = {
    'lib': BASE + 'core/wmt_lib.c',
    'lib_h': BASE + 'core/include/wmt_lib.h',
    'dev': BASE + 'linux/wmt_dev.c',
    'ctrl': BASE + 'core/wmt_ctrl.c',
    'adapter': ROOT + 'common/wmt_build_in_adapter.c',
    'uapi': 'include/uapi/linux/mtk_wmt_cmd.h',
    'util': 'mm/util.c',
}
CASES = {
    0: 'status-success', 1: 'status-negative', 2: 'unknown-command-tagged-negative',
    3: 'metadata-success-status-is-insufficient', 4: 'queued-before-bind',
    5: 'bind-idempotent', 6: 'other-open-busy-and-controls-usable',
    7: 'unrelated-close-keeps-request', 8: 'owner-close-cancels',
    9: 'unbind-rebind-rejects-old-reply', 10: 'stale-unbind-keeps-new-session',
    11: 'timeout-late-reply-other-open', 12: 'timeout-late-reply-same-open',
    13: 'completed-duplicate-after-next-read-same-open',
    14: 'cancel-late-reply-other-open', 15: 'cancel-late-reply-same-open',
    16: 'short-read-and-copy-fault-retry', 17: 'write-copy-fault-retry',
    18: 'session-input-copy-fault', 19: 'bind-output-copy-fault',
    20: 'unbind-output-copy-fault', 21: 'zero-length-io',
    22: 'frame-size-limits', 23: 'malformed-frame-fields',
    24: 'wrong-session', 25: 'wrong-transaction', 26: 'wrong-open-with-current-token',
    27: 'reply-before-delivery', 28: 'duplicate-before-retirement',
    29: 'second-producer-busy', 30: 'open-allocation-failure',
    31: 'session-counter-overflow', 32: 'transaction-counter-overflow',
    33: 'shutdown-pending-request', 34: 'shutdown-before-start-and-retry',
    35: 'read-copy-crosses-deadline', 36: 'write-copy-crosses-deadline',
    37: 'validation-cancel-before-commit', 38: 'patch-cache-lock-crosses-deadline',
    39: 'ten-patch-records-canonical-order', 40: 'invalid-patch-lists',
    41: 'reply-copy-allocation-failure', 42: 'patch-array-allocation-failure',
    43: 'all-five-rom-types', 44: 'zero-optional-rom-records',
    45: 'invalid-rom-lists', 46: 'rom-first-record-and-cached-retry',
    47: 'each-rom-record-allocation-fails-atomically',
    48: 'expired-normal-metadata-does-not-poison-next-search',
    49: 'cancel-after-accepted-metadata-keeps-success',
    50: 'stale-rom-cannot-fill-wmt-cache', 51: 'normal-consumer-copies-and-bounds',
    52: 'rom-consumer-optional-retry-and-wmt-cache',
    53: 'legacy-setters-rejected-native-and-compat',
    54: 'bind-copy-crosses-request-deadline', 55: 'reply-private-copy-no-refetch',
    56: 'failed-bind-keeps-unassigned-request', 57: 'poll-owner-and-delivery-masks',
    58: 'session-input-validation', 59: 'session-id-survives-library-restart',
    60: 'unbind-before-delivery', 61: 'fwlog-return-propagation',
    62: 'rom-cache-lock-crosses-deadline',
    63: 'negative-normal-search-does-not-publish-cache',
    64: 'negative-rom-search-does-not-poison-retry',
    65: 'command-input-bounds-and-maximum-frame',
    66: 'unbound-timeout-cannot-be-resurrected-by-bind',
    67: 'poll-then-expiry-before-read', 68: 'unbind-copy-fault-keeps-pending-request',
    100: 'two-readers-sharing-owner', 101: 'copied-old-reply-after-next-read',
    102: 'accepted-reply-before-reset', 103: 'reset-before-copied-reply',
    104: 'consumer-copy-survives-cache-replacement',
}


def case_block(source, name):
    start = source.index('\tcase ' + name + ':')
    end = source.find('\tcase ', start + 1)
    if end < 0:
        end = source.index('\tdefault:', start)
    return source[start:end]


def build_fixture(sources):
    lib, dev = sources['lib'], sources['dev']
    types = re.search(r'typedef struct \{\n\tUINT32 dowloadSeq;.*?P_WMT_PATCH_INFO;',
                      sources['lib_h'], re.S)[0] + '\n'
    types += re.search(r'struct wmt_rom_patch_info \{.*?\n};',
                       sources['lib_h'], re.S)[0] + '\n'
    types += re.search(r'struct wmt_cmd_file \{.*?\n};', lib, re.S)[0] + '\n'
    types += re.search(r'struct wmt_cmd_reply \{.*?\n};', lib, re.S)[0] + '\n'
    state = '\n'.join(re.findall(
        r'^static (?:DEFINE_MUTEX\(g_(?:wmt_cmd|rom_patch_info)_lock\)|'
        r'bool g_wmt_cmd_\w+(?: = true)?|struct wmt_cmd_file \*g_wmt_cmd_\w+|'
        r'u64 g_wmt_cmd_\w+|unsigned long g_wmt_cmd_deadline|u16 g_wmt_cmd_reply_kind);$',
        lib, re.M)) + '\n'
    state += '\n'.join(re.findall(
        r'^static (?:P_WMT_PATCH_INFO pPatchInfo|UINT32 pAtchNum|'
        r'DEFINE_MUTEX\(g_patch_info_lock\)|bool g_patch_info_ready);$', dev, re.M)) + '\n'
    state += '\n'.join(re.findall(
        r'^static (?:DEFINE_SPINLOCK\(wmt_cmd_session_lock\)|u64 wmt_cmd_session_next);$',
        sources['adapter'], re.M)) + '\n'
    groups = {
        'adapter': ['wmt_export_alloc_cmd_session'],
        'util': ['memdup_user'],
        'lib': ['wmt_lib_cmd_finish', 'wmt_lib_cmd_expire', 'wmt_lib_cmd_start',
                'wmt_lib_cmd_disconnect', 'wmt_lib_cmd_shutdown', 'wmt_lib_cmd_open',
                'wmt_lib_cmd_close', 'wmt_lib_cmd_session', 'wmt_lib_cancel_cmd',
                'wmt_lib_send_cmd', 'wmt_lib_get_cmd_event', 'wmt_lib_read_cmd',
                'wmt_lib_cmd_reply_free', 'wmt_lib_cmd_reply_prepare',
                'wmt_lib_cmd_publish_rom', 'wmt_lib_write_cmd', 'wmt_lib_poll_cmd',
                'wmt_lib_set_patch_num', 'wmt_lib_set_patch_info',
                'wmt_lib_get_patch_info', 'wmt_lib_get_rom_patch_info',
                'wmt_lib_rom_patch_info_free'],
        'dev': ['wmt_dev_patch_info_free', 'wmt_dev_publish_patch_info',
                'wmt_dev_get_patch_info', 'WMT_open', 'WMT_close',
                'WMT_read', 'WMT_write', 'WMT_poll', 'WMT_compat_ioctl'],
        'ctrl': ['wmt_ctrl_ul_cmd', 'wmt_ctrl_patch_search', 'wmt_ctrl_get_patch_num',
                 'wmt_ctrl_get_patch_info', 'wmt_ctrl_get_rom_patch_info',
                 'wmt_ctrl_update_patch_version'],
    }
    bodies = [function(sources[group], name) for group, names in groups.items() for name in names]
    # These complete case bodies are unchanged excerpts of the real dispatcher.
    dispatch = ''.join(case_block(dev, name) for name in [
        'WMT_IOCTL_CMD2_SESSION', 'WMT_IOCTL_SET_PATCH_NAME', 'WMT_IOCTL_SET_PATCH_NUM',
        'WMT_IOCTL_SET_PATCH_INFO', 'WMT_IOCTL_SET_ROM_PATCH_INFO',
        'WMT_IOCTL_GET_CHIP_INFO', 'WMT_IOCTL_SET_LAUNCHER_KILL', 'WMT_IOCTL_FW_DBGLOG_CTRL'])
    bodies.append('LONG WMT_unlocked_ioctl(struct file *filp, UINT32 cmd, ULONG arg)\n{\n'
                  'INT32 iRet = 0; switch (cmd) {\n' + dispatch +
                  '\ndefault: return -EINVAL; } return iRet;\n}\n')
    prototypes = ''.join(body[:body.index('\n{')] + ';\n' for body in bodies)
    macros = '\n'.join(re.findall(r'^#define (?:COMPAT_)?WMT_IOC[^\n]*', dev, re.M)) + '\n'
    host = Path(__file__).with_name('wmt_command_v2_host.c').read_text()
    for marker, value in {'SOURCE_UAPI': sources['uapi'], 'SOURCE_TYPES': types,
                          'SOURCE_STATE': state, 'SOURCE_PROTOTYPES': prototypes,
                          'SOURCE_IOCTLS': macros, 'SOURCE_FUNCTIONS': '\n'.join(bodies)}.items():
        host = host.replace('/* ' + marker + ' */', value)
    # The full teardown remains covered by its existing source-derived fixture.
    deinit = function(lib, 'wmt_lib_deinit')
    assert deinit.index('wmt_lib_cmd_shutdown();') < deinit.index('osal_thread_destroy(')
    assert deinit.index('wait_event(g_wmt_op_pool_idle') < deinit.index('wmt_dev_patch_info_free();')
    assert deinit.index('wmt_dev_patch_info_free();') < deinit.index('osal_memset(&gDevWmt')
    assert 'wmt_dev_patch_info_free();' not in function(dev, 'WMT_exit')
    return host


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[7])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--revision')
    parser.add_argument('--case', type=int, action='append')
    parser.add_argument('--tsan', action='store_true')
    args = parser.parse_args()
    args.kernel = args.kernel.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    sources = {name: (subprocess.check_output(['git', 'show', args.revision + ':' + path],
                                             cwd=args.kernel).decode() if args.revision else
                      (args.kernel / path).read_text()) for name, path in PATHS.items()}
    fixture = args.output / 'fixture.c'
    fixture.write_text(build_fixture(sources))
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-pointer-sign',
        '-Wno-unused-parameter', '-Wno-unused-function', '-fno-omit-frame-pointer',
        '-no-pie', '-pthread', '-fsanitize=' + ('thread' if args.tsan else 'address,undefined'),
        str(fixture), '-o', str(args.output / 'fixture')]
    compiled = subprocess.run(command, capture_output=True)
    (args.output / 'compile.log').write_bytes(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    selected = args.case or [number for number in CASES if not args.tsan or number >= 100]
    rows = []
    for number in selected:
        env = dict(os.environ, ASAN_OPTIONS='halt_on_error=1:detect_leaks=1',
                   UBSAN_OPTIONS='halt_on_error=1', TSAN_OPTIONS='halt_on_error=1')
        attempts = 0
        while True:
            run = subprocess.run([str(args.output / 'fixture'), str(number)],
                                 capture_output=True, timeout=15, env=env)
            attempts += 1
            # TSan may fail before main on this host's randomized mappings.
            if not args.tsan or b'unexpected memory mapping' not in run.stderr or attempts == 50:
                break
            (args.output / f'{number}.mapping-{attempts}.log').write_bytes(run.stdout + run.stderr)
        (args.output / (CASES[number] + '.log')).write_bytes(run.stdout + run.stderr)
        row = dict(number=number, case=CASES[number], exit_code=run.returncode, attempts=attempts,
                   passed=run.returncode == 0, output=run.stdout.decode(errors='replace').strip())
        rows.append(row)
        print(('PASS ' if row['passed'] else 'FAIL ') + row['case'], flush=True)
    result = dict(passed=sum(row['passed'] for row in rows), total=len(rows), cases=rows,
                  revision=args.revision, sanitizer='thread' if args.tsan else 'address,undefined',
                  source_sha256={PATHS[name]: hashlib.sha256(text.encode()).hexdigest()
                                 for name, text in sources.items()}, compiler=command,
                  fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),
                  host_sha256=hashlib.sha256(Path(__file__).with_name('wmt_command_v2_host.c').read_bytes()).hexdigest(),
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  imported_runner_sha256=hashlib.sha256(Path(__file__).with_name('test_wmt_buffers.py').read_bytes()).hexdigest())
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(result['passed'] != result['total'])


if __name__ == '__main__':
    main()
