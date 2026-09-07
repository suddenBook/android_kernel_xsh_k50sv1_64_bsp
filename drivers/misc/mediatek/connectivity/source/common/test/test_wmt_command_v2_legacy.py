#!/usr/bin/env python3
"""Retain real legacy counter-histories; observations are not passing v2 tests."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

from test_wmt_buffers import function
from test_wmt_command import PREFIX
from test_wmt_command_identity import TESTS as IDENTITY_TESTS, CASES as IDENTITY_CASES


METADATA_TESTS = r'''
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "line %d: %s (case %d phase %d)\n", __LINE__, #value, scenario, phase); abort(); \
} } while (0)
static int phase;
static struct file owner;
static void on_event(void) {}
static void on_copy(void) {}
static void read_command(void)
{
    char buffer[256] = {0};
    CHECK(WMT_read(&owner, buffer, sizeof(buffer), NULL) > 0);
}
static void put_patch(unsigned sequence, const char *prefix)
{
    WMT_PATCH_INFO patch = {.dowloadSeq = sequence};
    snprintf((char *)patch.patchName, sizeof(patch.patchName), "%s%u.bin", prefix, sequence);
    CHECK(wmt_dev_set_patch_info(&patch) == 0);
}
static void put_rom(unsigned type, const char *prefix)
{
    struct wmt_rom_patch_info patch = {.type = type};
    snprintf((char *)patch.patchName, sizeof(patch.patchName), "%s%u.bin", prefix, type);
    CHECK(wmt_lib_set_rom_patch_info(&patch, type) == 0);
}
static void on_wait(void)
{
    if (phase && scenario == 0) {
        /* A's serial handler finishes after B queues, before reading B. */
        put_patch(2, "A");
        CHECK(wmt_lib_get_patch_info() != NULL);
        CHECK(wmt_dev_set_patch_num(2) == -EBUSY);
    }
    if (phase && scenario == 3) put_rom(4, "A");
    read_command();
    if (!phase) {
        if (scenario == 0 || scenario == 1 || scenario == 5) {
            CHECK(wmt_dev_set_patch_num(2) == 0);
            put_patch(1, "A");
            if (scenario != 0) put_patch(2, "A");
        }
        if (scenario == 3 || scenario == 4) put_rom(0, "A");
        if (scenario == 1 || scenario == 4) CHECK(WMT_write(&owner, "fail", 4, NULL) == 4);
        return;
    }
    if (scenario == 2) {
        CHECK(wmt_dev_set_patch_num(2) == 0);
        put_patch(1, "B"); put_patch(2, "B");
        /* A's old per-record setter has no identity and replaces B's slot. */
        put_patch(1, "A");
    }
    if (scenario == 4) put_rom(0, "B");
    CHECK(WMT_write(&owner, scenario == 0 ? "fail" : "ok", scenario == 0 ? 4 : 2, NULL) > 0);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); scenario = atoi(argv[1]); CHECK(scenario >= 0 && scenario <= 5);
    const char *request = scenario == 3 || scenario == 4 ? "srh_rom_patch" : (const char *)command;
    int result = wmt_ctrl_ul_cmd(&gDevWmt, (PUINT8)request);
    CHECK(result == (scenario == 1 || scenario == 4 ? -1 : -ETIMEDOUT));
    if (scenario == 1 || scenario == 5) {
        /* This is the exact selected-SoC startup cache gate from its source. */
        CHECK(legacy_startup_would_search() == 0);
    } else {
        phase = 1;
        CHECK(wmt_ctrl_ul_cmd(&gDevWmt, (PUINT8)request) == (scenario == 0 ? -1 : 0));
    }
    unsigned char name[256], address[4];
    if (scenario <= 2 || scenario == 5) {
        CHECK(wmt_dev_get_patch_info(1, name, address) == 0 && !strcmp((char *)name, "A1.bin"));
        CHECK(wmt_dev_get_patch_info(2, name, address) == 0);
        CHECK(!strcmp((char *)name, scenario == 2 ? "B2.bin" : "A2.bin"));
    } else {
        CHECK(wmt_lib_get_rom_patch_info(scenario == 3 ? 4 : 0, name, address) == 0);
        CHECK(!strcmp((char *)name, scenario == 3 ? "A4.bin" : "A0.bin"));
    }
    wmt_dev_patch_info_free(); wmt_lib_rom_patch_info_free();
    printf("{\"scenario\":%d,\"legacy_metadata_issue_observed\":true}\n", scenario);
    return 0;
}
'''
METADATA_CASES = [
    'serial-late-record-finishes-expired-cache', 'failed-ack-cache-skips-next-search',
    'late-record-mixes-next-cache', 'late-rom-fills-wmt-cache-gate',
    'failed-rom-ack-first-record-survives-retry', 'timed-out-complete-cache-skips-next-search',
]


def fixture(sources, metadata):
    lib, ctrl, dev, header, soc = sources
    production = '\n'.join(re.findall(
        r'^static (?:DEFINE_MUTEX\(g_wmt_cmd_lock\)|bool g_wmt_cmd_\w+);$', lib, re.M)) + '\n'
    names = ['wmt_lib_trigger_cmd_signal', 'wmt_lib_cancel_cmd', 'wmt_lib_send_cmd',
             'wmt_lib_read_cmd', 'wmt_lib_get_cmd_status']
    bodies = [function(lib, name) for name in names]
    bodies += [function(ctrl, 'wmt_ctrl_ul_cmd'), function(dev, 'WMT_write'), function(dev, 'WMT_read')]
    prefix = PREFIX
    if not metadata:
        prefix = prefix.replace('struct file { int unused; };',
                                'struct file { unsigned long delivered_generation; };')
        return prefix + production + '\n'.join(bodies) + IDENTITY_TESTS
    types = re.search(r'typedef struct \{\n\tUINT32 dowloadSeq;.*?P_WMT_PATCH_INFO;', header, re.S)[0] + '\n'
    types += re.search(r'struct wmt_rom_patch_info \{.*?\n};', header, re.S)[0] + '\n'
    prefix = prefix.replace('typedef struct {\n    unsigned long state;',
                            types + 'typedef struct {\n    unsigned long state;')
    prefix = prefix.replace('    OSAL_EVENT cmdReq;\n',
                            '    OSAL_EVENT cmdReq;\n    UINT32 patchNum;\n'
                            '    P_WMT_PATCH_INFO pWmtPatchInfo;\n'
                            '    struct wmt_rom_patch_info *pWmtRomPatchInfo[5];\n')
    prefix += '\ntypedef int ENUM_WMTDRV_TYPE_T; typedef size_t SIZE_T; typedef unsigned long ULONG;\n'
    prefix += '#define WMTDRV_TYPE_ANT 5\n#define MAX_PATCH_NUM 10\n#define GFP_KERNEL 0\n'
    prefix += '#define BIT(i) (1UL << (i))\n#define READ_ONCE(x) (x)\n#define WRITE_ONCE(x,v) ((x)=(v))\n'
    prefix += '#define kcalloc(n,s,f) calloc((n),(s))\n#define kfree free\n'
    production += '\n'.join(re.findall(
        r'^static (?:P_WMT_PATCH_INFO pPatchInfo|UINT32 pAtchNum|DEFINE_MUTEX\(g_patch_info_lock\)|'
        r'unsigned long g_patch_info_seen|bool g_patch_info_ready);$', dev, re.M)) + '\n'
    production += re.search(r'^static DEFINE_MUTEX\(g_rom_patch_info_lock\);$', lib, re.M)[0] + '\n'
    bodies += [function(lib, name) for name in [
        'wmt_lib_set_patch_num', 'wmt_lib_set_patch_info', 'wmt_lib_get_patch_info',
        'wmt_lib_set_rom_patch_info', 'wmt_lib_get_rom_patch_info', 'wmt_lib_rom_patch_info_free']]
    bodies += [function(dev, name) for name in [
        'wmt_dev_patch_info_free', 'wmt_dev_set_patch_num', 'wmt_dev_set_patch_info', 'wmt_dev_get_patch_info']]
    gate = re.search(r'if \(patch_num == 0 \|\| wmt_lib_get_patch_info\(\) == NULL\)', soc)[0]
    bodies += ['static int legacy_startup_would_search(void)\n{\n'
               'UINT32 patch_num = gDevWmt.patchNum;\n' + gate + '\n    return 1;\nreturn 0;\n}\n']
    return prefix + production + '\n'.join(bodies) + METADATA_TESTS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    base = 'drivers/misc/mediatek/connectivity/source/common/common_main/'
    paths = [base + name for name in ['core/wmt_lib.c', 'core/wmt_ctrl.c', 'linux/wmt_dev.c',
                                     'core/include/wmt_lib.h', 'core/wmt_ic_soc.c']]
    sources = [subprocess.check_output(['git', 'show', args.revision + ':' + path],
                                      cwd=args.kernel).decode() for path in paths]
    rows, compilers, fixtures = [], [], {}
    for metadata in [False, True]:
        mode = 'metadata' if metadata else 'identity'
        source, binary = args.output / (mode + '.c'), args.output / mode
        source.write_text(fixture(sources, metadata))
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-pointer-sign',
            '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-but-set-variable',
            '-fno-omit-frame-pointer', '-no-pie', '-pthread', '-fsanitize=address,undefined',
            str(source), '-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True)
        (args.output / (mode + '.compile.log')).write_bytes(compiled.stdout + compiled.stderr)
        compiled.check_returncode()
        compilers.append(command)
        fixtures[mode] = hashlib.sha256(source.read_bytes()).hexdigest()
        cases = [(f'metadata-{name}', [str(i)]) for i, name in enumerate(METADATA_CASES)] if metadata else [
            (f'{"affinity-model" if affinity else "production"}-{name}', [str(affinity), str(number)])
            for affinity in [0, 1] for number, name in IDENTITY_CASES.items()]
        for name, arguments in cases:
            run = subprocess.run([str(binary)] + arguments, capture_output=True, timeout=10,
                                 env=dict(os.environ, ASAN_OPTIONS='halt_on_error=1:detect_leaks=1',
                                          UBSAN_OPTIONS='halt_on_error=1'))
            (args.output / (name + '.log')).write_bytes(run.stdout + run.stderr)
            rows.append(dict(case=name, expected_observation_matched=run.returncode == 0,
                             exit_code=run.returncode, observation=json.loads(run.stdout) if run.returncode == 0 else None))
            print(('OBSERVED ' if run.returncode == 0 else 'UNEXPECTED ') + name, flush=True)
    result = dict(purpose='Legacy counter-histories, not candidate pass results', revision=args.revision,
                  matched=sum(row['expected_observation_matched'] for row in rows), total=len(rows), cases=rows,
                  source_sha256={path: hashlib.sha256(source.encode()).hexdigest() for path, source in zip(paths, sources)},
                  fixture_sha256=fixtures, compilers=compilers,
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  imports_sha256={name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                                  for name in ['test_wmt_buffers.py', 'test_wmt_command.py', 'test_wmt_command_identity.py']})
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(result['matched'] != result['total'])


if __name__ == '__main__':
    main()
