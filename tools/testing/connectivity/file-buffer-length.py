#!/usr/bin/env python3
"""Exercise the real WR-BUF dispatch branch with a length-recording callback."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("scratch", type=Path)
parser.add_argument("--source", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
source = args.source or root / "drivers/misc/mediatek/connectivity/source/wlan/adaptor/wmt_cdev_wifi.c"
text = source.read_text()
start = text.index('} else if (!strncmp(local, "WR-BUF:", 7)) {')
end = text.index('} else if (local[0] == \'S\'', start)
branch = text[start:end].removeprefix('} else ') + '}\n'
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define ENOTSUPP 524
#define WIFI_INFO_FUNC(...) ((void)0)
#define WIFI_ERR_FUNC(...) ((void)0)
typedef int (*file_buf_handler)(void *, const char *, uint16_t);
enum { BUF_TYPE_NVRAM, BUF_TYPE_DRV_CFG, BUF_TYPE_FW_CFG };
static unsigned int calls, waits;
static uint16_t delivered;
static const char *delivered_buffer;
static void *delivered_context;
static int handler_result;
static int record(void *context, const char *buffer, uint16_t length) {
    ++calls; delivered = length; delivered_buffer = buffer; delivered_context = context;
    return handler_result;
}
static file_buf_handler buf_handler[3] = {record, record, record};
static void *buf_handler_ctx[3] = {(void *)1, (void *)2, (void *)3};
static void msleep(unsigned int ms) { assert(ms == 100); ++waits; }
static int run(const char *buf, size_t count) {
    char local[20] = {0};
    int copy_size = count < sizeof(local) ? count : sizeof(local) - 1;
    int retval = -EIO, wait_cnt = 0;
    if (!count) goto done;
    memcpy(local, buf, copy_size);
'''
tests = r'''
done:
    return retval;
}
int main(void) {
    const char *names[] = {"NVRAM", "DRVCFG", "FWCFG"};
    const size_t lengths[] = {1, 256, 65535};
    char command[64];
    for (unsigned int kind = 0; kind < 3; ++kind) {
        memset(command, 0, sizeof(command));
        size_t prefix_length = snprintf(command, sizeof(command), "WR-BUF:%s", names[kind]);
        for (unsigned int i = 0; i < 3; ++i) {
            calls = waits = 0;
            assert(run(command, prefix_length + lengths[i]) == (int)(prefix_length + lengths[i]));
            assert(calls == 1 && delivered == lengths[i]);
            assert(delivered_buffer == command + prefix_length);
            assert(delivered_context == buf_handler_ctx[kind]);
        }
        calls = waits = 0;
        for (size_t excess = 65536; excess <= 131072; excess += 65536) {
            assert(run(command, prefix_length + excess) == -EMSGSIZE);
            assert(!calls && !waits);
        }
        assert(run(command, prefix_length) == -EINVAL);
        assert(!calls && !waits);
        printf("PASS %s exact lengths, empty and oversized payloads\n", names[kind]);
    }
    for (unsigned int kind = 1; kind < 3; ++kind) {
        size_t length = snprintf(command, sizeof(command), "RM-BUF:%s", names[kind]);
        calls = 0;
        assert(run(command, length) == (int)length);
        assert(calls == 1 && delivered == 0 && delivered_buffer == NULL);
    }
    puts("PASS RM-BUF retains zero-length removal");
    memset(command, 0, sizeof(command)); strcpy(command, "WR-BUF:NVRAM");
    calls = waits = 0; buf_handler[0] = NULL;
    assert(run(command, 268) == -ENOTSUPP && !calls && waits == 20);
    buf_handler[0] = record; handler_result = -1;
    assert(run(command, 268) == -ENOTSUPP && calls == 1);
    puts("PASS absent and failing callback error behavior");
}
'''
args.scratch.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="file-buffer-", dir=args.scratch) as output:
    directory = Path(output)
    unit = directory / "test.c"
    unit.write_text(prefix + branch + tests)
    executable = directory / "test"
    subprocess.run(["clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(unit), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
