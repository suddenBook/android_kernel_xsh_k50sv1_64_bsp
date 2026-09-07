#!/usr/bin/env python3
"""Exercise the actual WMT debug reader with guarded host buffers and sanitizers."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


COMMON = Path(__file__).resolve().parents[1]

STUBS = r'''
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define __user
static pthread_mutex_t g_wmt_coex_lock = PTHREAD_MUTEX_INITIALIZER;
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
#define WMT_INFO_FUNC(...) ((void)0)
#define WMT_ERR_FUNC(...) ((void)0)
#define osal_sprintf sprintf
#define osal_strlen strlen
#define osal_sizeof sizeof
#define min_t(type, a, b) ({ type _a = (a); type _b = (b); _a < _b ? _a : _b; })
typedef int32_t INT32;
typedef uint8_t UINT8;
typedef char INT8;
typedef char *PINT8;
struct file { int unused; };
static size_t requested, copied;
static int copy_calls, fail_copy;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s (requested=%zu, copied=%zu)\n", message, requested, copied);
        exit(EXIT_FAILURE);
    }
}

static unsigned long copy_to_user(void *to, const void *from, size_t count)
{
    copy_calls++;
    copied = count;
    check(count <= requested, "copy exceeds read count");
    check(count <= 512, "copy exceeds local formatted buffer");
    if (fail_copy)
        return count;
    memcpy(to, from, count);
    return 0;
}
'''

TESTS = r'''
static void run_case(size_t count, int available, loff_t position, int fault)
{
    unsigned char guarded[32 + 512 + 32];
    char expected[1024];
    char *user = (char *)guarded + 32;
    loff_t pos = position;
    ssize_t ret;
    size_t i, length = 0, want;

    memset(guarded, 0xa5, sizeof(guarded));
    for (i = 0; i < sizeof(gCoexBuf.buffer); i++)
        gCoexBuf.buffer[i] = i;
    gCoexBuf.availSize = available;
    requested = count;
    copied = 0;
    copy_calls = 0;
    fail_copy = fault;
    ret = wmt_dbg_read(NULL, user, count, &pos);

    if (!count || position) {
        check(ret == 0 && pos == position && copy_calls == 0, "empty/EOF read changed output");
        if (!count)
            check(gCoexBuf.availSize == available, "zero-length read consumed a reply");
    } else if (fault) {
        check(ret == -EFAULT && pos == position, "copy fault changed position or errno");
        check(gCoexBuf.availSize == available, "copy fault consumed a reply");
    } else {
        if (available > 0) {
            /* Build the full expected reply independently, then limit its visible prefix. */
            for (i = 0; i < (size_t)available && i < sizeof(gCoexBuf.buffer); i++)
                length += sprintf(expected + length, "0x%02x ", gCoexBuf.buffer[i]);
            /* The 512-byte formatter holds at most 102 tokens and the newline/NUL. */
            if (length > 510)
                length = 510;
            expected[length++] = '\n';
        } else {
            strcpy(expected, "no data available, please run echo 15 xx > /proc/driver/wmt_psm first\n");
            length = strlen(expected) + 1;
        }
        want = count < length ? count : length;
        check(ret == (ssize_t)want && pos == (loff_t)want, "read length/position mismatch");
        check(copied == want && !memcmp(user, expected, want), "read content mismatch");
        check(gCoexBuf.availSize == 0, "successful one-shot read did not consume reply");
    }
    want = ret > 0 ? (size_t)ret : 0;
    for (i = 0; i < 32; i++)
        check(guarded[i] == 0xa5, "leading user-buffer guard changed");
    for (i = 32 + want; i < sizeof(guarded); i++)
        check(guarded[i] == 0xa5, "bytes beyond returned data changed");
}

int main(int argc, char **argv)
{
    static const size_t counts[] = {0, 1, 2, 5, 6, 20, 21, 41, 66, 510, 511, 512, 4096,
                                   INT_MAX, SIZE_MAX};
    static const int available[] = {-1, 0, 1, 4, 8, 100, 128, 129, INT_MAX};
    size_t i, j;

    if (argc == 2 && !strcmp(argv[1], "short")) {
        run_case(1, 4, 0, 0);
        return EXIT_SUCCESS;
    }
    for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
        for (j = 0; j < sizeof(available) / sizeof(available[0]); j++) {
            run_case(counts[i], available[j], 0, 0);
            run_case(counts[i], available[j], 0, 1);
            run_case(counts[i], available[j], 1, 0);
        }
    puts("PASS: WMT debug read bounds, zero-count/EOF, copy faults, and reply contents");
    return EXIT_SUCCESS;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="read driver sources at a git revision")
    parser.add_argument("--case", choices=("all", "short"), default="all")
    args = parser.parse_args()
    git_root = Path(subprocess.check_output(
        ["git", "-C", str(COMMON), "rev-parse", "--show-toplevel"], text=True
    ).strip())

    def read(relative):
        path = COMMON / relative
        if args.revision:
            return subprocess.check_output([
                "git", "-C", str(git_root), "show",
                args.revision + ":" + str(path.relative_to(git_root)),
            ], text=True)
        return path.read_text()

    body = re.search(r"^ssize_t wmt_dbg_read\(.*?^\}", read("common_main/linux/wmt_dbg.c"),
                     re.MULTILINE | re.DOTALL).group(0)
    coex = re.search(r"typedef struct _COEX_BUF \{.*?\} COEX_BUF, \*P_COEX_BUF;",
                     read("common_main/linux/include/wmt_dbg.h"), re.DOTALL).group(0)
    with tempfile.TemporaryDirectory(prefix="wmt-debug-read-") as work:
        cfile, binary = Path(work) / "read.c", Path(work) / "read"
        cfile.write_text(STUBS + coex + "\nstatic COEX_BUF gCoexBuf;\n" + body + TESTS)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wno-sign-compare", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", str(cfile), "-o", str(binary),
        ], check=True)
        return subprocess.run([str(binary), args.case], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
