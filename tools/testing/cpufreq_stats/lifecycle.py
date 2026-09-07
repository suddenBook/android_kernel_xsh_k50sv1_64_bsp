#!/usr/bin/env python3
"""Run the driver's actual table lifecycle across policy-kobject replacement."""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
source = (ROOT / "drivers/cpufreq/cpufreq_stats.c").read_text()


def function(name):
    match = re.search(r"^static [^\n]+\b" + name + r"\(", source, re.M)
    if not match:
        raise RuntimeError("missing source function: " + name)
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


struct = re.search(r"struct cpufreq_stats \{.*?\n\};", source, re.S).group()
reset = re.search(r"^#define CPUFREQ_STATS_RESET .*", source, re.M)
program = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
typedef uint64_t u64;
#define CONFIG_CPU_FREQ_STAT_DETAILS 1
#define GFP_KERNEL 0
#define per_cpu(name, cpu) (name[cpu])
#define pr_debug(...) do {} while (0)
#define spin_lock(lock) do {} while (0)
#define spin_unlock(lock) do {} while (0)
static unsigned long long ticks;
#define get_jiffies_64() ticks
#define jiffies_64_to_clock_t(x) (x)
struct kobject { bool stats; };
struct cpufreq_policy { unsigned int cpu, cur, last_cpu; struct kobject kobj; };
struct cpufreq_frequency_table { unsigned int frequency; };
#define cpufreq_for_each_valid_entry(pos, table) \
    for (pos = table; pos->frequency; ++pos)
static int stats_attr_group;
static int allocations, fail_after = -1;
static void *kzalloc(size_t size, int flags)
{
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    void *ptr = calloc(1, size);
    if (ptr) ++allocations;
    return ptr;
}
static void kfree(void *ptr)
{
    if (ptr) --allocations;
    free(ptr);
}
static int sysfs_create_group(struct kobject *obj, void *group)
{
    if (obj->stats) return -EEXIST;
    obj->stats = true;
    return 0;
}
static void sysfs_remove_group(struct kobject *obj, void *group)
{
    obj->stats = false;
}
"""
if reset:
    program += reset.group() + "\nstatic bool isCpufreqStatExit;\n"
program += struct + "\nstatic struct cpufreq_stats *cpufreq_stats_table[8];\n"
program += "\n".join(function(name) for name in (
    "freq_table_get_index", "__cpufreq_stats_free_table",
    "__cpufreq_stats_create_table", "cpufreq_stats_update_policy_cpu"))
program += r"""
int main(void)
{
    struct cpufreq_frequency_table table[] = {{1001000}, {598000}, {0}};
    struct cpufreq_policy policy = {.cpu = 0, .cur = 598000};
    for (int cycle = 0; cycle < 20; ++cycle) {
        assert(__cpufreq_stats_create_table(&policy, table, 2) == 0);
        if (!policy.kobj.stats) {
            fprintf(stderr, "FAIL: policy cycle %d has no stats/time_in_state\n", cycle);
            return 1;
        }
        assert(cpufreq_stats_table[0]->freq_table[0] == 1001000);
        assert(cpufreq_stats_table[0]->freq_table[1] == 598000);
        assert(cpufreq_stats_table[0]->last_index == 1);
        __cpufreq_stats_free_table(&policy);
        /* cpufreq_policy_put_kobj destroys this kobject after REMOVE_POLICY. */
        policy.kobj.stats = false;
        ticks += 100;
    }
    assert(allocations == 0);
    for (int failure = 0; failure < 2; ++failure) {
        fail_after = failure;
        assert(__cpufreq_stats_create_table(&policy, table, 2) == -ENOMEM);
        assert(!policy.kobj.stats);
        assert(!cpufreq_stats_table[0]);
        assert(allocations == 0);
        /* The core ignores CREATE_POLICY allocation errors, then may move
         * the policy owner when HPS offlines only the original CPU. */
        policy.last_cpu = 0;
        policy.cpu = 1;
        cpufreq_stats_update_policy_cpu(&policy);
        assert(!cpufreq_stats_table[0] && !cpufreq_stats_table[1]);
        policy.cpu = 0;
    }
    fail_after = -1;
    assert(__cpufreq_stats_create_table(&policy, table, 2) == 0);
    policy.last_cpu = 0;
    policy.cpu = 1;
    cpufreq_stats_update_policy_cpu(&policy);
    assert(!cpufreq_stats_table[0]);
    assert(cpufreq_stats_table[1]->cpu == 1);
    __cpufreq_stats_free_table(&policy);
    assert(allocations == 0);
    puts("PASS: 20 replacements; allocation cleanup; owner migration with/without stats");
}
"""
with tempfile.TemporaryDirectory(prefix="cpufreq-stats-", dir=Path(__file__).parent) as tmp:
    test = Path(tmp) / "lifecycle.c"
    executable = Path(tmp) / "lifecycle"
    test.write_text(program)
    subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-sign-compare", "-g",
                    str(test), "-o", str(executable)],
                   check=True)
    subprocess.run([str(executable)], check=True)
