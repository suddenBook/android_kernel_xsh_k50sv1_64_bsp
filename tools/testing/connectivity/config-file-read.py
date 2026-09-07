#!/usr/bin/env python3
"""Check actual WLAN config-file reads for errors, termination and search paths."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("scratch", type=Path)
parser.add_argument("--root", type=Path)
args = parser.parse_args()
root = args.root or Path(__file__).resolve().parents[3]
wlan = root / "drivers/misc/mediatek/connectivity/source/wlan/core/gen2"


def block(text, start):
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


kal = (wlan / "os/linux/gl_kal.c").read_text()
reader = block(kal, kal.index("INT_32 kalReadToFile("))
init = (wlan / "os/linux/gl_init.c").read_text()
anchor = init.index("wlanCfgInit(prAdapter, NULL, 0, 0);")
config = block(init, init.index("{", anchor))
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t *PUINT_8;
typedef uint32_t UINT_32;
typedef int32_t INT_32;
typedef UINT_32 *PUINT_32;
#define DBGLOG(...) ((void)0)
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((intptr_t)(p))
#define WLAN_CFG_FILE_BUF_SIZE 2048
#define VIR_MEM_TYPE 0
#define kalMemZero(p, n) memset(p, 0, n)
struct file { int unused; };
static struct file fixture;
static int opens, closes, reads, parses, no_file, read_error, empty, alloc_fail;
static size_t allocated;
static char first_path[256];
static struct file *kalFileOpen(const unsigned char *path, int flags, int mode) {
    assert(flags == O_RDONLY && mode == 0);
    if (!opens++) snprintf(first_path, sizeof(first_path), "%s", path);
    return no_file ? NULL : &fixture;
}
static void kalFileClose(struct file *file) { assert(file == &fixture); ++closes; }
static UINT_32 kalFileRead(struct file *file, unsigned long offset, PUINT_8 data, UINT_32 size) {
    assert(file == &fixture && offset == 0); ++reads;
    if (read_error) return -EIO;
    if (empty) return 0;
    memset(data, 'x', size); return size;
}
static void *kalMemAlloc(size_t size, int type) {
    assert(size >= WLAN_CFG_FILE_BUF_SIZE && size <= WLAN_CFG_FILE_BUF_SIZE + 1 && !type);
    allocated = size;
    return alloc_fail ? NULL : malloc(size);
}
static void kalMemFree(void *data, int type, size_t size) {
    assert(!type && size == allocated); free(data);
}
static void wlanCfgInit(void *adapter, PUINT_8 data, UINT_32 size, int flags) {
    assert(adapter && !flags && size > 0);
    /* The production parser also requires a NUL-terminated input string. */
    assert(strlen((char *)data) == size && size <= WLAN_CFG_FILE_BUF_SIZE);
    ++parses;
}
'''
tests = r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *name = argv[1];
    if (!strncmp(name, "config-", 7)) {
        no_file = !strcmp(name, "config-missing");
        alloc_fail = !strcmp(name, "config-allocation");
        read_error = !strcmp(name, "config-read-error");
        load_config(&fixture);
        assert(parses == !(no_file || alloc_fail || read_error));
        if (alloc_fail) assert(!opens && !reads && !closes);
        else {
            assert(opens == 1 && !strcmp(first_path, "/vendor/firmware/wifi.cfg"));
            assert(reads == !no_file && closes == !no_file);
        }
    } else {
        unsigned char data[16]; UINT_32 size = 1234;
        read_error = !strcmp(name, "read-error");
        no_file = !strcmp(name, "missing");
        empty = !strcmp(name, "empty");
        int expected = read_error ? -EIO : no_file ? -ENOENT : 0;
        int result = kalReadToFile((PUINT_8)"/fixture", data, sizeof(data), &size);
        assert(result == expected);
        assert(size == (expected || empty ? 0 : sizeof(data)));
        assert(opens == 1 && reads == !no_file && closes == !no_file);
        if (!expected && !empty) assert(!memcmp(data, "xxxxxxxxxxxxxxxx", sizeof(data)));
        assert(kalReadToFile((PUINT_8)"/fixture", data, sizeof(data), NULL) == expected);
    }
    printf("PASS %s\n", name);
}
'''
args.scratch.mkdir(parents=True, exist_ok=True)
cases = ("full-read", "read-error", "missing", "empty", "config-full",
         "config-missing", "config-allocation", "config-read-error")
with tempfile.TemporaryDirectory(prefix="wlan-config-", dir=args.scratch) as temporary:
    unit = Path(temporary) / "test.c"
    unit.write_text(prefix + reader + "\nstatic void load_config(void *prAdapter) " + config + tests)
    binary = Path(temporary) / "test"
    subprocess.run(["clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-pointer-sign", "-fsanitize=address,undefined",
                    str(unit), "-o", str(binary)], check=True)
    failed = [case for case in cases if subprocess.run([str(binary), case]).returncode]
    print(f"{len(cases) - len(failed)}/{len(cases)} cases pass; failures: {failed}", flush=True)
    raise SystemExit(bool(failed))
