#!/usr/bin/env python3
"""Run extracted driver functions with scripted kernel and bus boundaries.

The fixtures compile production function bodies as GNU C with ASan/UBSan.
They do not build the kernel, emulate I2C timing, or touch a device. --revision
can demonstrate the same contract failures against an earlier git revision.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


BASE = "drivers/misc/mediatek/sensors-1.0/accelerometer/"


def function(source, name):
    pattern = rf"(?m)^[\w *]+\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{"
    match = re.search(pattern, source)
    if match is None:
        raise ValueError(f"Function not found: {name}")
    start = match.start()
    depth = 1
    pos = match.end()
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


def macro(source, name):
    match = re.search(rf"(?m)^#\s*define {re.escape(name)}\b[^\n]*", source)
    if match is None:
        raise ValueError(f"Macro not found: {name}")
    start, end = match.span()
    while source[end - 1] == "\\":
        end = source.index("\n", end + 1)
    return source[start:end]


HEADERS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#define MI_ERR(...) do {} while (0)
#define MI_MSG(...) do {} while (0)
#define MI_DATA(...) do {} while (0)
#define ERESTARTSYS 512
typedef void *PLAT_HANDLE;
typedef void *MIR_HANDLE;
typedef uint8_t u8;
struct i2c_client { void *data; };
"""


def smbus_fixture(cust, core):
    boundary = r"""
static int read_result, block_result, write_count;
static u8 written_value;
static int i2c_smbus_read_byte_data(struct i2c_client *client, u8 addr)
{ (void)client; (void)addr; return read_result; }
static int i2c_smbus_read_i2c_block_data(struct i2c_client *client, u8 addr,
                                      u8 count, u8 *data)
{ (void)client; (void)addr; (void)count; (void)data; return block_result; }
static int i2c_smbus_write_byte_data(struct i2c_client *client, u8 addr, u8 data)
{ (void)client; (void)addr; ++write_count; written_value = data; return 0; }
"""
    adapter = "\n".join(function(cust, name) for name in (
        "i2c_smbus_read", "i2c_smbus_read_block", "i2c_smbus_write"))
    dispatch = r"""
#define MIR3DA_REG_ADDR(reg) ((reg) & 0xff)
struct general_ops {
    struct {
        int (*read)(PLAT_HANDLE, u8, u8 *);
        int (*read_block)(PLAT_HANDLE, u8, u8, u8 *);
        int (*write)(PLAT_HANDLE, u8, u8);
    } smi;
};
static struct general_ops ops = {
    .smi = { i2c_smbus_read, i2c_smbus_read_block, i2c_smbus_write }
};
static struct { struct general_ops *method; } mir3da_gsensor_drv = { &ops };
"""
    registers = "\n".join(function(core, name) for name in (
        "mir3da_register_read", "mir3da_register_read_continuously",
        "mir3da_register_write", "mir3da_register_mask_write"))
    tests = r"""
int main(void)
{
    struct i2c_client client = { 0 };
    u8 byte = 0xa5;
    read_result = -EREMOTEIO;
    assert(i2c_smbus_read(&client, 0x11, &byte) == -EREMOTEIO);
    assert(byte == 0xa5);
    assert(mir3da_register_mask_write(&client, 0x11, 0x0f, 0x05) == -EREMOTEIO);
    assert(write_count == 0);
    read_result = 0xff;
    assert(i2c_smbus_read(&client, 0x11, &byte) == 0 && byte == 0xff);
    assert(mir3da_register_mask_write(&client, 0x11, 0x0f, 0x05) == 0);
    assert(write_count == 1 && written_value == 0xf5);
    read_result = 0;
    assert(i2c_smbus_read(&client, 0x11, &byte) == 0 && byte == 0);
    block_result = 6;
    assert(i2c_smbus_read_block(&client, 0x02, 6, &byte) == 6);
    assert(mir3da_register_read_continuously(&client, 0x02, 6, &byte) == 0);
    block_result = 4;
    assert(mir3da_register_read_continuously(&client, 0x02, 6, &byte) != 0);
    block_result = -EREMOTEIO;
    assert(mir3da_register_read_continuously(&client, 0x02, 6, &byte) != 0);
    puts("PASS: bus error leaves output intact and prevents register write; block counts unchanged");
}
"""
    return HEADERS + boundary + adapter + dispatch + registers + tests


SAMPLING_BOUNDARY = r"""
#define MIR3DA_AXIS_X 0
#define MIR3DA_AXIS_Y 1
#define MIR3DA_AXIS_Z 2
#define MIR3DA_AXES_NUM 3
#define MIR3DA_BUFSIZE 256
#define MIR3DA_STK_TEMP_SOLUTION 1
#define GRAVITY_EARTH_1000 9807
#define SENSOR_STATUS_ACCURACY_MEDIUM 2
struct mir3da_i2c_data {
    struct { int sign[3], map[3]; } cvt;
    short cali_sw[4], data[4];
};
static struct mir3da_i2c_data device;
static struct i2c_client client = { &device };
static struct i2c_client *mir3da_i2c_client = &client;
static struct { int x, y, z; } gsensor_gain = { 1024, 1024, 1024 };
static bool sensor_power;
static int bzstk;
static int read_result, power_result, lock_result;
static int read_count, power_count, lock_count, unlock_count, sleep_count;
static int clientdata_count, s_tSemaProtect;
static void *i2c_get_clientdata(struct i2c_client *ptr)
{ ++clientdata_count; assert(ptr); return ptr->data; }
static int down_interruptible(int *semaphore)
{ (void)semaphore; ++lock_count; return lock_result; }
static void up(int *semaphore)
{ (void)semaphore; ++unlock_count; assert(!lock_result); }
static void msleep(int delay)
{ assert(delay == 20); ++sleep_count; }
static int mir3da_setPowerMode(struct i2c_client *ptr, bool on)
{
    assert(ptr == &client && on);
    ++power_count;
    if (!power_result) sensor_power = on;
    return power_result;
}
static int mir3da_read_data(struct i2c_client *ptr, short *x, short *y, short *z)
{
    assert(ptr == &client);
    ++read_count;
    if (read_result) return read_result;
    *x = 100; *y = 200; *z = 1024;
    return 0;
}
static void reset(void)
{
    memset(&device, 0, sizeof(device));
    /* Direction 5 from hwmon/hwmsen/hwmsen_helper.c. */
    device.cvt.sign[0] = 1; device.cvt.sign[1] = 1; device.cvt.sign[2] = -1;
    device.cvt.map[0] = 1; device.cvt.map[1] = 0; device.cvt.map[2] = 2;
    device.cali_sw[0] = 10; device.cali_sw[1] = -20;
    client.data = &device;
    sensor_power = true;
    read_result = power_result = lock_result = 0;
    read_count = power_count = lock_count = unlock_count = sleep_count = 0;
    clientdata_count = 0;
}
"""


def sampling_fixture(cust, core):
    production = "\n".join((
        function(core, "squareRoot"),
        function(cust, "mir3da_mutex_lock"),
        function(cust, "mir3da_mutex_unlock"),
        function(cust, "mir3da_readSensorData"),
        function(cust, "mir3da_get_data"),
    ))
    tests = r"""
int main(int argc, char **argv)
{
    int x = 11, y = 22, z = 33, status = 44;
    char buffer[MIR3DA_BUFSIZE] = "previous sample";
    assert(argc == 2);
    reset();
    if (!strcmp(argv[1], "read")) {
        read_result = -EREMOTEIO;
        assert(mir3da_get_data(&x, &y, &z, &status) == -EREMOTEIO);
        assert(x == 11 && y == 22 && z == 33 && status == 44);
        assert(read_count == 1 && lock_count == 1 && unlock_count == 1);
        assert(mir3da_readSensorData(&client, buffer) == -EREMOTEIO);
        assert(buffer[0] == '\0');
    } else if (!strcmp(argv[1], "lock")) {
        lock_result = -EINTR;
        assert(mir3da_get_data(&x, &y, &z, &status) == -ERESTARTSYS);
        assert(read_count == 0 && unlock_count == 0);
        assert(x == 11 && y == 22 && z == 33 && status == 44);
    } else if (!strcmp(argv[1], "power")) {
        sensor_power = false;
        power_result = -EIO;
        assert(mir3da_get_data(&x, &y, &z, &status) == -EIO);
        assert(power_count == 1 && read_count == 0 && sleep_count == 0);
        assert(unlock_count == 1 && status == 44);
    } else if (!strcmp(argv[1], "null")) {
        assert(mir3da_readSensorData(NULL, buffer) == -ENODEV);
        assert(buffer[0] == '\0' && clientdata_count == 0);
        assert(mir3da_readSensorData(&client, NULL) == -EINVAL);
        assert(clientdata_count == 0);
        client.data = NULL;
        assert(mir3da_get_data(&x, &y, &z, &status) == -ENODEV);
        assert(read_count == 0 && unlock_count == 1 && status == 44);
    } else {
        assert(!strcmp(argv[1], "success"));
        sensor_power = false;
        assert(mir3da_get_data(&x, &y, &z, &status) == 0);
        assert(x == 1723 && y == 1053 && z == -9807);
        assert(status == SENSOR_STATUS_ACCURACY_MEDIUM);
        assert(read_count == 1 && power_count == 1 && sleep_count == 1);
        assert(lock_count == 1 && unlock_count == 1 && sensor_power);
    }
    printf("PASS: sampling %s\n", argv[1]);
}
"""
    return HEADERS + SAMPLING_BOUNDARY + production + tests


def parser_fixture(cust):
    boundary = r"""
#define MIR3DA_BUFSIZE 256
#define MIR3DA_AXES_NUM 3
#define SENSOR_STATUS_ACCURACY_MEDIUM 2
static struct i2c_client *mir3da_i2c_client;
static const char *sample;
static int mir3da_mutex_lock(void) { return 0; }
static void mir3da_mutex_unlock(void) {}
static int mir3da_readSensorData(struct i2c_client *client, char *buffer)
{ (void)client; strcpy(buffer, sample); return 0; }
"""
    tests = r"""
int main(void)
{
    int x = 11, y = 22, z = 33, status = 44;
    sample = "0001 invalid";
    assert(mir3da_get_data(&x, &y, &z, &status) == -EIO);
    assert(x == 11 && y == 22 && z == 33 && status == 44);
    sample = "";
    assert(mir3da_get_data(&x, &y, &z, &status) == -EIO);
    sample = "ffffffff 0002 fffffffe";
    assert(mir3da_get_data(&x, &y, &z, &status) == 0);
    assert(x == -1 && y == 2 && z == -2 && status == 2);
    puts("PASS: reject partial samples without publishing any output; retain signed hex values");
}
"""
    return HEADERS + boundary + function(cust, "mir3da_get_data") + tests


def dump_fixture(cust, core):
    boundary = r"""
#include <stdarg.h>
#include <sys/types.h>
#define MIR3DA_REG_ADDR(reg) ((reg) & 0xff)
#define MI_FUN do {} while (0)
struct device_driver { int unused; };
static struct i2c_client client;
static MIR_HANDLE mir_handle = &client;
static int fail_address, read_count, formatted_values;
static int i2c_smbus_read_byte_data(struct i2c_client *ptr, u8 addr)
{
    assert(ptr == &client); ++read_count;
    return addr == fail_address ? -EREMOTEIO : addr;
}
static int counted_sprintf(char *buffer, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (!strcmp(format, "%02X ")) ++formatted_values;
    int count = vsprintf(buffer, format, args);
    va_end(args);
    return count;
}
"""
    adapter = function(cust, "i2c_smbus_read")
    dispatch = r"""
static int NSA_get_reg_data(MIR_HANDLE handle, char *buf);
struct general_ops {
    struct { int (*read)(PLAT_HANDLE, u8, u8 *); } smi;
    int (*mysprintf)(char *, const char *, ...);
};
static struct general_ops ops = { { i2c_smbus_read }, counted_sprintf };
static struct {
    struct general_ops *method;
    struct { int (*get_reg_data)(MIR_HANDLE, char *); } obj[1];
} mir3da_gsensor_drv = { &ops, { { NSA_get_reg_data } } };
static int gsensor_mod;
"""
    production = "\n".join((
        function(core, "mir3da_register_read"),
        function(core, "NSA_get_reg_data"),
        function(core, "mir3da_get_reg_data"),
        function(cust, "mir3da_reg_data_show"),
    ))
    tests = r"""
int main(int argc, char **argv)
{
    char buffer[4096] = { 0 };
    assert(argc == 2);
    fail_address = atoi(argv[1]);
    ssize_t count = mir3da_reg_data_show(NULL, buffer);
    if (fail_address >= 0) {
        assert(count == -EREMOTEIO);
        assert(read_count == fail_address + 1);
        assert(formatted_values == fail_address);
        printf("PASS: register dump stops at failed address %d before formatting its value\n", fail_address);
    } else {
        assert(count == (ssize_t)strlen(buffer));
        assert(read_count == 0xd3 && formatted_values == 0xd3);
        assert(strstr(buffer, "D2 ") && strstr(buffer, "--------end---------"));
        puts("PASS: complete register dump retains all 211 values and its byte count");
    }
}
"""
    return HEADERS + boundary + adapter + dispatch + production + tests


def period_fixture(accel, kernel_header, division_header):
    definitions = "\n".join(macro(kernel_header, name) for name in (
        "min_t", "max_t", "clamp_t", "clamp_val"))
    definitions += "\n" + macro(division_header, "do_div")
    boundary = r"""
#include <stddef.h>
#define ACC_LOG(...) do {} while (0)
#define ACC_PR_ERR(...) do {} while (0)
#define ACC_INVALID_VALUE -1
#define HRTIMER_MODE_ABS 0
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define smp_mb() do {} while (0)
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)
int acc_poll_lock;
typedef uint64_t u64;
typedef int64_t ktime_t;
struct hrtimer { ktime_t deadline; };
struct work_struct { int unused; };
struct acc_data { int x, y, z, status; int64_t timestamp; };
struct acc_context {
    int power, enable, delay;
    int64_t delay_ns, latency_ns;
    bool is_active_data, is_active_nodata, is_polling_run, is_first_data_after_enable;
    struct {
        int (*enable_nodata)(int);
        int (*batch)(int, int64_t, int64_t);
        bool is_report_input_direct, is_support_batch;
    } acc_ctl;
    struct { int (*get_data)(int *, int *, int *, int *); } acc_data;
    struct acc_data drv_data;
    struct hrtimer hrTimer;
    struct work_struct report;
    ktime_t target_ktime;
};
static struct acc_context context;
static struct acc_context *acc_context_obj = &context;
static int batch_result, batch_count, timer_count, report_count;
static int64_t batch_period, batch_latency;
static int64_t now_ns = 1000000000;
static int atomic_read(const int *value) { return *value; }
static void atomic_set(int *value, int data) { *value = data; }
static ktime_t ktime_get(void) { return now_ns; }
static ktime_t ktime_add_ns(ktime_t when, int64_t delay) { return when + delay; }
static int64_t ktime_to_ns(ktime_t when) { return when; }
static int64_t getCurNS(void) { return now_ns; }
static void hrtimer_start(struct hrtimer *timer, ktime_t when, int mode)
{ assert(mode == HRTIMER_MODE_ABS); timer->deadline = when; ++timer_count; }
static void hrtimer_cancel(struct hrtimer *timer) { (void)timer; }
static void cancel_work_sync(struct work_struct *work) { (void)work; }
static int enable_nodata(int enabled) { (void)enabled; return 0; }
static int batch(int flag, int64_t period, int64_t latency)
{
    assert(flag == 0); ++batch_count;
    batch_period = period; batch_latency = latency;
    return batch_result;
}
static int get_data(int *x, int *y, int *z, int *status)
{ *x = 2; *y = 3; *z = 4; *status = 2; return 0; }
static int acc_data_report(struct acc_data *data)
{
    assert(data->x == 2 && data->y == 3 && data->z == 4);
    /* Turn a non-progressing production fill loop into a deterministic failure. */
    assert(++report_count <= 10);
    return 0;
}
static void reset(int64_t period)
{
    memset(&context, 0, sizeof(context));
    context.power = context.enable = 1;
    context.is_active_data = true;
    context.delay = 200;
    context.delay_ns = period;
    context.latency_ns = 12345;
    context.acc_ctl.enable_nodata = enable_nodata;
    context.acc_ctl.batch = batch;
    context.acc_data.get_data = get_data;
    batch_result = batch_count = timer_count = report_count = 0;
    batch_period = batch_latency = -1;
}
"""
    stop = "acc_stop_polling" if "static void acc_stop_polling(" in accel else "stopTimer"
    production = "\n".join(function(accel, name) for name in (
        "startTimer", stop, "acc_work_func", "acc_enable_and_batch"))
    tests = r"""
int main(int argc, char **argv)
{
    assert(argc >= 2);
    if (!strcmp(argv[1], "work")) {
        reset(0);
        assert(acc_enable_and_batch() == 0);
        acc_work_func(&context.report);
        assert(report_count == 1);
        assert(context.hrTimer.deadline > now_ns);
        puts("PASS: zero-request work reports once and rearms a future timer");
    } else if (!strcmp(argv[1], "guards")) {
        reset(0);
        context.acc_ctl.is_report_input_direct = true;
        assert(acc_enable_and_batch() == 0);
        assert(context.delay == 200 && timer_count == 0 && batch_count == 1);
        reset(2500000000LL);
        batch_result = -EIO;
        assert(acc_enable_and_batch() != 0);
        assert(context.delay == 200 && timer_count == 0 && !context.is_polling_run);
        reset(2500000000LL);
        context.is_active_data = false;
        assert(acc_enable_and_batch() == 0);
        assert(context.delay == 200 && timer_count == 0);
        puts("PASS: direct reporting, inactive polling, and failed hardware configuration retain behavior");
    } else {
        assert(argc == 3);
        int64_t period = strtoll(argv[1], NULL, 10);
        int expected = atoi(argv[2]);
        reset(period);
        context.acc_ctl.is_support_batch = true;
        assert(acc_enable_and_batch() == 0);
        assert(context.delay == expected);
        assert(batch_count == 1 && batch_period == period && batch_latency == 12345);
        assert(timer_count == 1 && context.is_polling_run && context.is_first_data_after_enable);
        assert(context.hrTimer.deadline == now_ns + (int64_t)expected * 1000000);
        acc_work_func(&context.report);
        assert(report_count == 1 && context.hrTimer.deadline > now_ns);
        printf("PASS: %lld ns -> %d ms with intact hardware batch arguments\n", (long long)period, expected);
    }
}
"""
    return HEADERS + definitions + boundary + production + tests


def run_fixture(directory, name, code, arguments, extra_flags=(), timeout=5):
    source = directory / f"{name}.c"
    executable = directory / name
    source.write_text(code)
    subprocess.run([
        "gcc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-format", "-Wno-unused-but-set-variable", "-Wno-unused-parameter",
        "-fno-omit-frame-pointer",
        "-fsanitize=address,undefined",
        "-fno-pie", "-no-pie", *extra_flags, str(source), "-o", str(executable),
    ], check=True)
    passed = True
    for argv in arguments:
        try:
            result = subprocess.run([str(executable), *argv], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=timeout,
                                    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})
        except subprocess.TimeoutExpired:
            print(f"FAIL: {name} {' '.join(argv)} timed out after {timeout}s", flush=True)
            passed = False
            continue
        print(result.stdout, end="", flush=True)
        if result.returncode:
            print(f"FAIL: {name} {' '.join(argv)} exited {result.returncode}", flush=True)
            passed = False
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--revision", help="Read production bodies from this git revision")
    parser.add_argument("--keep-generated", type=Path, help="Keep the exact generated C fixtures")
    parser.add_argument("--suite", choices=("mir3da", "dump", "period", "all"), default="all")
    args = parser.parse_args()

    def read(relative):
        if args.revision:
            return subprocess.check_output(
                ["git", "show", f"{args.revision}:{relative}"], cwd=args.kernel, text=True)
        return (args.kernel / relative).read_text()

    fixtures = []
    if args.suite in ("mir3da", "dump", "all"):
        cust = read(BASE + "mir3da/mir3da_cust.c")
        core = read(BASE + "mir3da/mir3da_core.c")
        if args.suite != "dump":
            fixtures.extend((
                ("smbus", smbus_fixture(cust, core), [[]]),
                ("sampling", sampling_fixture(cust, core),
                 [[test] for test in ("read", "lock", "power", "null", "success")]),
                ("parser", parser_fixture(cust), [[]]),
            ))
        fixtures.append(("dump", dump_fixture(cust, core), [[str(addr)] for addr in (0, 17, 210, -1)]))
    if args.suite in ("period", "all"):
        arguments = [
            [str(ns), str(ms)] for ns, ms in (
                (0, 1), (1, 1), (999999, 1), (1000000, 1), (200000000, 200),
                (2500000000, 2500), (4294967296, 4294), (60000000000, 60000),
                (2147483647000000, 2147483647), (2147483648000000, 2147483647),
                (9223372036854775807, 2147483647),
            )
        ] + [["work"], ["guards"]]
        fixtures.append(("period", period_fixture(
            read(BASE + "accel.c"), read("include/linux/kernel.h"),
            read("include/asm-generic/div64.h")), arguments))
    with tempfile.TemporaryDirectory(prefix="mir3da-host-") as temporary:
        directory = Path(temporary)
        if args.keep_generated:
            directory = args.keep_generated.resolve()
            directory.mkdir(parents=True, exist_ok=True)
        results = [run_fixture(directory, *fixture) for fixture in fixtures]
    raise SystemExit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
