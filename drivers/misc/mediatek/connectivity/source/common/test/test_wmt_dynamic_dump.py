#!/usr/bin/env python3
"""Test the real WMT dynamic-dump ioctl and the kernel's integer parser."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


STUBS = r'''
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WMT_IOCTL_DYNAMIC_DUMP_CTRL 1
#define GFP_KERNEL 0
#define WMT_INFO_FUNC(...) ((void)0)
#define WMT_ERR_FUNC(...) ((void)0)
#define unlikely(x) (x)
#define div_u64(value, base) ((value) / (base))
#define KSTRTOX_OVERFLOW (1U << 31)
#define kstrtou32 kstrtouint
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef uint8_t UINT8;
typedef char INT8;
typedef uint8_t *PUINT8;
typedef void *PVOID;
typedef unsigned long ULONG;
typedef long LONG;
static int fail_alloc, fail_copy, platform_error;
static int allocations, copy_calls, platform_calls;
static UINT32 applied[10];

static void *kmalloc(size_t count, int flags)
{
    (void)flags;
    if (fail_alloc) return NULL;
    void *memory = malloc(count);
    if (memory) allocations++;
    return memory;
}
static void kfree(void *memory)
{
    if (memory) allocations--;
    free(memory);
}
static unsigned long copy_from_user(void *to, const void *from, size_t count)
{
    copy_calls++;
    if (fail_copy) return count;
    memcpy(to, from, count);
    return 0;
}
static INT32 wmt_plat_set_dynamic_dumpmem(UINT32 *values)
{
    platform_calls++;
    if (platform_error) return platform_error;
    memcpy(applied, values, sizeof(applied));
    return 0;
}
'''


TESTS = r'''
struct test_case {
    const char *name;
    const char *input;
    int expected;
    UINT32 values[10];
    int allocation_failure;
    int copy_failure;
    int backend_failure;
};

static const struct test_case tests[] = {
    { "empty-clears", "", 0, {0} },
    { "single", "1", 0, {1} },
    { "single-zero", "0", 0, {0} },
    { "pair", "1/2", 0, {1,2} },
    { "pair-trailing-slash", "1/2/", 0, {1,2} },
    { "ten", "1/2/3/4/5/6/7/8/9/10", 0, {1,2,3,4,5,6,7,8,9,10} },
    { "ten-trailing-slash", "1/2/3/4/5/6/7/8/9/10/", 0, {1,2,3,4,5,6,7,8,9,10} },
    { "radix", "0xffffffff/0x80000000/077", 0, {UINT32_MAX,0x80000000,63} },
    { "uint32-max", "4294967295", 0, {UINT32_MAX} },
    { "plus", "+1/+2", 0, {1,2} },
    { "newline", "42\n", 0, {42} },
    { "eleven-character-value", "00000000001/2/", 0, {1,2} },
    { "long-leading-zeroes", "00000000000000000000000000000000000000001/2", 0, {1,2} },
    { "empty-interior", "1//2", -EINVAL, {0} },
    { "empty-first", "/1", -EINVAL, {0} },
    { "only-separator", "/", -EINVAL, {0} },
    { "double-trailing-separator", "1//", -EINVAL, {0} },
    { "too-many", "1/2/3/4/5/6/7/8/9/10/11", -EINVAL, {0} },
    { "too-many-trailing-slash", "1/2/3/4/5/6/7/8/9/10/11/", -EINVAL, {0} },
    { "decimal-overflow", "4294967296/", -ERANGE, {0} },
    { "hex-overflow", "0x100000000/", -ERANGE, {0} },
    { "negative", "-1/", -EINVAL, {0} },
    { "invalid-middle", "1/no/2/", -EINVAL, {0} },
    { "non-ascii", "\xff/", -EINVAL, {0} },
    { "bad-octal", "09/", -EINVAL, {0} },
    { "copy-failure", "1/2/", -EFAULT, {0}, 0, 1, 0 },
    { "allocation-failure", "1/2/", -ENOMEM, {0}, 1, 0, 0 },
    { "platform-failure", "1/2/", -ENODEV, {0}, 0, 0, -ENODEV },
};

int main(void)
{
    unsigned failures = 0, checks = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(tests) + 2; i++) {
        unsigned char input[DYNAMIC_DUMP_BUF + 1] = {0};
        UINT32 expected[10] = {0};
        const char *name;
        int expected_ret = 0, expected_calls = 1;
        fail_alloc = fail_copy = platform_error = 0;
        allocations = copy_calls = platform_calls = 0;
        for (unsigned j = 0; j < ARRAY_SIZE(applied); j++) applied[j] = 0xa5a5a5a5;
        if (i < ARRAY_SIZE(tests)) {
            const struct test_case *test = &tests[i];
            name = test->name;
            memcpy(input, test->input, strlen(test->input));
            expected_ret = test->expected;
            memcpy(expected, test->values, sizeof(expected));
            fail_alloc = test->allocation_failure;
            fail_copy = test->copy_failure;
            platform_error = test->backend_failure;
            expected_calls = expected_ret && !platform_error ? 0 : 1;
        } else if (i == ARRAY_SIZE(tests)) {
            name = "full-buffer-zero-value";
            memset(input, '0', DYNAMIC_DUMP_BUF);
        } else {
            name = "padding-after-terminator";
            memcpy(input, "1\0/2/3/", 8);
            expected[0] = 1;
        }
        LONG result = run_dynamic_dump_ioctl((ULONG)input);
        int pass = result == expected_ret && platform_calls == expected_calls && allocations == 0;
        if (expected_ret) {
            for (unsigned j = 0; j < ARRAY_SIZE(applied); j++) pass &= applied[j] == 0xa5a5a5a5;
        } else {
            pass &= memcmp(applied, expected, sizeof(expected)) == 0;
        }
        pass &= copy_calls == (fail_alloc ? 0 : 1);
        printf("%s %s: ret=%ld expected=%d platform_calls=%d expected_calls=%d\n",
               pass ? "PASS" : "FAIL", name, result, expected_ret, platform_calls, expected_calls);
        failures += !pass;
        checks++;
    }
    printf("%u/%u passed\n", checks - failures, checks);
    return failures != 0;
}
'''


def function(text, name):
    match = re.search(r'\b' + re.escape(name) + r'\(', text)
    if match is None:
        return ''
    start = text.rfind('\n', 0, match.start()) + 1
    opening = text.index('{', match.end())
    end = text.index('\n}', opening) + 2
    return text[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[7])
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    kernel = args.kernel.resolve()
    source = (kernel / 'drivers/misc/mediatek/connectivity/source/common/common_main/linux/wmt_dev.c').read_text()
    integers = (kernel / 'lib/kstrtox.c').read_text()
    definition = re.search(r'^#define DYNAMIC_DUMP_BUF\s+\d+.*$', source, re.M)[0]
    start = source.index('\tcase WMT_IOCTL_DYNAMIC_DUMP_CTRL:')
    end = source.index('\tcase WMT_IOCTL_SET_ROM_PATCH_INFO:', start)
    case = source[start:end]
    helpers = ''.join(function(integers, name) for name in [
        '_parse_integer_fixup_radix', '_parse_integer', '_kstrtoull', 'kstrtoull', 'kstrtouint'])
    body = '\n'.join([STUBS, definition, helpers,
        function(source, 'wmt_dev_parse_dynamic_dump'),
        'static LONG run_dynamic_dump_ioctl(ULONG arg) { INT32 iRet = 0; switch (WMT_IOCTL_DYNAMIC_DUMP_CTRL) {',
        case, '} return iRet; }', TESTS])
    with tempfile.TemporaryDirectory(prefix='wmt-dynamic-dump-') as temporary:
        output = args.output or Path(temporary)
        if args.output:
            output.mkdir(parents=True, exist_ok=False)
        c_file = output / 'test.c'
        c_file.write_text(body)
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-sign-compare',
            '-Wno-missing-field-initializers', '-fno-omit-frame-pointer', '-no-pie',
            '-fsanitize=address,undefined', str(c_file), '-o', str(output / 'test')]
        subprocess.run(command, check=True)
        return subprocess.run([str(output / 'test')], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
