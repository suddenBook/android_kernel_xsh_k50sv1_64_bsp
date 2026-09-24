#!/usr/bin/env python3
"""Exercise production MT6755 DVFS code with fault-injected hardware boundaries."""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


BASE = "drivers/misc/mediatek/base/power/mt6755/"
CASES = ["retry", "busy", "partial", "other_group", "release", "concurrent", "masked", "autok"]


def function(source, name):
    match = re.search(rf"(?m)^[\w *]+\b{name}\s*\([^;{{}}]*\)\s*\{{", source)
    if not match:
        raise ValueError(f"Function not found: {name}")
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def between(source, start, end):
    return source[source.index(start):source.index(end)]


def fixture(read):
    manager = read(BASE + "mt_vcorefs_manager.c")
    governor = read(BASE + "mt_vcorefs_governor.c")
    header = read(BASE + "mt_vcorefs_governor.h")
    declarations = between(header, "struct kicker_config {", "enum md_status {")
    declarations += between(header, "struct opp_profile {", "extern unsigned int vcorefs_log_mask")
    # Keep the actual group masks and arbitration rather than duplicating them.
    dispatch = between(governor, "struct dvfs_func {", "/*\n * init vcorefs function")
    manager_state = between(manager, "struct vcorefs_profile {", "void vcorefs_register_req_notify")
    manager_code = "\n".join(function(manager, name) for name in (
        "is_vcorefs_can_work", "_get_dvfs_opp",
        *(["kicker_request_compare"] if "static int kicker_request_compare(" in manager else []),
        "kicker_request_mask", "record_kicker_opp_in_aee",
        "vcorefs_request_dvfs_opp", "vcorefs_autok_lock_dvfs", "vcorefs_autok_set_vcore"))
    return (PREAMBLE + declarations + BOUNDARY + dispatch + manager_state + manager_code
            + TESTS)


PREAMBLE = r"""
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef uint32_t u32;
#define __nosavedata
#define SPM_AEE_RR_REC 0
#define BUG() abort()
#define vcorefs_debug_mask(kicker, ...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define vcorefs_err(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define vcorefs_crit(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
"""

BOUNDARY = r"""
typedef void (*vcorefs_req_handler_t)(enum dvfs_kicker, enum dvfs_opp);
static pthread_mutex_t vcorefs_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed = PTHREAD_COND_INITIALIZER;
static _Thread_local bool second_request;
static bool pause_spm, entered_spm, waiting_for_manager;
static void mutex_lock(pthread_mutex_t *mutex)
{
    if (second_request && mutex == &vcorefs_mutex) {
        assert(!pthread_mutex_lock(&gate));
        waiting_for_manager = true;
        assert(!pthread_cond_broadcast(&gate_changed));
        assert(!pthread_mutex_unlock(&gate));
    }
    assert(!pthread_mutex_lock(mutex));
}
static void mutex_unlock(pthread_mutex_t *mutex)
{ assert(!pthread_mutex_unlock(mutex)); }

static int kicker_table[LAST_KICKER];
static struct governor_profile {
    int curr_vcore_uv, curr_ddr_khz, vcore_dvs, ddr_dfs;
} governor_ctrl = { .vcore_dvs = 1, .ddr_dfs = 1 };
static struct opp_profile opp_table[] = {
    { VCORE_1_P_00_UV, FDDR_S0_KHZ }, { VCORE_0_P_90_UV, FDDR_S1_KHZ }
};
static int applied[2], spm_calls, next_error;
static int vcorefs_get_curr_vcore(void)
{
    return applied[0] == OPPI_PERF || applied[1] == OPPI_PERF
        ? VCORE_1_P_00_UV : VCORE_0_P_90_UV;
}
static int vcorefs_get_curr_ddr(void)
{
    return applied[0] == OPPI_PERF || applied[1] == OPPI_PERF
        ? FDDR_S0_KHZ : FDDR_S1_KHZ;
}
static int spm_apply(int group, int opp)
{
    int ret = next_error;
    ++spm_calls;
    next_error = 0;
    if (pause_spm) {
        assert(!pthread_mutex_lock(&gate));
        entered_spm = true;
        assert(!pthread_cond_broadcast(&gate_changed));
        while (!waiting_for_manager)
            assert(!pthread_cond_wait(&gate_changed, &gate));
        /* A blocked request must not alter the in-flight vote snapshot. */
        assert(kicker_table[KIR_OVL] == OPPI_UNREQ);
        pause_spm = false;
        assert(!pthread_mutex_unlock(&gate));
    }
    if (!ret)
        applied[group] = opp;
    return ret;
}
static int spm_vcorefs_set_dvfs_hpm_force(int opp, int vcore, int ddr)
{ return spm_apply(0, opp); }
static int spm_vcorefs_set_dvfs_hpm(int opp, int vcore, int ddr)
{ return spm_apply(1, opp); }
static int vcorefs_release_hpm(int opp, int vcore, int ddr)
{ abort(); }
static int vcorefs_handle_kir_sysfsx_req(int opp, int vcore, int ddr)
{ abort(); }
static bool is_vcorefs_feature_enable(void) { return true; }
static bool vcorefs_request_init_opp(int kicker, int opp) { abort(); }
static bool governor_autok_check(int kicker, int opp) { return false; }
static bool governor_autok_lock_check(int kicker, int opp) { abort(); }
static int vcorefs_gpu_get_init_opp(void) { return OPPI_UNREQ; }
static const char *get_kicker_name(int kicker) { return "test"; }
"""

TESTS = r"""
static void reset(void)
{
    for (int i = 0; i < LAST_KICKER; ++i)
        kicker_table[i] = OPPI_UNREQ;
    feature_en = true;
    vcorefs_ctrl.init_done = true;
    vcorefs_ctrl.plat_init_opp = OPPI_LOW_PWR;
    vcorefs_curr_opp = vcorefs_prev_opp = OPPI_LOW_PWR;
    applied[0] = applied[1] = OPPI_LOW_PWR;
}
static void *request_thread(void *arg)
{
    int kicker = (int)(intptr_t)arg;
    second_request = kicker == KIR_OVL;
    assert(vcorefs_request_dvfs_opp(kicker, OPPI_PERF) == 0);
    return NULL;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    alarm(10);
    reset();
    if (!strcmp(argv[1], "retry") || !strcmp(argv[1], "busy") ||
        !strcmp(argv[1], "partial")) {
        int error = !strcmp(argv[1], "retry") ? -1 :
                    !strcmp(argv[1], "busy") ? -EBUSY : -3;
        next_error = error;
        assert(vcorefs_request_dvfs_opp(KIR_GPU, OPPI_PERF) == error);
        assert(kicker_table[KIR_GPU] == OPPI_PERF);
        assert(applied[0] == OPPI_LOW_PWR);
        next_error = error;
        assert(vcorefs_request_dvfs_opp(KIR_GPU, OPPI_PERF) == error);
        assert(spm_calls == 2 && applied[0] == OPPI_LOW_PWR);
        assert(vcorefs_request_dvfs_opp(KIR_GPU, OPPI_PERF) == 0);
        assert(spm_calls == 3 && applied[0] == OPPI_PERF);
    } else if (!strcmp(argv[1], "other_group")) {
        next_error = -1;
        assert(vcorefs_request_dvfs_opp(KIR_GPU, OPPI_PERF) == -1);
        assert(vcorefs_request_dvfs_opp(KIR_MM_WFD, OPPI_PERF) == 0);
        assert(applied[0] == OPPI_LOW_PWR && applied[1] == OPPI_PERF);
        assert(vcorefs_request_dvfs_opp(KIR_GPU, OPPI_PERF) == 0);
        assert(spm_calls == 3 && applied[0] == OPPI_PERF);
    } else if (!strcmp(argv[1], "release")) {
        assert(vcorefs_request_dvfs_opp(KIR_GPU, OPPI_PERF) == 0);
        next_error = -EBUSY;
        assert(vcorefs_request_dvfs_opp(KIR_GPU, OPPI_UNREQ) == -EBUSY);
        assert(kicker_table[KIR_GPU] == OPPI_UNREQ);
        assert(vcorefs_request_dvfs_opp(KIR_GPU, OPPI_UNREQ) == 0);
        assert(spm_calls == 3 && applied[0] == OPPI_LOW_PWR);
        assert(vcorefs_request_dvfs_opp(KIR_OVL, OPPI_PERF) == 0);
        assert(vcorefs_request_dvfs_opp(KIR_GPU, OPPI_PERF) == 0);
        assert(vcorefs_request_dvfs_opp(KIR_GPU, OPPI_UNREQ) == 0);
        assert(kicker_table[KIR_OVL] == OPPI_PERF && applied[0] == OPPI_PERF);
    } else if (!strcmp(argv[1], "concurrent")) {
        pthread_t first, second;
        pause_spm = true;
        assert(!pthread_create(&first, NULL, request_thread, (void *)(intptr_t)KIR_GPU));
        assert(!pthread_mutex_lock(&gate));
        while (!entered_spm)
            assert(!pthread_cond_wait(&gate_changed, &gate));
        assert(!pthread_mutex_unlock(&gate));
        assert(!pthread_create(&second, NULL, request_thread, (void *)(intptr_t)KIR_OVL));
        assert(!pthread_join(first, NULL));
        assert(!pthread_join(second, NULL));
        assert(spm_calls == 2);
        assert(kicker_table[KIR_GPU] == OPPI_PERF && kicker_table[KIR_OVL] == OPPI_PERF);
        assert(vcorefs_request_dvfs_opp(KIR_GPU, OPPI_UNREQ) == 0);
        assert(applied[0] == OPPI_PERF);
    } else if (!strcmp(argv[1], "masked") || !strcmp(argv[1], "autok")) {
        if (!strcmp(argv[1], "masked"))
            vcorefs_ctrl.kr_req_mask = 1U << KIR_GPU;
        else
            vcorefs_ctrl.autok_lock = true;
        assert(vcorefs_request_dvfs_opp(KIR_GPU, OPPI_PERF) == -1);
        assert(spm_calls == 0 && kicker_table[KIR_GPU] == OPPI_UNREQ);
        assert(!pthread_mutex_trylock(&vcorefs_mutex));
        assert(!pthread_mutex_unlock(&vcorefs_mutex));
    } else {
        abort();
    }
    printf("PASS: %s\n", argv[1]);
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--revision", help="Extract production code from this git revision")
    parser.add_argument("--keep-generated", type=Path)
    parser.add_argument("--case", action="append", choices=CASES)
    args = parser.parse_args()

    def read(path):
        if args.revision:
            return subprocess.check_output(
                ["git", "show", f"{args.revision}:{path}"], cwd=args.kernel, text=True)
        return (args.kernel / path).read_text()

    def run(directory):
        directory.mkdir(parents=True, exist_ok=True)
        source = directory / "dvfs.c"
        executable = directory / "dvfs"
        source.write_text(fixture(read))
        subprocess.run(["gcc", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra",
                        "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-unused-variable",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                        "-pthread", str(source), "-o", str(executable)], check=True)
        failures = 0
        for case in args.case or CASES:
            result = subprocess.run([str(executable), case], timeout=15)
            if result.returncode:
                print(f"FAIL: {case} (exit {result.returncode})", flush=True)
                failures += 1
        return int(failures != 0)

    if args.keep_generated:
        return run(args.keep_generated.resolve())
    with tempfile.TemporaryDirectory(prefix="mt6755-dvfs-") as directory:
        return run(Path(directory))


if __name__ == "__main__":
    raise SystemExit(main())
