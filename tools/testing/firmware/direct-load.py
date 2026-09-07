#!/usr/bin/env python3
"""Run actual firmware filesystem loader functions against controlled I/O failures."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("scratch", type=Path)
parser.add_argument("--source", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
source = (args.source or root / "drivers/base/firmware_class.c").read_text()


def function(name):
    start = source.index("static int " + name + "(")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


prefix = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define dev_warn(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define FW_STATUS_DONE 1
struct inode { unsigned int i_mode; loff_t size; };
struct file { struct inode inode; };
struct device { int unused; };
struct firmware_buf { void *data; size_t size; const char *fw_id;
                      unsigned long status; int completion; };
static struct file fixture = {{S_IFREG | 0644, 32}};
static const char * const fw_path[] = {"", "/first", "/fallback"};
static int path_fail, vm_fail, opens, closes, path_allocs, path_frees;
static int vm_allocs, vm_frees, reads, security_calls;
static int open_at = 2, read_result = 32, security_result;
static int fw_lock;
static struct inode *file_inode(struct file *file) { return &file->inode; }
static loff_t i_size_read(struct inode *inode) { return inode->size; }
static char *__getname(void) {
    ++path_allocs; return path_fail ? NULL : malloc(PATH_MAX);
}
static void __putname(char *path) { ++path_frees; free(path); }
static void *vmalloc(size_t size) {
    ++vm_allocs; assert(size <= 32); return vm_fail ? NULL : malloc(size);
}
static void vfree(void *data) { ++vm_frees; free(data); }
static struct file *filp_open(const char *path, int flags, int mode) {
    assert(flags == O_RDONLY && mode == 0 && path[0] == '/');
    return ++opens == open_at ? &fixture : (struct file *)(intptr_t)-ENOENT;
}
static void fput(struct file *file) { assert(file == &fixture); ++closes; }
static int kernel_read(struct file *file, int offset, void *data, size_t size) {
    assert(file == &fixture && offset == 0 && size == 32); ++reads;
    if (read_result > 0) memset(data, 0xa5, read_result);
    return read_result;
}
static int security_kernel_fw_from_file(struct file *file, void *data, size_t size) {
    assert(file == &fixture && data && size == 32); ++security_calls;
    return security_result;
}
static void mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static void set_bit(int bit, unsigned long *value) { *value |= 1UL << bit; }
static void complete_all(int *completion) { ++*completion; }
'''
tests = r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *name = argv[1];
    struct device device = {0};
    struct firmware_buf buf = {.fw_id = "fixture.bin"};
    char long_name[PATH_MAX + 20];
    int expected = 0;
    if (!strcmp(name, "allocation")) { path_fail = 1; expected = -ENOMEM; }
    else if (!strcmp(name, "long-name")) {
        memset(long_name, 'x', sizeof(long_name) - 1);
        long_name[sizeof(long_name) - 1] = 0;
        buf.fw_id = long_name; expected = -ENAMETOOLONG;
    } else if (!strcmp(name, "missing")) { open_at = 0; expected = -ENOENT; }
    else if (!strcmp(name, "empty")) { fixture.inode.size = 0; expected = -EINVAL; }
    else if (!strcmp(name, "directory")) { fixture.inode.i_mode = S_IFDIR; expected = -EINVAL; }
    else if (!strcmp(name, "large-file")) {
        fixture.inode.size = (1LL << 32) + 32; expected = -EFBIG;
    } else if (!strcmp(name, "buffer-allocation")) { vm_fail = 1; expected = -ENOMEM; }
    else if (!strcmp(name, "read-error")) { read_result = -EIO; expected = -EIO; }
    else if (!strcmp(name, "short-read")) { read_result = 8; expected = -EIO; }
    else if (!strcmp(name, "eof")) { read_result = 0; expected = -EIO; }
    else if (!strcmp(name, "security")) { security_result = -EACCES; expected = -EACCES; }
    else assert(!strcmp(name, "fallback-success"));
    int result = fw_get_filesystem_firmware(&device, &buf);
    if (result != expected) {
        fprintf(stderr, "%s returned %d, expected %d\n", name, result, expected);
        return 1;
    }
    assert(path_allocs == 1 && path_frees == !path_fail && !fw_lock);
    assert(closes == (opens && open_at && opens >= open_at));
    if (expected) {
        assert(!buf.data && !buf.size && !buf.status && !buf.completion);
        assert(vm_frees == vm_allocs - vm_fail);
    } else {
        assert(opens == 2 && closes == 1 && reads == 1 && security_calls == 1);
        assert(buf.size == 32 && buf.status == (1UL << FW_STATUS_DONE));
        assert(buf.completion == 1 && !vm_frees);
        for (size_t i = 0; i < buf.size; ++i) assert(((unsigned char *)buf.data)[i] == 0xa5);
        vfree(buf.data);
    }
    if (path_fail || !strcmp(name, "long-name")) assert(!opens && !vm_allocs);
    if (!strcmp(name, "large-file")) assert(!vm_allocs && !reads && !security_calls);
    if (read_result != 32) assert(!security_calls);
    printf("PASS %s\n", name);
    return 0;
}
'''
args.scratch.mkdir(parents=True, exist_ok=True)
cases = ("allocation", "long-name", "missing", "empty", "directory", "large-file",
         "buffer-allocation", "read-error", "short-read", "eof", "security", "fallback-success")
with tempfile.TemporaryDirectory(prefix="firmware-", dir=args.scratch) as temporary:
    unit = Path(temporary) / "test.c"
    unit.write_text(prefix + function("fw_read_file_contents") +
                    function("fw_get_filesystem_firmware") + tests)
    binary = Path(temporary) / "test"
    subprocess.run(["clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-sign-compare",
                    "-fsanitize=address,undefined", str(unit), "-o", str(binary)], check=True)
    failed = [case for case in cases if subprocess.run([str(binary), case]).returncode]
    print(f"{len(cases) - len(failed)}/{len(cases)} cases pass; failures: {failed}", flush=True)
    raise SystemExit(bool(failed))
