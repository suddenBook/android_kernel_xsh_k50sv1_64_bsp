#!/usr/bin/env python3
"""Run extracted platform, hardware-registration, wake-lock and stub lifecycle code."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


CASES = [
    "soc-successful-cleanup", "combo-default-sequence", "combo-custom-sequence",
    "clock-query-unsupported", "clock-query-default", "configured-clock-preferred",
    "soc-wake-allocation-failure", "combo-wake-allocation-failure",
    "soc-driver-register-enomem", "soc-driver-register-ebusy", "stub-registration-rejected",
    "deinit-before-init", "repeated-deinit", "duplicate-init-keeps-owner",
    "soc-owner-survives-detection-change", "combo-owner-survives-detection-change",
    "callbacks-published-after-resources", "callbacks-withdrawn-before-hardware",
    "stub-unregister-clears-assertion", "invalid-stub-does-not-publish",
    "driver-registration-rollback", "combo-invalid-sequence-uses-default",
    "combo-stub-registration-rejected",
]


def function(source, name):
    match = re.search(r"(?m)^[A-Za-z_][^;\n]*\b" + re.escape(name) + r"\([^;{}]*\)\n\{", source)
    if not match:
        raise ValueError("Missing production function: " + name)
    return source[match.start():source.index("\n}", match.end()) + 2] + "\n"


def conditional_stub_function(source, name):
    start = source.index("#ifdef MTK_WCN_REMOVE_KERNEL_MODULE\nint mtk_wcn_cmb_stub_" + name + "(void)")
    return source[start:source.index("\n}", start) + 2] + "\n"


def fixture(sources):
    plat, consys, combo, osal, stub, stub_h, bridge, bridge_h = sources
    types = "".join(re.search(r"enum " + name + r" \{.*?^\};", stub_h, re.M | re.S)[0] + "\n"
                    for name in ["CMB_STUB_AIF_X", "CMB_STUB_AIF_CTRL"])
    types += stub_h[stub_h.index("typedef int (*wmt_aif_ctrl_cb)"):stub_h.index("typedef void (*msdc_sdio_irq_handler_t)")]
    types += re.search(r"struct _CMB_STUB_CB_ \{.*?^\};", stub_h, re.M | re.S)[0] + "\n"
    types += "\n".join(re.findall(r"^typedef[^\n]*wmt_bridge_[^\n]*;$", bridge_h, re.M)) + "\n"
    types += re.search(r"struct wmt_platform_bridge \{.*?^\};", bridge_h, re.M | re.S)[0] + "\n"
    states = "\n".join(re.findall(r"^static (?:bool|ENUM_WMT_CHIP_TYPE) g_wmt_plat_[^\n]*;$", plat, re.M))
    states += "\n" + "\n".join(re.findall(r"^static wmt_[^\n]* cmb_stub_[^;\n]*;$", stub, re.M))
    states += "\n" + "\n".join(re.findall(r"^static (?:bool|INT32) g_wmt_(?:probe_result|hw_registered)[^\n]*;$",
                                            consys, re.M))
    constants = "\n".join(re.findall(r"^#define DFT_[^\n]*$", combo, re.M))
    wake = "\n".join(function(osal, name) for name in ["osal_wake_lock_init", "osal_wake_lock_deinit",
                                                     "osal_sleepable_lock_init", "osal_sleepable_lock_deinit"])
    bridge_start = bridge.index("static struct wmt_platform_bridge bridge;")
    bridge_end = bridge.index("/*******************************************************************************", bridge_start)
    bridge_code = bridge[bridge_start:bridge_end]
    stub_code = "".join(conditional_stub_function(stub, name) for name in ["query_ctrl", "trigger_assert"])
    stub_code += function(stub, "_mtk_wcn_cmb_stub_clock_fail_dump")
    stub_code += "#define mtk_wcn_cmb_stub_reg source_cmb_stub_reg\n"
    stub_code += function(stub, "mtk_wcn_cmb_stub_reg") + "#undef mtk_wcn_cmb_stub_reg\n"
    stub_code += function(stub, "mtk_wcn_cmb_stub_unreg")
    hardware = "\n".join(function(consys, name) for name in ["mtk_wcn_consys_co_clock_type",
                                                           "mtk_wcn_consys_hw_init", "mtk_wcn_consys_hw_deinit"])
    hardware += "\n" + "\n".join(function(combo, name) for name in ["mtk_wcn_cmb_hw_init", "mtk_wcn_cmb_hw_deinit"])
    platform = "\n".join(function(plat, name) for name in ["wmt_plat_soc_co_clock_flag_get",
                                                          "wmt_plat_soc_co_clock_flag_set",
                                                          "wmt_plat_init", "wmt_plat_deinit"])
    host = Path(__file__).with_name("wmt_platform_init_host.c").read_text()
    for marker, value in [("SOURCE_TYPES", types), ("SOURCE_STATES", states), ("SOURCE_CONSTANTS", constants),
                          ("SOURCE_WAKE", wake), ("SOURCE_BRIDGE", bridge_code), ("SOURCE_STUB", stub_code),
                          ("SOURCE_HARDWARE", hardware), ("SOURCE_PLATFORM", platform)]:
        host = host.replace("/* " + marker + " */", value)
    return host


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", help="Read production sources at this git revision")
    parser.add_argument("--case", action="append")
    args = parser.parse_args()
    common = Path("drivers/misc/mediatek/connectivity/source/common")
    paths = [common / name for name in ["common_main/platform/wmt_plat_alps.c",
                                        "common_main/platform/mtk_wcn_consys_hw.c",
                                        "common_main/platform/mtk_wcn_cmb_hw.c", "common_main/linux/osal.c",
                                        "common_detect/mtk_wcn_stub_alps.c"]]
    paths += [Path("drivers/misc/mediatek/include/mt-plat/mtk_wcn_cmb_stub.h"),
              Path("drivers/misc/mediatek/connectivity/common/wmt_build_in_adapter.c"),
              Path("drivers/misc/mediatek/connectivity/common/wmt_build_in_adapter.h")]
    contents = [subprocess.check_output(["git", "show", args.revision + ":" + str(path)], cwd=args.kernel)
                if args.revision else (args.kernel / path).read_bytes() for path in paths]
    if args.case and set(args.case) - set(CASES):
        parser.error("Unknown case")
    args.output.mkdir(parents=True, exist_ok=False)
    source, binary = args.output / "fixture.c", args.output / "fixture"
    source.write_text(fixture([value.decode() for value in contents]))
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-but-set-variable",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
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
            run = subprocess.run([str(binary), str(number)], capture_output=True, timeout=10,
                                 env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"))
            output, code = run.stdout + run.stderr, run.returncode
        except subprocess.TimeoutExpired as error:
            output = (error.stdout or b"") + (error.stderr or b"") + b"Timed out\n"
            code = 124
        (args.output / (name + ".txt")).write_bytes(output)
        rows.append(dict(case=name, passed=code == 0, exit_code=code))
        print(("PASS: " if code == 0 else "FAIL: ") + name, flush=True)
    result = dict(passed=sum(row["passed"] for row in rows), total=len(rows), cases=rows,
                  revision=args.revision, sanitizer="address,undefined", compiler=command,
                  source_sha256={str(path): hashlib.sha256(data).hexdigest() for path, data in zip(paths, contents)},
                  fixture_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  host_sha256=hashlib.sha256(Path(__file__).with_name("wmt_platform_init_host.c").read_bytes()).hexdigest())
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    raise SystemExit(result["passed"] != result["total"])


if __name__ == "__main__":
    main()
