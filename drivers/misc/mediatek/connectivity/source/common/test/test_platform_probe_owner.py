#!/usr/bin/env python3
"""Exercise the actual platform core helpers with caller-owned module identities."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

CASES = ["module-bound", "builtin-bound", "module-unbound", "builtin-unbound",
         "module-registration-error", "builtin-registration-error",
         "module-retry", "distinct-module-owners"]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--git-revision")
    args = parser.parse_args()
    kernel = args.kernel.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source_name = "drivers/base/platform.c"
    source = (subprocess.check_output(["git", "show", f"{args.git_revision}:{source_name}"], cwd=kernel)
              if args.git_revision else (kernel / source_name).read_bytes())
    helpers = "\n\n".join(function(source.decode(), signature) for signature in [
        "int __platform_driver_register(", "void platform_driver_unregister(",
        "int __init_or_module platform_driver_probe("])
    fixture = Path(__file__).with_name("platform_probe_owner_host.c")
    combined = fixture.read_text().replace("/* ACTUAL_PLATFORM_HELPERS */", helpers)
    generated = out / "test.c"
    generated.write_text(combined)
    command = ["clang", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
               "-fno-omit-frame-pointer", "-no-pie", "-fsanitize=address,undefined",
               str(generated), "-o", str(out / "test")]
    build = subprocess.run(command, capture_output=True)
    (out / "compile.log").write_bytes(build.stdout + build.stderr)
    result = dict(source=source_name, source_sha256=digest(source),
                  revision=args.git_revision or "working-tree", command=command,
                  fixture_sha256=digest(fixture.read_bytes()),
                  runner_sha256=digest(Path(__file__).read_bytes()),
                  compile_exit_code=build.returncode, cases=[])
    if not build.returncode:
        for case in CASES:
            run = subprocess.run([str(out / "test"), case], capture_output=True, timeout=10,
                                 env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                                          UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
            (out / (case + ".txt")).write_bytes(run.stdout + run.stderr)
            result["cases"].append(dict(case=case, exit_code=run.returncode))
    result["passed"] = sum(row["exit_code"] == 0 for row in result["cases"])
    result["total"] = len(CASES)
    result["status"] = "PASS" if not build.returncode and result["passed"] == len(CASES) else "FAIL"
    result["artifacts"] = {p.name: digest(p.read_bytes()) for p in sorted(out.iterdir()) if p.is_file()}
    result["boundary"] = "Actual core registration/probe/unregister helpers; deterministic driver-core and list adapters, no hardware or module unloading."
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: result[k] for k in ["status", "passed", "total", "source_sha256"]}))
    if build.returncode:
        print((out / "compile.log").read_text())
    return result["status"] != "PASS"


if __name__ == "__main__":
    raise SystemExit(main())
