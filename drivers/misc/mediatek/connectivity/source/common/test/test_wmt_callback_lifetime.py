#!/usr/bin/env python3
"""Exercise complete production bridge, thermal callback and teardown functions."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

from test_wmt_shutdown import deinit_functions


CASES = [
    "unregistered-dispatch-and-repeated-close",
    "actual-thermal-assert-clock-chain",
    "thermal-command-may-sleep",
    "thermal-final-module-return",
    "assert-final-module-return",
    "clock-final-module-return",
    "drain-waits-for-last-of-three-callbacks",
    "closed-entry-refuses-all-new-callbacks",
    "replacement-drains-old-callback",
    "registration-cannot-reopen-during-unregister",
    "concurrent-unregisters-wait-for-callback",
    "nested-dispatch-keeps-outer-reference",
    "clock-dispatch-preserves-atomic-context",
    "partial-table-does-not-leak-references",
    "outer-exit-drains-before-first-resource",
    "library-failure-cleanup-drains-before-first-resource",
    "library-cleanup-without-platform-ownership",
    "close-followed-by-successful-registration",
    "callback-acquired-before-module-body",
]


def function(source, name):
    match = re.search(r"(?m)^[A-Za-z_][^;\n]*\b" + re.escape(name)
                      + r"\([^;{}]*\)\n\{", source)
    if not match:
        raise ValueError("Missing production function: " + name)
    return source[match.start():source.index("\n}", match.end()) + 2] + "\n"


def stub_dispatcher(source, name):
    start = source.index("#ifdef MTK_WCN_REMOVE_KERNEL_MODULE\nint mtk_wcn_cmb_stub_" + name + "(void)")
    return source[start:source.index("\n}", start) + 2] + "\n"


def fixture(sources):
    bridge, bridge_h, stub, stub_h, plat, plat_h, dev, lib, consys, exp_h = sources
    types = "".join(re.search(r"typedef enum _" + name + r" \{.*?^\}[^;]*;", exp_h, re.M | re.S)[0] + "\n"
                    for name in ["ENUM_WMTDRV_TYPE_T", "ENUM_WMTTHERM_TYPE_T"])
    types += re.search(r"^#define WMT_LOG_DBG[^\n]*$", exp_h, re.M)[0] + "\n"
    types += "\n".join(re.findall(r"^typedef[^\n]*wmt_bridge_[^\n]*;$", bridge_h, re.M)) + "\n"
    types += re.search(r"struct wmt_platform_bridge \{.*?^\};", bridge_h, re.M | re.S)[0] + "\n"
    types += "".join(re.search(r"enum " + name + r" \{.*?^\};", stub_h, re.M | re.S)[0] + "\n"
                     for name in ["CMB_STUB_AIF_X", "CMB_STUB_AIF_CTRL"])
    types += stub_h[stub_h.index("typedef int (*wmt_aif_ctrl_cb)"):stub_h.index("typedef void (*msdc_sdio_irq_handler_t)")]
    types += re.search(r"struct _CMB_STUB_CB_ \{.*?^\};", stub_h, re.M | re.S)[0] + "\n"
    types += "\n".join(re.findall(r"^typedef[^\n]*(?:thermal_query_ctrl_cb|trigger_assert_cb)[^\n]*;$", plat_h, re.M)) + "\n"
    types += re.search(r"enum wmt_init_status \{.*?\n\};", dev, re.S)[0] + "\n"

    bridge_start = bridge.index("static struct wmt_platform_bridge bridge;")
    bridge_end = bridge.index("/*******************************************************************************", bridge_start)
    bridge_code = bridge[bridge_start:bridge_end]
    state = "\n".join(re.findall(r"^static wmt_[^\n]* cmb_stub_[^;\n]*;$", stub, re.M)) + "\n"
    state += "\n".join(re.findall(r"^(?:static )?(?:thermal_query_ctrl_cb|trigger_assert_cb|bool|ENUM_WMT_CHIP_TYPE) "
                                  r"(?:wmt_plat_thermal_query_ctrl_cb|wmt_plat_trigger_assert_cb|g_wmt_plat_\w+)[^;\n]*;$", plat, re.M)) + "\n"
    state += "\n".join(re.findall(
        r"^static (?:DEFINE_(?:MUTEX|SPINLOCK)\(g_wmt_(?:op|assert)[^\n]*|"
        r"DECLARE_WAIT_QUEUE_HEAD\(g_wmt_op[^\n]*|atomic_t g_wmt_ops_checked_out[^\n]*|"
        r"bool g_wmt_(?:op_pool|worker_timer|utc_timer|core|resources|platform|ps|idc|assert_work|deinit_prepared)[^\n]*);$",
        lib, re.M)) + "\n"
    for name in ["gWmtMajor", "gWmtInitStatus", "gWmtCdev"]:
        state += re.search(r"(?m)^static [^;\n]*\b" + name + r"\b[^;]*;", dev)[0] + "\n"
    if "g_wmt_plat_initialized" in state:
        state += "#define HOST_HAS_PLATFORM_OWNER 1\n"

    functions = [stub_dispatcher(stub, "query_ctrl"), stub_dispatcher(stub, "trigger_assert")]
    functions += [function(stub, name) for name in ["_mtk_wcn_cmb_stub_clock_fail_dump",
                                                   "mtk_wcn_cmb_stub_reg", "mtk_wcn_cmb_stub_unreg"]]
    functions += [function(plat, name) for name in ["wmt_plat_thermal_ctrl", "wmt_plat_assert_ctrl",
                                                   "wmt_plat_clock_fail_dump", "wmt_plat_thermal_ctrl_cb_reg",
                                                   "wmt_plat_trigger_assert_cb_reg", "wmt_plat_deinit"]]
    functions += [function(consys, "mtk_wcn_consys_clock_fail_dump")]
    functions += [function(dev, "wmt_dev_tm_temp_query"), function(dev, "WMT_exit")]
    functions += [function(lib, name) for name in ["wmt_lib_register_thermal_ctrl_cb",
                                                  "wmt_lib_register_trigger_assert_cb",
                                                  "wmt_lib_trigger_assert"]]
    functions += deinit_functions(lib)
    prototypes = "\n".join(value[:value.index("\n{")] + "\n;" for value in functions)
    host = Path(__file__).with_name("wmt_callback_lifetime_host.c").read_text()
    for marker, value in [("SOURCE_TYPES", types), ("SOURCE_STATES", state),
                          ("SOURCE_PROTOTYPES", prototypes), ("SOURCE_BRIDGE", bridge_code),
                          ("SOURCE_FUNCTIONS", "\n".join(functions))]:
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
    common = Path("drivers/misc/mediatek/connectivity")
    paths = [common / name for name in ["common/wmt_build_in_adapter.c", "common/wmt_build_in_adapter.h",
                                        "source/common/common_detect/mtk_wcn_stub_alps.c"]]
    paths += [Path("drivers/misc/mediatek/include/mt-plat/mtk_wcn_cmb_stub.h")]
    paths += [common / "source/common/common_main" / name for name in [
        "platform/wmt_plat_alps.c", "include/wmt_plat.h", "linux/wmt_dev.c", "core/wmt_lib.c",
        "platform/mtk_wcn_consys_hw.c", "include/wmt_exp.h"]]
    contents = [subprocess.check_output(["git", "show", args.revision + ":" + str(path)], cwd=args.kernel)
                if args.revision else (args.kernel / path).read_bytes() for path in paths]
    if args.case and set(args.case) - set(CASES):
        parser.error("Unknown case")
    args.output.mkdir(parents=True, exist_ok=False)
    source, binary = args.output / "fixture.c", args.output / "fixture"
    source.write_text(fixture([value.decode() for value in contents]))
    sanitizer = "thread" if args.sanitizer == "thread" else "address,undefined"
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-but-set-variable",
        "-pthread", "-fsanitize=" + sanitizer,
        "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-finstrument-functions",
        "-no-pie", str(source), "-o", str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True)
    (args.output / "compile.txt").write_bytes(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print(compiled.stderr.decode(errors="replace"))
        raise SystemExit(compiled.returncode)
    rows = []
    for number, name in enumerate(CASES):
        if args.case and name not in args.case:
            continue
        try:
            run = subprocess.run([str(binary), str(number)], capture_output=True, timeout=15,
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
                  source_sha256={str(path): hashlib.sha256(data).hexdigest() for path, data in zip(paths, contents)},
                  fixture_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  host_sha256=hashlib.sha256(Path(__file__).with_name("wmt_callback_lifetime_host.c").read_bytes()).hexdigest())
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    raise SystemExit(result["passed"] != result["total"])


if __name__ == "__main__":
    main()
