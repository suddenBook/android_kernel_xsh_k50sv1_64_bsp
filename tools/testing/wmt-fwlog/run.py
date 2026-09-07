#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Extract complete collector functions and exercise host lifecycle adapters."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

BASE = "372a643505f6b0aab0b3adbd150ecb9d9291d8d9"
COMMON = "drivers/misc/mediatek/connectivity/source/common/common_main/"
CASES = [
    "ioctl-empty", "ioctl-progress", "one-index-snapshot", "full-ring-chunks",
    "wrap-reserved-byte", "invalid-producer-indices", "allocation-failure",
    "missing-info", "missing-control", "missing-trace", "invalid-manual-arguments",
    "manual-clamped-length", "manual-wrap", "manual-end-offset", "disabled-at-entry",
    "pending-signal", "pending-fatal-signal", "signal-during-mapping",
    "disable-between-chunks", "signal-between-chunks", "signal-during-sleep",
    "disable-during-sleep", "unmap-between-passes", "interrupt-waiting-lock",
    "ioctl-during-proc-stream", "signal-held-stream",
]
BASELINE_CASES = ["ioctl-empty", "allocation-failure", "missing-control",
                  "invalid-producer-indices", "full-ring-chunks", "pending-signal"]
THREAD_CASES = ["interrupt-waiting-lock", "ioctl-during-proc-stream", "signal-held-stream"]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def function(source, name):
    pattern = re.compile(r"^[A-Za-z_][\w \t*]*\b" + re.escape(name) + r"\s*\(", re.M)
    for match in pattern.finditer(source):
        position = match.end()
        depth = 1
        while depth:
            depth += (source[position] == "(") - (source[position] == ")")
            position += 1
        while source[position].isspace():
            position += 1
        if source[position] != "{":
            continue
        start = match.start()
        depth, state = 0, "code"
        while position < len(source):
            char = source[position]
            pair = source[position:position + 2]
            if state == "line":
                if char == "\n":
                    state = "code"
            elif state == "comment":
                if pair == "*/":
                    state = "code"
                    position += 1
            elif state in ('"', "'"):
                if char == "\\":
                    position += 1
                elif char == state:
                    state = "code"
            elif pair == "/*":
                state = "comment"
                position += 1
            elif pair == "//":
                state = "line"
                position += 1
            elif char in ('"', "'"):
                state = char
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start:position + 1] + "\n", source.count("\n", 0, start) + 1
            position += 1
    raise ValueError("Complete function not found: " + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--small-buffer", action="store_true")
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--thread", action="store_true")
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repo = here.parents[2]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sources = {}
    for suffix in ["linux/wmt_dbg.c", "linux/osal.c", "core/wmt_lib.c"]:
        path = COMMON + suffix
        data = subprocess.check_output(["git", "-C", str(repo), "show", BASE + ":" + path]) if args.baseline else (repo / path).read_bytes()
        sources[path] = data.decode()
    debug_path = COMMON + "linux/wmt_dbg.c"
    debug = sources[debug_path]
    units = []

    def add(path, name):
        body, line = function(sources[path], name)
        units.append({"path": path, "name": name, "start_line": line,
                      "sha256": digest(body.encode()), "text": body})

    begin = debug.index("#define BUF_LEN_MAX")
    end = debug.index("#endif", begin) + len("#endif")
    declarations = debug[begin:end] + "\n"
    for name in ["gEmiBuf", "buf_emi", "g_dbg_emi_lock"]:
        line = re.search(r"^[^\n]*\b" + name + r"(?:\[[^\n]*\])?;\s*$", debug, re.M)
        assert line, name
        declarations += line.group(0).rstrip() + "\n"
    add(COMMON + "linux/osal.c", "osal_lock_sleepable_lock")
    add(COMMON + "linux/osal.c", "osal_unlock_sleepable_lock")
    add(COMMON + "core/wmt_lib.c", "wmt_lib_get_fwinfor_from_emi")
    add(debug_path, "wmt_dbg_fwinfor_print_buff")
    if not args.baseline:
        add(debug_path, "wmt_dbg_fwinfor_trace_to")
    add(debug_path, "wmt_dbg_fwinfor_from_emi")
    fixture = declarations + "\n" + "\n".join(unit["text"] for unit in units)
    (output / "fixture.h").write_text(fixture)
    shutil.copyfile(here / "test_host.c", output / "test_host.c")
    for unit in units:
        assert fixture.count(unit["text"]) == 1, unit["name"]
    cc = shutil.which(args.cc)
    assert cc, args.cc
    command = [cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-Wno-unused-parameter", "-pthread", "-fno-omit-frame-pointer",
               "-fsanitize=" + ("thread" if args.thread else "address,undefined"),
               "-fno-sanitize-recover=all", "-I", str(output), str(output / "test_host.c"),
               "-o", str(output / "test_host")]
    if args.small_buffer:
        command += ["-DCONFIG_MTK_GMO_RAM_OPTIMIZE=1"]
    if args.thread:
        command += ["-fno-pie", "-no-pie"]
    result = subprocess.run(command, text=True, capture_output=True, timeout=60)
    (output / "compile.stdout").write_text(result.stdout)
    (output / "compile.stderr").write_text(result.stderr)
    manifest = {"repository": str(repo), "base_revision": BASE, "baseline_control": args.baseline,
                "small_buffer": args.small_buffer, "thread_sanitizer": args.thread,
                "source_files": [{"path": p, "sha256": digest(t.encode())} for p, t in sources.items()],
                "source_units": units, "fixture_sha256": digest(fixture.encode()),
                "host_sha256": digest((here / "test_host.c").read_bytes()),
                "runner_sha256": digest(Path(__file__).read_bytes()),
                "compiler": subprocess.check_output([cc, "--version"], text=True),
                "compile_command": command, "compile_returncode": result.returncode,
                "cases": []}
    cases = BASELINE_CASES if args.baseline else THREAD_CASES if args.thread else CASES
    environment = os.environ.copy()
    options = {"ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
               "TSAN_OPTIONS": "halt_on_error=1:exitcode=66"}
    environment.update(options)
    manifest["sanitizer_options"] = options
    if result.returncode == 0:
        for name in cases:
            run_command = [str(output / "test_host"), name]
            try:
                result = subprocess.run(run_command, env=environment, text=True,
                                        capture_output=True, timeout=8)
                code, stdout, stderr = result.returncode, result.stdout, result.stderr
            except subprocess.TimeoutExpired as error:
                code, stdout, stderr = 124, (error.stdout or b"").decode(), (error.stderr or b"").decode()
            (output / (name + ".stdout")).write_text(stdout)
            (output / (name + ".stderr")).write_text(stderr)
            manifest["cases"].append({"name": name, "command": run_command, "returncode": code,
                                       "passed": code == 0,
                                       "stdout_sha256": digest(stdout.encode()), "stderr_sha256": digest(stderr.encode())})
    manifest["passed_cases"] = sum(case["passed"] for case in manifest["cases"])
    manifest["completed_cases"] = len(manifest["cases"])
    manifest["expected_behavior_verified"] = (result.returncode == 0 and len(manifest["cases"]) == len(cases)
        and all(not case["passed"] if args.baseline else case["passed"] for case in manifest["cases"]))
    # For negative controls the final test is intentionally unsuccessful.
    if args.baseline:
        manifest["expected_behavior_verified"] = (manifest["compile_returncode"] == 0
            and len(manifest["cases"]) == len(cases)
            and all(not case["passed"] and case["returncode"] != 124 for case in manifest["cases"]))
    path = output / "results.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"output": str(output), "passed_cases": manifest["passed_cases"],
                      "completed_cases": manifest["completed_cases"],
                      "compile_returncode": manifest["compile_returncode"],
                      "expected_behavior_verified": manifest["expected_behavior_verified"],
                      "results_sha256": digest(path.read_bytes())}))
    raise SystemExit(0 if manifest["expected_behavior_verified"] else 1)


if __name__ == "__main__":
    main()
