#!/usr/bin/env python3
"""Fault-test the actual WLAN probe, net allocation and proc setup functions.

The host fixture replaces kernel services, tracks their resource ownership and
executes callbacks while each service is drained. ASan/UBSan check the extracted
C for memory errors. It is an ordering/fault test, not a hardware or race test.
Use --revision to run the same assertions against an earlier kernel revision.
"""
import argparse
from itertools import product
from pathlib import Path
import re
import subprocess
import tempfile


def extract_function(source, name):
    """Find a definition and balance braces without counting comments/strings."""
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda match: re.sub(r"[^\n]", " ", match.group()),
                    source, flags=re.S)
    match = re.search(r"^[^\n;{}]*\b" + name + r"\([^;{}]*\)\s*\{", masked, re.M)
    if not match:
        raise ValueError(f"missing definition: {name}")
    end = match.end()
    depth = 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--revision", help="read driver sources from this git revision")
    parser.add_argument("--case", help="run only this named failure case")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    driver = "drivers/misc/mediatek/connectivity/source/wlan/core/gen2/os/linux/"

    def read(name):
        path = driver + name
        if args.revision:
            return subprocess.check_output(["git", "show", f"{args.revision}:{path}"],
                                           cwd=root, text=True)
        return (root / path).read_text()

    init, proc = read("gl_init.c"), read("gl_proc.c")
    unit = "".join(extract_function(proc, name) for name in (
        "procUninitProcFs", "procInitFs", "procRemoveProcfs", "procCreateFsEntry",
        "cfgRemoveProcEntry", "cfgCreateProcEntry", "procCountryRead"))
    unit += "".join(extract_function(init, name) for name in ("wlanNetCreate", "wlanNetDestroy"))
    if "enum wlan_probe_stage" in init:
        unit += "#define HAVE_PROBE_CLEANUP 1\n"
        unit += re.search(r"enum wlan_probe_stage\s*\{[^}]+\};", init).group() + "\n"
        unit += extract_function(init, "wlanProbeCleanup")
    unit += extract_function(init, "wlanProbe")

    args.scratch.mkdir(parents=True, exist_ok=True)
    failed = []
    total = 0
    with tempfile.TemporaryDirectory(prefix="wlan-probe-", dir=args.scratch) as temporary:
        temporary = Path(temporary)
        (temporary / "probe-under-test.c").write_text(unit)
        fixture = Path(__file__).with_suffix(".c")
        for threads, cam, firmware in product((0, 1), repeat=3):
            variant = f"threads={threads},cam={cam},firmware={firmware}"
            binary = temporary / "probe-test"
            subprocess.run(["clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-Wno-unused-variable",
                            "-Wno-unused-function", "-Wno-unused-but-set-variable",
                            "-fsanitize=address,undefined", "-g", "-I", str(temporary),
                            f"-DCFG_SUPPORT_MULTITHREAD={threads}",
                            f"-DCFG_SUPPORT_SET_CAM_BY_PROC={cam}",
                            f"-DCFG_ENABLE_FW_DOWNLOAD={firmware}",
                            f"-DCFG_TC10_FEATURE={firmware}",
                            str(fixture), "-o", str(binary)], check=True)
            cases = ["success", "debug", "bus", "wdev", "netdev", "adapter", "irq",
                     "start", "tx", "register", "thermo", "command", "cfg",
                     "proc-net", "proc-root", "proc-alloc", "proc-debug",
                     "proc-txdone", "proc-perf", "proc-country", "country-lock"]
            if threads:
                cases.append("rx")
            if cam:
                cases.append("cam")
            if firmware:
                cases.extend(("firmware", "mac"))
            if args.case:
                cases = [case for case in cases if case == args.case]
            if not cases:
                continue
            for case in cases:
                total += 1
                result = subprocess.run([str(binary), case], text=True, capture_output=True)
                if result.returncode:
                    failed.append((variant, case))
                    print(f"FAIL {variant} case={case}:\n{result.stderr[-5000:]}", flush=True)
            print(f"checked {len(cases)} cases ({variant})", flush=True)
    print(f"{total - len(failed)}/{total} cases pass; failures: {failed}", flush=True)
    return bool(failed) or total == 0


if __name__ == "__main__":
    raise SystemExit(main())
