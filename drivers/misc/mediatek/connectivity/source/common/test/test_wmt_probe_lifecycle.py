#!/usr/bin/env python3
"""Exercise complete MT6755 probe functions with the local devm/probe wrappers."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


CASES = [
    "successful-probe-remove", "remove-before-probe", "repeated-hardware-deinit",
    "duplicate-hardware-init", "driver-register-enomem", "driver-register-ebusy",
    "driver-groups-rollback", "driver-groups-rollback-after-probe-failure",
    "no-matching-device", "probe-null-device", "probe-missing-device-node",
] + [f"missing-register-resource-{i}" for i in range(4)] + [
    f"register-devres-allocation-{i}" for i in range(4)
] + [f"register-ioremap-failure-{i}" for i in range(4)] + [
    "clock-devres-allocation", "clock-provider-enoent", "clock-provider-deferred",
    "emi-base-missing", "emi-size-zero", "emi-clear-map-failure",
    "coredump-map-failure", "coredump-region-too-small",
] + [f"regulator-devres-allocation-{i}" for i in range(4)] + [
    f"regulator-provider-failure-{i}" for i in range(4)
] + [
    "pinctrl-devres-allocation", "pinctrl-provider-enomem", "pinctrl-provider-deferred",
    "optional-pinctrl-absent", "gps-pinmux-property", "gps-pins-fallback",
    "gps-phandle-absent", "gps-child-absent", "gps-properties-absent",
    "wifi-gpio-provider-deferred", "optional-wifi-gpio-absent",
    "coredump-restore-reuses-mapping", "coredump-unmap-is-idempotent",
    "framework-failure-before-probe", "legacy-ops-allow-unbound-registration",
]


def function(source, name):
    match = re.search(r"(?m)^[A-Za-z_][^;\n]*\b" + re.escape(name) + r"\([^;{}]*\)\n\{", source)
    if not match:
        raise ValueError("Missing complete source function: " + name)
    return source[match.start():source.index("\n}", match.end()) + 2] + "\n"


def block(source, pattern):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise ValueError("Missing source block: " + pattern)
    return match[0] + "\n"


def fixture(sources):
    common, chip, header, chip_h, io, clock, regulator, pinctrl, platform = sources
    types = block(header, r"struct CONSYS_BASE_ADDRESS \{.*?^\};")
    types += header[header.index("typedef INT32(*CONSYS_IC_CLOCK_BUFFER_CTRL)"):
                    header.index("} WMT_CONSYS_IC_OPS, *P_WMT_CONSYS_IC_OPS;")]
    types += "} WMT_CONSYS_IC_OPS, *P_WMT_CONSYS_IC_OPS;\n"
    constants = "\n".join(re.findall(
        r"^#define (?:CONSYS_BT_WIFI_SHARE_V33|CONSYS_PMIC_CTRL_ENABLE|CONSYS_EMI_MPU_SETTING|"
        r"CONSYS_EMI_COREDUMP_OFFSET|CONSYS_EMI_MAPPING_OFFSET|PLATFORM_SOC_CHIP)\s+[^\n]+$", chip_h, re.M))
    constants += "\n" + "\n".join(re.findall(r"^#define (?:KBYTE|CONSYS_EMI_MEM_SIZE)\s+[^\n]+$", header, re.M))
    state = "\n".join(re.findall(
        r"^(?![ \t])[^;\n{}]*\b(?:pEmibaseaddr|wmt_consys_ic_ops|g_pdev|gps_lna_pin_num|"
        r"wifi_ant_swap_gpio_pin_num|g_wmt_probe_result|g_wmt_hw_registered|consys_pinctrl)\b[^;\n]*;$",
        common, re.M)) + "\n"
    state += "\n".join(re.findall(
        r"^struct (?:regulator|clk) \*(?:reg_VCN18|reg_VCN28|reg_VCN33_BT|reg_VCN33_WIFI|clk_scp_conn_main);",
        chip, re.M)) + "\nstruct CONSYS_BASE_ADDRESS conn_reg;\n"
    if "consys_ic_probe_required" in header:
        state += "#define HOST_HAS_PROBE_GATE 1\n"

    chip_names = ["consys_read_reg_from_dts", "consys_clk_get_from_dts", "consys_pmic_get_from_dts",
                  "consys_emi_mpu_set_region_protection", "consys_emi_set_remapping_reg",
                  "bt_wifi_share_v33_spin_lock_init", "consys_emi_coredump_remapping",
                  "mtk_wcn_get_consys_ic_ops"]
    if "static VOID consys_probe_cleanup(VOID)\n{" in chip:
        chip_names += ["consys_probe_cleanup"]
    common_names = ["mtk_wmt_probe", "mtk_wmt_remove", "mtk_wcn_consys_hw_init",
                    "mtk_wcn_consys_hw_deinit", "mtk_wcn_consys_hw_restore"]
    if "static VOID mtk_wmt_probe_cleanup(" in common:
        common_names += ["mtk_wmt_probe_cleanup"]
    devm_sources = [(io, ["devm_ioremap_release", "devm_ioremap"]),
                    (clock, ["devm_clk_release", "devm_clk_get"]),
                    (regulator, ["devm_regulator_release", "_devm_regulator_get", "devm_regulator_get"]),
                    (pinctrl, ["devm_pinctrl_release", "devm_pinctrl_get"])]
    devm_functions = [function(source, name) for source, names in devm_sources for name in names]
    functions = [function(chip, name) for name in chip_names] + [function(common, name) for name in common_names]
    platform_functions = [function(platform, name) for name in
                          ["platform_drv_probe", "platform_drv_probe_fail", "platform_drv_remove", "platform_driver_probe"]]
    prototypes = "\n".join(value[:value.index("\n{")] + ";" for value in functions + devm_functions + platform_functions)
    ops = block(chip, r"^WMT_CONSYS_IC_OPS consys_ic_ops = \{.*?^\};")
    unused = []
    for name in re.findall(r"^\s*\.[A-Za-z_][A-Za-z_0-9]* = ([A-Za-z_][A-Za-z_0-9]*),$", ops, re.M):
        if name in chip_names or name == "MTK_WCN_BOOL_TRUE":
            continue
        prototype = re.search(r"^static [^;\n]*\b" + re.escape(name) + r"\([^;]*\);", chip, re.M)[0][:-1]
        unused.append(prototype + ("\n{}\n" if prototype.startswith("static VOID ") else "\n{ return 0; }\n"))
    driver = block(common, r"^static struct platform_driver mtk_wmt_dev_drv = \{.*?^\};")
    host = Path(__file__).with_name("wmt_probe_lifecycle_host.c").read_text()
    for marker, value in [("SOURCE_CONSTANTS", constants), ("SOURCE_TYPES", types), ("SOURCE_STATES", state),
                          ("SOURCE_PROTOTYPES", prototypes), ("SOURCE_UNUSED_OPS", "\n".join(unused)),
                          ("SOURCE_OPS", ops), ("SOURCE_DRIVER", driver),
                          ("SOURCE_DEVM", "\n".join(devm_functions)),
                          ("SOURCE_FUNCTIONS", "\n".join(functions)),
                          ("SOURCE_PLATFORM_PROBE", "\n".join(platform_functions))]:
        host = host.replace("/* " + marker + " */", value)
    return host


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", help="Read the three driver files and headers at this revision")
    parser.add_argument("--case", action="append")
    args = parser.parse_args()
    common = Path("drivers/misc/mediatek/connectivity/source/common/common_main/platform")
    paths = [common / name for name in ["mtk_wcn_consys_hw.c", "mt6755.c", "include/mtk_wcn_consys_hw.h", "include/mt6755.h"]]
    paths += [Path(name) for name in ["lib/devres.c", "drivers/clk/clk-devres.c", "drivers/regulator/devres.c",
                                    "drivers/pinctrl/core.c", "drivers/base/platform.c"]]
    contents = [subprocess.check_output(["git", "show", args.revision + ":" + str(path)], cwd=args.kernel)
                if args.revision else (args.kernel / path).read_bytes() for path in paths]
    context_paths = [Path(name) for name in ["drivers/base/dd.c", "drivers/base/driver.c",
                                            "drivers/base/devres.c", "drivers/of/address.c",
                                            "arch/arm64/boot/dts/mt6755.dts"]]
    context_contents = [subprocess.check_output(["git", "show", args.revision + ":" + str(path)], cwd=args.kernel)
                        if args.revision else (args.kernel / path).read_bytes() for path in context_paths]
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
        raise SystemExit(compiled.stderr.decode(errors="replace"))
    rows = []
    for name in CASES:
        if args.case and name not in args.case:
            continue
        try:
            run = subprocess.run([str(binary), name], capture_output=True, timeout=10,
                                 env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"))
            output, code = run.stdout + run.stderr, run.returncode
        except subprocess.TimeoutExpired as error:
            output = (error.stdout or b"") + (error.stderr or b"") + b"Timed out\n"
            code = 124
        (args.output / (name + ".txt")).write_bytes(output)
        diagnostic = None
        kind = None
        if code:
            log = output.decode(errors="replace")
            for label, pattern in [
                ("assertion", r"^.*Assertion .* failed\.$"),
                ("address-sanitizer", r"^.*ERROR: AddressSanitizer:.*$"),
                ("undefined-behavior-sanitizer", r"^.*runtime error:.*$"),
                ("timeout", r"^Timed out$"),
            ]:
                match = re.search(pattern, log, re.M)
                if match:
                    kind, diagnostic = label, match[0]
                    break
            if not kind:
                kind, diagnostic = "process-exit", log
        rows.append(dict(case=name, passed=code == 0, exit_code=code,
                         failure_kind=kind, diagnostic=diagnostic))
        print(("PASS: " if code == 0 else "FAIL: ") + name, flush=True)
    result = dict(passed=sum(row["passed"] for row in rows), total=len(rows), cases=rows,
                  revision=args.revision, sanitizer="address,undefined", compiler=command,
                  source_sha256={str(path): hashlib.sha256(data).hexdigest() for path, data in zip(paths, contents)},
                  adapter_context_sha256={str(path): hashlib.sha256(data).hexdigest()
                                          for path, data in zip(context_paths, context_contents)},
                  fixture_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  host_sha256=hashlib.sha256(Path(__file__).with_name("wmt_probe_lifecycle_host.c").read_bytes()).hexdigest())
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    raise SystemExit(result["passed"] != result["total"])


if __name__ == "__main__":
    main()
