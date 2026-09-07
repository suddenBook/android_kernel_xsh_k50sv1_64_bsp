#!/usr/bin/env python3
"""Exercise actual WMT patch ioctls, consumers and cleanup with fault injection.

The fixture extracts the production functions, record definitions and task maps.
Only kernel allocation, user copies, logging, locks and the launcher command are
host substitutes. Each case runs in a separate sanitizer process so a bad pointer
in an older kernel does not prevent the other cases from running.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


TYPES = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

typedef void VOID;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef uint8_t UINT8;
typedef uint8_t *PUINT8;
typedef void *PVOID;
typedef size_t SIZE_T;
typedef unsigned long ULONG;
typedef long LONG;
#define GFP_KERNEL 0
#define GFP_ATOMIC 1
#define BIT(index) (1UL << (index))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define DEFINE_MUTEX(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define mutex_lock(lock) pthread_mutex_lock(lock)
#define mutex_unlock(lock) pthread_mutex_unlock(lock)
#define READ_ONCE(value) __atomic_load_n(&(value), __ATOMIC_RELAXED)
#define WRITE_ONCE(value, input) __atomic_store_n(&(value), (input), __ATOMIC_RELAXED)
#define osal_memcpy memcpy
#define osal_sizeof sizeof
#define osal_snprintf snprintf
#define osal_strncmp(a, b, n) strncmp((const char *)(a), (const char *)(b), (n))
#define WMT_IOCTL_SET_PATCH_NUM 1
#define WMT_IOCTL_SET_PATCH_INFO 2
#define WMT_IOCTL_SET_ROM_PATCH_INFO 3
'''

STUBS = r'''
typedef struct {
    UINT32 patchNum, ip_ver, fw_ver;
    P_WMT_PATCH_INFO pWmtPatchInfo;
    struct wmt_rom_patch_info *pWmtRomPatchInfo[WMTDRV_TYPE_ANT];
} DEV_WMT, *P_DEV_WMT;
static DEV_WMT gDevWmt;
static int fail_alloc, fail_copy, search_error;
static atomic_int allocations, copy_calls, search_calls;

static void mock_log(const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
}
#define WMT_DBG_FUNC mock_log
#define WMT_ERR_FUNC mock_log
#define WMT_WARN_FUNC mock_log
#define WMT_INFO_FUNC mock_log
#define STP_DBG_PR_ERR mock_log
#define STP_DBG_PR_INFO mock_log
static void *kcalloc(size_t count, size_t size, int flags)
{
    (void)flags;
    if (fail_alloc) return NULL;
    void *memory = calloc(count, size);
    if (memory) atomic_fetch_add(&allocations, 1);
    return memory;
}
static void kfree(void *memory)
{
    if (memory) atomic_fetch_sub(&allocations, 1);
    free(memory);
}
static unsigned long copy_from_user(void *to, const void *from, size_t count)
{
    atomic_fetch_add(&copy_calls, 1);
    if (fail_copy) return count;
    memcpy(to, from, count);
    return 0;
}
static int wmt_ctrl_ul_cmd(P_DEV_WMT device, const UINT8 *command)
{
    (void)device;
    if (strcmp((const char *)command, "srh_rom_patch")) abort();
    atomic_fetch_add(&search_calls, 1);
    return search_error;
}
'''

TESTS = r'''
#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

static WMT_PATCH_INFO record(UINT32 sequence, unsigned char value)
{
    WMT_PATCH_INFO info;
    memset(&info, value, sizeof(info));
    info.dowloadSeq = sequence;
    info.patchName[sizeof(info.patchName) - 1] = 0;
    return info;
}
static struct wmt_rom_patch_info rom_record(UINT32 type, unsigned char value)
{
    struct wmt_rom_patch_info info;
    memset(&info, value, sizeof(info));
    info.type = type;
    info.patchName[sizeof(info.patchName) - 1] = 0;
    return info;
}
static int set_count(ULONG count) { return run_ioctl(WMT_IOCTL_SET_PATCH_NUM, count); }
static int put(WMT_PATCH_INFO *info) { return run_ioctl(WMT_IOCTL_SET_PATCH_INFO, (ULONG)info); }
static int put_rom(struct wmt_rom_patch_info *info) {
    return run_ioctl(WMT_IOCTL_SET_ROM_PATCH_INFO, (ULONG)info);
}
static int get(SIZE_T sequence, UINT8 *name, UINT8 *address)
{
    WMT_CTRL_DATA data = {0};
    data.au4CtrlData[0] = sequence;
    data.au4CtrlData[1] = (SIZE_T)name;
    data.au4CtrlData[2] = (SIZE_T)address;
    return wmt_ctrl_get_patch_info(&data);
}
static int get_rom(SIZE_T type, UINT8 *name, UINT8 *address)
{
    WMT_CTRL_DATA data = {0};
    data.au4CtrlData[0] = type;
    data.au4CtrlData[1] = (SIZE_T)name;
    data.au4CtrlData[2] = (SIZE_T)address;
    data.au4CtrlData[3] = 0x1234;
    data.au4CtrlData[4] = 0x5678;
    return wmt_ctrl_get_rom_patch_info(&data);
}
static bool matches(const UINT8 *name, const UINT8 *address, unsigned char value)
{
    for (unsigned i = 0; i < 255; i++) if (name[i] != value) return false;
    for (unsigned i = 0; i < 4; i++) if (address[i] != value) return false;
    return name[255] == 0;
}
static bool untouched(const UINT8 *name, const UINT8 *address)
{
    for (unsigned i = 0; i < 256; i++) if (name[i] != 0xcc) return false;
    for (unsigned i = 0; i < 4; i++) if (address[i] != 0xcc) return false;
    return true;
}

static int count_case(int variant)
{
    WMT_CTRL_DATA data = {0};
    ULONG invalid[] = {0, MAX_PATCH_NUM + 1, ULONG_MAX};
    if (variant < 3) {
        REQUIRE(set_count(invalid[variant]) < 0);
        REQUIRE(allocations == 0 && gDevWmt.patchNum == 0);
        REQUIRE(set_count(1) == 0);
    } else if (variant == 3 || variant == 4) {
        fail_alloc = 1;
        int result = set_count(2);
        REQUIRE(gDevWmt.patchNum == 0 && wmt_lib_get_patch_info() == NULL);
        REQUIRE(allocations == 0);
        if (variant == 3) REQUIRE(result == -ENOMEM);
        fail_alloc = 0;
        REQUIRE(set_count(2) == 0);
    } else if (variant == 5) {
        REQUIRE(set_count(2) == 0);
        REQUIRE(set_count(3) < 0);
        REQUIRE(gDevWmt.patchNum == 2 && allocations == 1);
    } else if (variant == 6) {
        REQUIRE(wmt_ctrl_get_patch_num(NULL) < 0);
    }
    REQUIRE(wmt_ctrl_get_patch_num(&data) == 0);
    REQUIRE(data.au4CtrlData[0] == gDevWmt.patchNum);
    return 0;
}

static int common_case(int variant)
{
    UINT8 name[256], address[4];
    WMT_PATCH_INFO info = record(1, 'A');
    memset(name, 0xcc, sizeof(name));
    memset(address, 0xcc, sizeof(address));
    if (variant == 0) {
        REQUIRE(put(&info) < 0);
        REQUIRE(get(1, name, address) < 0 && untouched(name, address));
        return 0;
    }
    REQUIRE(set_count(2) == 0);
    if (variant == 1) {
        fail_copy = 1;
        REQUIRE(put(&info) == -EFAULT);
        REQUIRE(wmt_lib_get_patch_info() == NULL);
        fail_copy = 0;
    } else if (variant == 2) {
        memset(info.patchName, 'A', sizeof(info.patchName));
        REQUIRE(put(&info) < 0);
        REQUIRE(wmt_lib_get_patch_info() == NULL);
        info = record(1, 'A');
    } else if (variant == 3 || variant == 4) {
        info.dowloadSeq = variant == 3 ? 0 : 3;
        REQUIRE(put(&info) < 0);
        info.dowloadSeq = 1;
    }
    REQUIRE(put(&info) == 0);
    REQUIRE(get(1, name, address) < 0 && untouched(name, address));
    if (variant == 5) {
        REQUIRE(put(&info) == 0);
        REQUIRE(wmt_lib_get_patch_info() == NULL);
        REQUIRE(get(2, name, address) < 0 && untouched(name, address));
    } else if (variant == 6) {
        info.dowloadSeq = 0;
        REQUIRE(put(&info) < 0);
    }
    info = record(2, 'B');
    REQUIRE(put(&info) == 0);
    REQUIRE(wmt_lib_get_patch_info() != NULL);
    REQUIRE(get(1, name, address) == 0 && matches(name, address, 'A'));
    REQUIRE(get(2, name, address) == 0 && matches(name, address, 'B'));
    if (variant == 7) {
        info = record(1, 'C');
        REQUIRE(put(&info) == 0);
        REQUIRE(get(1, name, address) == 0 && matches(name, address, 'C'));
        REQUIRE(get(2, name, address) == 0 && matches(name, address, 'B'));
    } else if (variant == 8) {
        memset(info.patchName, 'C', sizeof(info.patchName));
        REQUIRE(put(&info) < 0);
        REQUIRE(get(2, name, address) == 0 && matches(name, address, 'B'));
    }
    return 0;
}

static int bounds_case(int variant)
{
    WMT_PATCH_INFO info = record(1, 'A');
    UINT8 name[256], address[4];
    SIZE_T bad[] = {0, 2, UINT32_MAX, (SIZE_T)UINT32_MAX + 2, SIZE_MAX};
    REQUIRE(set_count(1) == 0 && put(&info) == 0);
    memset(name, 0xcc, sizeof(name));
    memset(address, 0xcc, sizeof(address));
    if (variant < 5) {
        REQUIRE(get(bad[variant], name, address) < 0);
        REQUIRE(untouched(name, address));
    } else if (variant == 5) REQUIRE(wmt_ctrl_get_patch_info(NULL) < 0);
    else if (variant == 6) REQUIRE(get(1, NULL, address) < 0);
    else REQUIRE(get(1, name, NULL) < 0);
    return 0;
}

static int lifecycle_case(int variant)
{
    WMT_PATCH_INFO info = record(1, 'A');
    UINT8 name[256], address[4];
    REQUIRE(set_count(MAX_PATCH_NUM) == 0);
    for (int i = MAX_PATCH_NUM; i > 0; i--) {
        info = record(i, 'A' + i);
        REQUIRE(put(&info) == 0);
        REQUIRE((wmt_lib_get_patch_info() != NULL) == (i == 1));
    }
    for (int i = 1; i <= MAX_PATCH_NUM; i++)
        REQUIRE(get(i, name, address) == 0 && matches(name, address, 'A' + i));
    if (variant) {
        wmt_dev_patch_info_free();
        REQUIRE(allocations == 0 && wmt_lib_get_patch_info() == NULL);
        REQUIRE(get(1, name, address) < 0);
        if (variant == 1) REQUIRE(gDevWmt.patchNum == 0);
        wmt_dev_patch_info_free();
        REQUIRE(set_count(1) == 0);
        info = record(1, 'Z');
        REQUIRE(put(&info) == 0);
        REQUIRE(get(1, name, address) == 0 && matches(name, address, 'Z'));
    }
    return 0;
}

static int rom_case(int variant)
{
    struct wmt_rom_patch_info info = rom_record(WMTDRV_TYPE_WMT, 'R');
    UINT8 name[256], address[4];
    memset(name, 0xcc, sizeof(name));
    memset(address, 0xcc, sizeof(address));
    if (variant == 0) {
        REQUIRE(get_rom(WMTDRV_TYPE_BT, name, address) == 1);
        REQUIRE(search_calls == 1 && untouched(name, address));
        return 0;
    } else if (variant == 1) {
        search_error = -EIO;
        REQUIRE(get_rom(WMTDRV_TYPE_WMT, name, address) < 0);
        REQUIRE(search_calls == 1 && untouched(name, address));
        return 0;
    } else if (variant == 2 || variant == 3) {
        fail_alloc = 1;
        int result = put_rom(&info);
        if (variant == 2) REQUIRE(result == -ENOMEM);
        REQUIRE(allocations == 0);
        fail_alloc = 0;
    } else if (variant == 4) {
        fail_copy = 1;
        REQUIRE(put_rom(&info) == -EFAULT);
        REQUIRE(allocations == 0);
        fail_copy = 0;
    } else if (variant == 5) {
        memset(info.patchName, 'R', sizeof(info.patchName));
        REQUIRE(put_rom(&info) < 0);
        REQUIRE(allocations == 0);
        info = rom_record(WMTDRV_TYPE_WMT, 'R');
    } else if (variant == 6 || variant == 7) {
        info.type = variant == 6 ? WMTDRV_TYPE_ANT : UINT32_MAX;
        REQUIRE(put_rom(&info) < 0 && allocations == 0);
        info.type = WMTDRV_TYPE_WMT;
    }
    REQUIRE(put_rom(&info) == 0);
    REQUIRE(get_rom(WMTDRV_TYPE_WMT, name, address) == 0);
    REQUIRE(matches(name, address, 'R') && search_calls == 0);
    if (variant == 8) {
        info = rom_record(WMTDRV_TYPE_WMT, 'S');
        REQUIRE(put_rom(&info) == 0);
        REQUIRE(get_rom(WMTDRV_TYPE_WMT, name, address) == 0);
        REQUIRE(matches(name, address, 'R') && allocations == 1);
    } else if (variant == 9) {
        for (int type = 0; type < WMTDRV_TYPE_ANT; type++) {
            if (type == WMTDRV_TYPE_WMT) continue;
            info = rom_record(type, 'A' + type);
            REQUIRE(put_rom(&info) == 0);
            REQUIRE(get_rom(type, name, address) == 0 && matches(name, address, 'A' + type));
        }
        REQUIRE(allocations == WMTDRV_TYPE_ANT);
        wmt_lib_rom_patch_info_free();
        REQUIRE(allocations == 0);
        REQUIRE(get_rom(WMTDRV_TYPE_WMT, name, address) == 1);
        info = rom_record(WMTDRV_TYPE_WMT, 'Z');
        REQUIRE(put_rom(&info) == 0);
        REQUIRE(get_rom(WMTDRV_TYPE_WMT, name, address) == 0 && matches(name, address, 'Z'));
    }
    return 0;
}

static int rom_bounds_case(int variant)
{
    UINT8 name[256], address[4];
    SIZE_T bad[] = {WMTDRV_TYPE_ANT, UINT32_MAX, (SIZE_T)UINT32_MAX + 1, SIZE_MAX};
    memset(name, 0xcc, sizeof(name));
    memset(address, 0xcc, sizeof(address));
    if (variant < 4) {
        REQUIRE(get_rom(bad[variant], name, address) < 0);
        REQUIRE(untouched(name, address));
    } else if (variant == 4) REQUIRE(wmt_ctrl_get_rom_patch_info(NULL) < 0);
    else if (variant == 5) REQUIRE(get_rom(WMTDRV_TYPE_WMT, NULL, address) < 0);
    else REQUIRE(get_rom(WMTDRV_TYPE_WMT, name, NULL) < 0);
    REQUIRE(search_calls == 0 && gDevWmt.ip_ver == 0 && gDevWmt.fw_ver == 0);
    return 0;
}

static int task_case(int variant)
{
    if (!variant) {
        REQUIRE(stp_dbg_soc_id_to_task(0) == NULL);
        return 0;
    }
    WMT_PATCH_INFO info = record(1, 'A');
    const char *prefixes[] = {"", "ROMv2", "ROMv3", "ROMv4", "CONNAC"};
    const int expected_two[] = {0,1,2,3,4,8,0,0,0,9,10,11,12,13};
    const int expected_three[] = {0,1,2,3,4,5,6,8,0,9,10,11,12,13};
    const char *expected_names[] = {
        "Task_WMT", "Task_BT", "Task_Wifi", "Task_Tst", "Task_FM",
        "Task_GPS", "Task_FLP", "Task_BT2", "Task_Idle", "Task_DrvStp",
        "Task_DrvBtif", "Task_NatBt", "Task_DrvWifi", "Task_GPS"
    };
    strcpy((char *)info.patchName, prefixes[variant]);
    REQUIRE(set_count(1) == 0 && put(&info) == 0);
    for (unsigned i = 0; i < ARRAY_SIZE(expected_names); i++) {
        unsigned id = variant == 1 ? expected_two[i] :
                      variant < 4 ? expected_three[i] : i;
        const char *result = (const char *)stp_dbg_soc_id_to_task(i);
        REQUIRE(result && strcmp(result, expected_names[id]) == 0);
    }
    REQUIRE(stp_dbg_soc_id_to_task(STP_DBG_TASK_ID_MAX) == NULL);
    REQUIRE(stp_dbg_soc_id_to_task(UINT32_MAX) == NULL);
    wmt_dev_patch_info_free();
    REQUIRE(stp_dbg_soc_id_to_task(0) == NULL);
    return 0;
}

static atomic_int writer_done, concurrent_failure;
static void *writer(void *argument)
{
    int variant = *(int *)argument;
    for (int i = 0; i < 4000; i++) {
        unsigned char value = 'A' + (i & 1);
        int result;
        if (variant == 2) {
            struct wmt_rom_patch_info info = rom_record(WMTDRV_TYPE_WMT, value);
            wmt_lib_rom_patch_info_free();
            result = put_rom(&info);
        } else {
            WMT_PATCH_INFO info = record(1, value);
            if (variant == 1) {
                wmt_dev_patch_info_free();
                if (set_count(1)) atomic_store(&concurrent_failure, 1);
            }
            result = put(&info);
        }
        if (result) atomic_store(&concurrent_failure, 1);
        if ((i % 10) == 0) sched_yield();
    }
    atomic_store(&writer_done, 1);
    return NULL;
}
static int concurrent_case(int variant)
{
    UINT8 name[256], address[4];
    WMT_PATCH_INFO info = record(1, 'A');
    if (variant != 2) REQUIRE(set_count(1) == 0 && put(&info) == 0);
    pthread_t thread;
    REQUIRE(pthread_create(&thread, NULL, writer, &variant) == 0);
    unsigned samples = 0;
    do {
        int result = variant == 2 ? get_rom(WMTDRV_TYPE_WMT, name, address) :
                                   get(1, name, address);
        if (result && (variant == 0 || result != (variant == 2 ? 1 : -ENOENT)))
            atomic_store(&concurrent_failure, 1);
        if (!result && !matches(name, address, 'A') && !matches(name, address, 'B'))
            atomic_store(&concurrent_failure, 1);
        samples++;
    } while (!atomic_load(&writer_done) || samples < 10000);
    REQUIRE(pthread_join(thread, NULL) == 0);
    REQUIRE(!atomic_load(&concurrent_failure));
    REQUIRE(allocations == 1);
    REQUIRE((variant == 2 ? get_rom(WMTDRV_TYPE_WMT, name, address) :
                           get(1, name, address)) == 0);
    REQUIRE(matches(name, address, 'B'));
    printf("coherent read attempts: %u\n", samples);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    int variant = atoi(argv[2]);
    if (!strcmp(argv[1], "count")) return count_case(variant);
    if (!strcmp(argv[1], "common")) return common_case(variant);
    if (!strcmp(argv[1], "bounds")) return bounds_case(variant);
    if (!strcmp(argv[1], "lifecycle")) return lifecycle_case(variant);
    if (!strcmp(argv[1], "rom")) return rom_case(variant);
    if (!strcmp(argv[1], "rom-bounds")) return rom_bounds_case(variant);
    if (!strcmp(argv[1], "task")) return task_case(variant);
    if (!strcmp(argv[1], "concurrent")) return concurrent_case(variant);
    return 2;
}
'''


CASES = {
    'count': ['zero', 'too-many', 'word-max', 'allocation-errno', 'allocation-retry',
              'duplicate-preserves-count', 'null-output'],
    'common': ['before-count', 'copy-failure', 'unterminated-name', 'zero-sequence',
               'high-sequence', 'duplicate-does-not-publish-holes',
               'invalid-sequence-preserves-progress', 'replace-complete-record',
               'invalid-update-preserves-record'],
    'bounds': ['zero', 'past-end', 'u32-max', 'u64-wrap', 'word-max',
               'null-control', 'null-name', 'null-address'],
    'lifecycle': ['out-of-order-all-ten', 'free-resets-count', 'free-reallocate'],
    'rom': ['optional-missing', 'launcher-failure', 'allocation-errno',
            'allocation-retry', 'copy-failure', 'unterminated-name', 'invalid-type',
            'word-max-type', 'duplicate-preserves-first', 'all-types-free-reallocate'],
    'rom-bounds': ['past-end', 'u32-max', 'u64-wrap', 'word-max',
                  'null-control', 'null-name', 'null-address'],
    'task': ['metadata-absent', 'rom2', 'rom3', 'rom4', 'connac'],
    'concurrent': ['update-read', 'free-reallocate-read', 'rom-free-reallocate-read'],
}


def function(source, name):
    """Extract a definition, ignoring prototypes and call sites."""
    pattern = r'(?m)^[A-Za-z_][^;\n]*\b' + re.escape(name) + r'\([^;{}]*\)\n\{'
    match = re.search(pattern, source)
    if not match:
        return ''
    end = source.index('\n}', match.end()) + 2
    return source[match.start():end] + '\n'


def capture(source, pattern):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise ValueError(f'production declaration not found: {pattern}')
    return match[0] + '\n'


def fixture(kernel):
    root = kernel / 'drivers/misc/mediatek/connectivity/source/common/common_main'
    dev = (root / 'linux/wmt_dev.c').read_text()
    lib = (root / 'core/wmt_lib.c').read_text()
    ctrl = (root / 'core/wmt_ctrl.c').read_text()
    dbg = (root / 'linux/stp_dbg_soc.c').read_text()
    header = (root / 'core/include/wmt_lib.h').read_text()
    definitions = capture(header, r'^#define MAX_PATCH_NUM[^\n]*')
    definitions += capture(header, r'^typedef struct \{\n\tUINT32 dowloadSeq;.*?WMT_PATCH_INFO;')
    definitions += capture(header, r'^struct wmt_rom_patch_info \{.*?^\};')
    definitions += capture((root / 'include/wmt_exp.h').read_text(),
                           r'^typedef enum _ENUM_WMTDRV_TYPE_T \{.*?P_ENUM_WMTDRV_TYPE_T;')
    ctrl_header = (root / 'core/include/wmt_ctrl.h').read_text()
    definitions += capture(ctrl_header, r'^#define DWCNT_CTRL_DATA[^\n]*')
    definitions += capture(ctrl_header,
                           r'^typedef struct _WMT_CTRL_DATA_ \{.*?P_WMT_CTRL_DATA;')
    definitions += capture((root / 'linux/include/stp_dbg.h').read_text(),
                           r'^enum STP_DBG_TAKS_ID_T \{.*?^\};')
    globals_ = '\n'.join(re.findall(
        r'^(?:static )?(?:P_WMT_PATCH_INFO pPatchInfo|UINT32 pAtchNum|'
        r'DEFINE_MUTEX\(g_patch_info_lock\)|unsigned long g_patch_info_seen|'
        r'bool g_patch_info_ready);$', dev, re.M)) + '\n'
    globals_ += '\n'.join(re.findall(r'^static DEFINE_MUTEX\(g_rom_patch_info_lock\);$', lib, re.M))
    helpers = ''.join(function(lib, name) for name in [
        'wmt_lib_set_patch_num', 'wmt_lib_set_patch_info', 'wmt_lib_get_patch_info',
        'wmt_lib_set_rom_patch_info', 'wmt_lib_get_rom_patch_info',
        'wmt_lib_rom_patch_info_free'])
    if not function(lib, 'wmt_lib_rom_patch_info_free'):
        # Execute the actual older teardown loop, with its local device pointer.
        loop = capture(function(lib, 'wmt_lib_deinit'),
                       r'^\tfor \(i = 0; i < WMTDRV_TYPE_ANT; i\+\+\) \{.*?^\t\}')
        helpers += ('static void wmt_lib_rom_patch_info_free(void) {\n'
                    'int i; P_DEV_WMT pDevWmt = &gDevWmt;\n' + loop + '\n}\n')
    helpers += ''.join(function(dev, name) for name in [
        'wmt_dev_patch_info_free', 'wmt_dev_set_patch_num', 'wmt_dev_set_patch_info',
        'wmt_dev_get_patch_info'])
    helpers += ''.join(function(ctrl, name) for name in [
        'wmt_ctrl_get_patch_num', 'wmt_ctrl_get_patch_info', 'wmt_ctrl_get_rom_patch_info'])
    maps = '\n'.join(re.findall(r'^#define ROM_V[234]_PATCH[^\n]*', dbg, re.M)) + '\n'
    for symbol in ['soc_task_str', 'soc_gen_two_task_id_adapter', 'soc_gen_three_task_id_adapter']:
        maps += capture(dbg, r'^[^\n]*\b' + symbol + r'\[STP_DBG_TASK_ID_MAX\] = \{.*?^\};')
    helpers += maps + function(dbg, 'stp_dbg_soc_id_to_task')
    cases = ''
    for name in ['SET_PATCH_NUM', 'SET_PATCH_INFO', 'SET_ROM_PATCH_INFO']:
        start = dev.index('\tcase WMT_IOCTL_' + name + ':')
        end = dev.index('\tcase ', start + 1)
        cases += dev[start:end]
    ioctl = ('static LONG run_ioctl(UINT32 cmd, ULONG arg) { INT32 iRet = 0;\n'
             'switch (cmd) {\n' + cases + '\n} return iRet; }\n')
    return '\n'.join([TYPES, definitions, STUBS, globals_, helpers, ioctl, TESTS])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[7])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--sanitizer', choices=['address', 'thread'], default='address')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='wmt-patch-info-') as temporary:
        output = args.output or Path(temporary)
        if args.output:
            output.mkdir(parents=True, exist_ok=False)
        c_file = output / 'test.c'
        c_file.write_text(fixture(args.kernel.resolve()))
        sanitizer = 'address,undefined' if args.sanitizer == 'address' else 'thread'
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-pointer-sign',
            '-Wno-sign-compare', '-Wno-unused-but-set-variable', '-Wno-unused-function',
            '-fno-omit-frame-pointer', '-no-pie', '-pthread', f'-fsanitize={sanitizer}',
            str(c_file), '-o', str(output / 'test')]
        subprocess.run(command, check=True)
        results = []
        # Objects intentionally remain live at process exit in non-teardown cases.
        environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', TSAN_OPTIONS='halt_on_error=1')
        for group, names in CASES.items():
            if args.sanitizer == 'thread' and group != 'concurrent':
                continue
            for variant, name in enumerate(names):
                run = subprocess.run([str(output / 'test'), group, str(variant)],
                                     text=True, capture_output=True, env=environment, timeout=30)
                case = f'{group}/{name}'
                (output / f'{group}-{name}.log').write_text(run.stdout + run.stderr)
                results.append({'case': case, 'exit_code': run.returncode})
                print(f'{"PASS" if run.returncode == 0 else "FAIL"} {case}', flush=True)
        passed = sum(item['exit_code'] == 0 for item in results)
        print(f'{passed}/{len(results)} passed', flush=True)
        (output / 'result.json').write_text(json.dumps({
            'kernel': str(args.kernel.resolve()), 'sanitizer': sanitizer,
            'fixture_sha256': hashlib.sha256(c_file.read_bytes()).hexdigest(),
            'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'compile_command': command,
            'passed': passed, 'total': len(results), 'cases': results,
        }, indent=2) + '\n')
        return passed != len(results)


if __name__ == '__main__':
    raise SystemExit(main())
