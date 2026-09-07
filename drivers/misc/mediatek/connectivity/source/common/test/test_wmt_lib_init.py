#!/usr/bin/env python3
"""Exercise extracted WMT library lifecycle and OSAL constructors in a strict host."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

from test_wmt_shutdown import deinit_functions, function


CASES = [
    "cleanup-before-init", "configuration-read-failure", "wmtd-create-null",
    "wmtd-create-enomem", "wmtd-create-eintr", "worker-create-null",
    "worker-create-enomem", "worker-create-eintr", "core-init-failure",
    "platform-init-failure", "ps-init-failure", "wmtd-start-failure",
    "worker-start-failure", "wmtd-history-queue-allocation-failure",
    "wmtd-history-snapshot-allocation-failure", "worker-history-queue-allocation-failure",
    "worker-history-snapshot-allocation-failure", "optional-idc-registration-failure",
    "successful-soc-reinit", "successful-combo-reinit", "pending-timer-and-history-work",
    "patch-allocations-before-clear", "assert-publication-serializes-with-close",
    "running-assert-finishes-before-clear", "late-assert-is-refused",
    "assert-before-init-is-refused",
    "stp-callback-outlives-operation-pool-drain",
]
CONCURRENT = CASES[22:25]


def fixture(lib, osal):
    # Each function is copied whole; baseline and candidate use identical adapters.
    names = ["wmt_lib_init", "wmt_lib_trigger_assert_keyword_delay", "wmt_lib_btm_cb"]
    functions = [function(lib, name) for name in names] + deinit_functions(lib)
    functions += [function(osal, name) for name in [
        "osal_thread_create", "osal_thread_destroy", "osal_thread_stop",
        "osal_op_history_init", "osal_op_history_deinit",
    ]]
    states = "\n".join(re.findall(
        r"^static (?:DEFINE_(?:MUTEX|SPINLOCK)\(g_wmt_(?:op|assert)[^\n]*|"
        r"DECLARE_WAIT_QUEUE_HEAD\(g_wmt_op[^\n]*|atomic_t g_wmt_ops_checked_out[^\n]*|"
        r"bool g_wmt_(?:op_pool|worker_timer|utc_timer|core|resources|platform|ps|idc|assert_work|deinit_prepared)[^\n]*);$",
        lib, re.M))
    if "INT32 wmt_lib_deinit_prepare(VOID)" in lib:
        states += "\n#define HOST_SPLIT_DEINIT 1\n"
    prototypes = "".join(value[:value.index("\n{")] + ";\n" for value in functions)
    host = Path(__file__).with_name("wmt_lib_init_host.c").read_text()
    for marker, value in [("SOURCE_STATES", states), ("SOURCE_PROTOTYPES", prototypes),
                          ("PRODUCTION", "\n".join(functions))]:
        host = host.replace("/* " + marker + " */", value)
    return host


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", help="Read production sources at this git revision")
    parser.add_argument("--sanitizer", choices=["address", "thread"], default="address")
    parser.add_argument("--case", action="append")
    args = parser.parse_args()
    base = Path("drivers/misc/mediatek/connectivity/source/common/common_main")
    paths = [base / name for name in ["core/wmt_lib.c", "linux/osal.c"]]
    contents = [subprocess.check_output(["git", "show", args.revision + ":" + str(path)], cwd=args.kernel)
                if args.revision else (args.kernel / path).read_bytes() for path in paths]
    selected = [(i, name) for i, name in enumerate(CASES) if not args.case or name in args.case]
    if args.case and set(args.case) - set(CASES):
        parser.error("Unknown case")
    args.output.mkdir(parents=True, exist_ok=False)
    source, binary = args.output / "fixture.c", args.output / "fixture"
    source.write_text(fixture(*(value.decode() for value in contents)))
    sanitizer = "thread" if args.sanitizer == "thread" else "address,undefined"
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        "-Wno-unused-variable", "-Wno-unused-parameter", "-pthread", "-fsanitize=" + sanitizer,
        "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-no-pie", str(source), "-o", str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True)
    (args.output / "compile.txt").write_bytes(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print(compiled.stderr.decode(errors="replace"))
        raise SystemExit(compiled.returncode)
    rows = []
    for i, name in selected:
        try:
            run = subprocess.run([str(binary), str(i)], capture_output=True, timeout=10,
                                 env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                                          TSAN_OPTIONS="halt_on_error=1"))
            output, code = run.stdout + run.stderr, run.returncode
        except subprocess.TimeoutExpired as error:
            output = (error.stdout or b"") + (error.stderr or b"") + b"Timed out\n"
            code = 124
        (args.output / (name + ".txt")).write_bytes(output)
        rows.append(dict(case=name, passed=code == 0, exit_code=code))
        print(("PASS: " if code == 0 else "FAIL: ") + name, flush=True)
    result = dict(passed=sum(row["passed"] for row in rows), total=len(rows), cases=rows,
                  revision=args.revision, sanitizer=sanitizer, compiler=command,
                  source_sha256={str(path): hashlib.sha256(data).hexdigest()
                                 for path, data in zip(paths, contents)},
                  fixture_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  host_sha256=hashlib.sha256(Path(__file__).with_name("wmt_lib_init_host.c").read_bytes()).hexdigest())
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    raise SystemExit(result["passed"] != result["total"])


if __name__ == "__main__":
    main()
