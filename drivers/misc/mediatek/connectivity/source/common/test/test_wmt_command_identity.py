#!/usr/bin/env python3
"""Characterize untagged WMT replies; this test does not implement a protocol fix."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

from test_wmt_command import PREFIX, function


TESTS = r'''
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #value); abort(); \
} } while (0)
static struct file open_a, open_b;
static struct file *model_owner;
static unsigned long model_generation;
static int affinity_model, phase;
static int first_result, second_result = -999;
static int stale_reply_result = -999, genuine_reply_result = -999;
static bool stale_reply_accepted;

/* Test-only open affinity plus an implicit per-open generation. No production
 * function is patched. Updating this generation on read B cannot identify which
 * request an untagged write from the same open actually answers.
 */
static ssize_t model_read(struct file *file, char *buffer, size_t count)
{
    ssize_t result = WMT_read(file, buffer, count, NULL);
    if (affinity_model && result > 0) {
        model_owner = file;
        file->delivered_generation = model_generation;
    }
    return result;
}

static ssize_t model_write(struct file *file, const char *buffer, size_t count)
{
    if (affinity_model && (model_owner != file ||
        file->delivered_generation != model_generation))
        return -ESTALE;
    ssize_t result = WMT_write(file, buffer, count, NULL);
    if (affinity_model && result > 0) model_owner = NULL;
    return result;
}

static void on_event(void) { model_generation++; }
static void on_copy(void) {}

static void read_command(struct file *file, const char *expected)
{
    char buffer[NAME_MAX + 1] = {0};
    CHECK(model_read(file, buffer, sizeof(buffer)) == (ssize_t)strlen(expected));
    CHECK(!strcmp(buffer, expected));
}

static void on_wait(void)
{
    if (scenario >= 11 && scenario <= 15) {
        if (!phase) {
            read_command(&open_a, "srh_patch");
            if (scenario == 13) CHECK(model_write(&open_a, "ok", 2) == 2);
            else if (scenario == 14 || scenario == 15) wmt_lib_cancel_cmd();
            /* Other first requests expire without a response. */
            return;
        }
        /* Existing guards correctly reject stale replies before B is read. */
        CHECK(model_write(&open_a, "ok", 2) < 0);
        bool cross_open = scenario == 11 || scenario == 14;
        struct file *reader = cross_open ? &open_b : &open_a;
        read_command(reader, "update_patch_version");
        stale_reply_result = model_write(&open_a, "ok", 2);
        stale_reply_accepted = stale_reply_result == 2;
        /* B's real handler failed. A's stale success must not replace that. */
        genuine_reply_result = model_write(reader, "fail", 4);
        CHECK(stale_reply_accepted == !(cross_open && affinity_model));
        CHECK(stale_reply_accepted ? genuine_reply_result < 0 : genuine_reply_result == 4);
        return;
    }

    if (scenario == 16) {
        char buffer[NAME_MAX + 1] = {0};
        CHECK(model_read(&open_a, buffer, 0) == 0);
        CHECK(model_read(&open_a, buffer, 2) == -EMSGSIZE);
        CHECK(wmt_lib_get_cmd_status());
        CHECK(model_write(&open_a, "ok", 2) < 0);
        fail_copy = 1;
        CHECK(model_read(&open_a, buffer, sizeof(buffer)) == -EFAULT);
        fail_copy = 0;
        CHECK(wmt_lib_get_cmd_status());
        CHECK(model_write(&open_a, "ok", 2) < 0);
    }
    read_command(&open_a, "srh_patch");
    if (scenario == 17) {
        fail_copy = 1;
        CHECK(model_write(&open_a, "ok", 2) == -EFAULT);
        fail_copy = 0;
    }
    genuine_reply_result = scenario == 18 ? model_write(&open_a, "fail", 4) :
                                          model_write(&open_a, "ok", 2);
    CHECK(genuine_reply_result == (scenario == 18 ? 4 : 2));
    if (scenario == 19) CHECK(model_write(&open_a, "ok", 2) < 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    affinity_model = atoi(argv[1]); scenario = atoi(argv[2]);
    CHECK(scenario >= 10 && scenario <= 19);
    first_result = wmt_ctrl_ul_cmd(&gDevWmt, (PUINT8)command);
    if (scenario >= 11 && scenario <= 15) {
        CHECK(first_result == (scenario == 13 ? 0 :
              (scenario == 14 || scenario == 15 ? -ECANCELED : -ETIMEDOUT)));
        phase = 1;
        second_result = wmt_ctrl_ul_cmd(&gDevWmt, (PUINT8)"update_patch_version");
        CHECK(second_result == (stale_reply_accepted ? 0 : -1));
        CHECK(event_calls == 2);
    } else {
        CHECK(first_result == (scenario == 18 ? -1 : 0));
        CHECK(event_calls == 1);
    }
    CHECK(!wmt_lib_get_cmd_status());
    CHECK(model_write(&open_a, "ok", 2) < 0);
    printf("{\"affinity_model\":%s,\"scenario\":%d,\"first_result\":%d,"
           "\"second_result\":%d,\"stale_reply_result\":%d,"
           "\"genuine_reply_result\":%d,\"stale_reply_accepted\":%s}\n",
           affinity_model ? "true" : "false", scenario, first_result, second_result,
           stale_reply_result, genuine_reply_result, stale_reply_accepted ? "true" : "false");
    return 0;
}
'''

CASES = {
    10: 'normal-command',
    11: 'timeout-late-reply-other-open',
    12: 'timeout-late-reply-same-open',
    13: 'completed-duplicate-after-next-read-same-open',
    14: 'cancel-late-reply-other-open',
    15: 'cancel-late-reply-same-open',
    16: 'short-read-and-efault-retry',
    17: 'write-efault-retry',
    18: 'normal-error-response',
    19: 'duplicate-before-next-command',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[7])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.kernel = args.kernel.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.kernel / 'drivers/misc/mediatek/connectivity/source/common/common_main'
    paths = [root / path for path in ['core/wmt_lib.c', 'core/wmt_ctrl.c', 'linux/wmt_dev.c']]
    lib, ctrl, dev = [path.read_text() for path in paths]
    production = '\n'.join(re.findall(
        r'^static (?:DEFINE_MUTEX\(g_wmt_cmd_lock\)|bool g_wmt_cmd_\w+);$', lib, re.M)) + '\n'
    for name in ['wmt_lib_trigger_cmd_signal', 'wmt_lib_cancel_cmd', 'wmt_lib_send_cmd',
                 'wmt_lib_read_cmd', 'wmt_lib_get_cmd_status']:
        production += function(lib, name)
    production += function(ctrl, 'wmt_ctrl_ul_cmd')
    production += function(dev, 'WMT_write') + function(dev, 'WMT_read')
    prefix = PREFIX.replace('struct file { int unused; };',
                            'struct file { unsigned long delivered_generation; };')
    assert prefix != PREFIX
    c_file = args.output / 'test.c'
    c_file.write_text(prefix + production + TESTS)
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-pointer-sign',
        '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-but-set-variable',
        '-fno-omit-frame-pointer', '-no-pie', '-pthread', '-fsanitize=address,undefined',
        str(c_file), '-o', str(args.output / 'test'),
    ]
    compiled = subprocess.run(command, capture_output=True)
    (args.output / 'compile.txt').write_bytes(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    rows = []
    for affinity in [0, 1]:
        for number, case in CASES.items():
            name = ('affinity-model-' if affinity else 'production-') + case
            run = subprocess.run([str(args.output / 'test'), str(affinity), str(number)],
                                 capture_output=True, timeout=10,
                                 env=dict(os.environ, ASAN_OPTIONS='halt_on_error=1'))
            (args.output / (name + '.txt')).write_bytes(run.stdout + run.stderr)
            observation = json.loads(run.stdout) if run.returncode == 0 else None
            rows.append(dict(case=name, exit_code=run.returncode,
                             expected_observation_matched=run.returncode == 0, observation=observation))
            print(('OBSERVED: ' if run.returncode == 0 else 'UNEXPECTED: ') + name, flush=True)
    result = dict(
        purpose='Legacy ABI boundary characterization, not verification of a production fix',
        production_fix=False, matched=sum(row['expected_observation_matched'] for row in rows),
        total=len(rows), cases=rows,
        source_sha256={str(path.relative_to(args.kernel)): hashlib.sha256(path.read_bytes()).hexdigest()
                       for path in paths},
        runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        imported_runner_sha256=hashlib.sha256(Path(__file__).with_name('test_wmt_command.py').read_bytes()).hexdigest(),
        fixture_sha256=hashlib.sha256(c_file.read_bytes()).hexdigest(), compiler=command,
    )
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(result['matched'] != result['total'])


if __name__ == '__main__':
    main()
