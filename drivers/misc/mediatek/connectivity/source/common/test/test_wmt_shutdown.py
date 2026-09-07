#!/usr/bin/env python3
"""Exercise unmodified WMT pool, workers, init and teardown in a pthread host."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


CASES = [
    "queued-active-payload-release", "queued-wifi-worker-payload-release",
    "completed-retained-sender-delays-clear", "timed-out-retained-sender-delays-clear",
    "preborrowed-submit-refused-after-close", "unsubmitted-borrower-delays-clear",
    "enqueue-and-wakeup-serialize-with-close", "wifi-handoff-refused-after-close",
    "active-worker-finishes-before-clear", "late-reset-and-checkout-after-clear",
    "final-recycle-finishes-before-clear", "failed-init-before-pool-stays-closed",
    "failed-init-after-pool-stays-closed", "failed-worker-start-stays-closed",
    "pool-can-reopen-after-complete-teardown", "reset-drain-finishes-before-clear",
]


def function(source, name):
    match = re.search(r"(?m)^[A-Za-z_][^;\n]*\b" + re.escape(name) + r"\([^;{}]*\)\n\{", source)
    if not match:
        raise ValueError("Missing production function: " + name)
    return source[match.start():source.index("\n}", match.end()) + 2] + "\n"


def declaration(source, tag):
    return re.search(r"^typedef struct " + tag + r" \{.*?^\}[^;]*;", source, re.M | re.S)[0] + "\n"


def deinit_functions(lib):
    if "INT32 wmt_lib_deinit_prepare(VOID)" not in lib:
        return [function(lib, "wmt_lib_deinit")]
    # Pool/lifecycle unit tests have no STP producer. The callback fixture
    # separately verifies that its join belongs between these two phases.
    return [function(lib, name) for name in ["wmt_lib_deinit_prepare", "wmt_lib_deinit_finish"]] + [
        "static INT32 wmt_lib_deinit(VOID)\n{\n"
        "    INT32 ret = wmt_lib_deinit_prepare();\n"
        "    return ret + wmt_lib_deinit_finish();\n}\n"]


def fixture(sources):
    lib, osal, core_h, lib_h = sources
    types = "".join(re.search(r"^#define " + name + r"[^\n]*", source, re.M)[0] + "\n"
                    for source, name in [(osal, "OSAL_OP_DATA_SIZE"), (osal, "OSAL_OP_BUF_SIZE"),
                                         (lib_h, "WMT_OP_BUF_SIZE")])
    types += "".join(declaration(osal, tag) for tag in ["_OSAL_OP_DAT", "_OSAL_LXOP_", "_OSAL_LXOP_Q"])
    types += re.search(r"^typedef enum _ENUM_WMT_OPID_T \{.*?^\}[^;]*;", core_h, re.M | re.S)[0] + "\n"
    types += re.search(r"enum wmt_op_state \{.*?\};", lib, re.S)[0] + "\n"
    rings = osal[osal.index("#define RB_LATEST("):osal.index("#define RB_GET_LATEST(")]
    states = "\n".join(re.findall(
        r"^static (?:DEFINE_(?:MUTEX|SPINLOCK)\(g_wmt_(?:op|assert)[^\n]*|DECLARE_WAIT_QUEUE_HEAD\(g_wmt_op[^\n]*|"
        r"atomic_t g_wmt_ops_checked_out[^\n]*|bool g_wmt_(?:op_pool|worker_timer|utc_timer|core|resources|platform|ps|idc|assert_work|deinit_prepared)[^\n]*);$", lib, re.M))
    names = ["wmt_lib_init", "wmt_lib_get_free_op", "wmt_lib_get_op", "wmt_lib_put_op",
             "wmt_lib_drain_op_queue", "wmt_lib_queue_op", "wmt_lib_put_op_to_free_queue",
             "wmt_lib_alloc_op_data", "wmt_lib_put_op_ref", "wmt_lib_submit_op_result",
             "wmt_lib_put_act_op_result", "wmt_lib_put_act_op", "wmt_lib_put_worker_op",
             "wmt_lib_complete_op", "wmt_lib_cancel_current_op", "wmt_lib_active_op_id",
             "wmt_lib_get_current_op", "wmt_lib_set_current_op", "wmt_lib_set_worker_op",
             "wmt_lib_state_init", "wmt_lib_wait_event_checker", "wmt_lib_worker_wait_event_checker",
             "wmtd_thread", "wmtd_worker_thread"]
    functions = [function(lib, name) for name in names] + deinit_functions(lib)
    prototypes = "".join(value[:value.index("\n{")] + ";\n" for value in functions)
    host = Path(__file__).with_name("wmt_shutdown_host.c").read_text()
    for marker, value in [("SOURCE_TYPES", types), ("SOURCE_RINGS", rings), ("SOURCE_STATES", states),
                          ("SOURCE_PROTOTYPES", prototypes), ("PRODUCTION", "\n".join(functions))]:
        host = host.replace("/* " + marker + " */", value)
    return host


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sanitizer", choices=["address", "thread"], default="address")
    parser.add_argument("--case", action="append")
    args = parser.parse_args()
    common = args.kernel / "drivers/misc/mediatek/connectivity/source/common/common_main"
    paths = [common / value for value in ["core/wmt_lib.c", "linux/include/osal.h",
                                          "core/include/wmt_core.h", "core/include/wmt_lib.h"]]
    contents = [path.read_bytes() for path in paths]
    host = fixture([value.decode() for value in contents])
    selected = [(number, name) for number, name in enumerate(CASES) if not args.case or name in args.case]
    if args.case and set(args.case) - set(CASES):
        parser.error("Unknown case")
    args.output.mkdir(parents=True, exist_ok=False)
    c_file, binary = args.output / "fixture.c", args.output / "fixture"
    c_file.write_text(host)
    sanitizer = "thread" if args.sanitizer == "thread" else "address,undefined"
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        "-Wno-unused-but-set-variable", "-Wno-unused-variable", "-Wno-unused-parameter",
        "-pthread", "-fsanitize=" + sanitizer, "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
        "-no-pie", str(c_file), "-o", str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True)
    (args.output / "compile.txt").write_bytes(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print(compiled.stderr.decode(errors="replace"))
        raise SystemExit(compiled.returncode)
    rows = []
    for number, name in selected:
        try:
            run = subprocess.run([str(binary), str(number)], capture_output=True, timeout=20,
                                 env=dict(os.environ, TSAN_OPTIONS="halt_on_error=1",
                                          ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"))
            output, code = run.stdout + run.stderr, run.returncode
        except subprocess.TimeoutExpired as error:
            output = (error.stdout or b"") + (error.stderr or b"") + b"Timed out\n"
            code = 124
        (args.output / (name + ".txt")).write_bytes(output)
        rows.append(dict(case=name, exit_code=code, passed=code == 0))
        print(("PASS: " if code == 0 else "FAIL: ") + name, flush=True)
    result = dict(passed=sum(row["passed"] for row in rows), total=len(rows), cases=rows,
                  source_sha256={str(path.relative_to(args.kernel)): hashlib.sha256(value).hexdigest()
                                 for path, value in zip(paths, contents)},
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  host_sha256=hashlib.sha256(Path(__file__).with_name("wmt_shutdown_host.c").read_bytes()).hexdigest(),
                  fixture_sha256=hashlib.sha256(c_file.read_bytes()).hexdigest(), compiler=command,
                  sanitizer=sanitizer)
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    raise SystemExit(result["passed"] != result["total"])


if __name__ == "__main__":
    main()
