#!/usr/bin/env python3
"""Run complete STP teardown functions against tracked host resources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

from test_wmt_outer_init import function

CASES = ["pending-debug-timers-and-work", "running-debug-timers",
         "running-debug-work", "debug-without-log-allocation",
         "btm-join-retains-psm-and-debug", "complete-stp-and-repeated-exit",
         "partial-psm-only", "partial-btm-without-debug", "running-stp-tx-timer",
         "exit-before-stp-init"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision")
    args = parser.parse_args()
    base = Path("drivers/misc/mediatek/connectivity/source/common/common_main")
    paths = [base / p for p in ["core/btm_core.c", "core/stp_core.c", "linux/stp_dbg.c"]]
    texts = [subprocess.check_output(["git", "show", args.revision + ":" + str(p)], cwd=args.kernel).decode()
             if args.revision else (args.kernel / p).read_text() for p in paths]
    extracted = []
    for text, names in zip(texts, [
            ["stp_btm_stop_thread", "stp_btm_deinit"],
            ["stp_deinit_locked", "mtk_wcn_stp_deinit"],
            ["stp_dbg_core_dump_deinit", "stp_dbg_deinit"]]):
        for name in names:
            if name not in text and name in {"stp_btm_stop_thread", "stp_deinit_locked"}:
                continue
            extracted.append(function(text, name))
    host_path = Path(__file__).with_name("stp_teardown_host.c")
    fixture = host_path.read_text().replace("/* PROTOTYPES */", "\n".join(
        value[:value.index("\n{")] + ";" for value in extracted))
    fixture = fixture.replace("/* PRODUCTION */", "\n\n".join(extracted))
    args.output.mkdir(parents=True, exist_ok=False)
    c_file, binary = args.output / "fixture.c", args.output / "fixture"
    c_file.write_text(fixture)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-variable",
        "-pthread", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-fno-omit-frame-pointer", "-no-pie", str(c_file), "-o", str(binary)]
    compiled = subprocess.run(command, capture_output=True)
    (args.output / "compile.txt").write_bytes(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    rows = []
    for i, name in enumerate(CASES):
        run = subprocess.run([str(binary), str(i)], capture_output=True, timeout=10,
                             env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"))
        (args.output / (name + ".txt")).write_bytes(run.stdout + run.stderr)
        rows.append({"case": name, "pass": run.returncode == 0, "exit_code": run.returncode})
        print(("PASS" if run.returncode == 0 else "FAIL") + ": " + name, flush=True)
    result = {"source_sha256": {str(p): hashlib.sha256(t.encode()).hexdigest() for p, t in zip(paths, texts)},
              "host_sha256": hashlib.sha256(host_path.read_bytes()).hexdigest(),
              "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "fixture_sha256": hashlib.sha256(c_file.read_bytes()).hexdigest(),
              "compiler": command, "cases": rows, "total": len(rows), "passed": sum(r["pass"] for r in rows)}
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"{result['passed']}/{result['total']} passed", flush=True)
    raise SystemExit(result["passed"] != result["total"])


if __name__ == "__main__":
    main()
