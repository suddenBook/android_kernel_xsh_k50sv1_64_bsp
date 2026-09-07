#!/usr/bin/env python3
"""Compare source overlay tools with a preserved prebuilt and actual K50 outputs."""

import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["old-dtc", "old-ufdt", "new-dtc", "new-ufdt", "baseline-output", "output"]:
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    for name in ["old_dtc", "old_ufdt", "new_dtc", "new_ufdt", "baseline_output", "output"]:
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    report = {"tools": {}, "commands": [], "checks": []}
    for name in ["old_dtc", "old_ufdt", "new_dtc", "new_ufdt"]:
        path = getattr(args, name)
        report["tools"][name] = {"path": str(path), "sha256": sha256(path)}

    def run(label, command):
        command = [str(part) for part in command]
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, errors="replace", timeout=15)
        (args.output / (label + ".stdout")).write_text(result.stdout)
        (args.output / (label + ".stderr")).write_text(result.stderr)
        report["commands"].append({"label": label, "argv": command, "rc": result.returncode})
        return result.returncode

    def check(label, condition, **details):
        report["checks"].append({"label": label, "pass": bool(condition), **details})
        print(("PASS " if condition else "FAIL ") + label, flush=True)
        if not condition:
            raise RuntimeError(label)

    try:
        baseline = args.baseline_output / "arch/arm64/boot/dts"
        dtbs = {}
        for name in ["mt6755", "k50sv1_64_bsp"]:
            source = baseline / ("." + name + ".dtb.dts.tmp")
            expected = baseline / (name + ".dtb")
            for generation in ["old", "new"]:
                output = args.output / (name + "." + generation + ".dtb")
                command = [getattr(args, generation + "_dtc"), "-@"]
                if generation == "new":
                    command += ["-H", "both"]
                command += ["-O", "dtb", "-b", "0", "-o", output, source]
                rc = run(name + "-" + generation, command)
                check(name + "-" + generation, rc == 0 and output.read_bytes() == expected.read_bytes(),
                      rc=rc, bytes=output.stat().st_size, sha256=sha256(output),
                      baseline_sha256=sha256(expected), input_sha256=sha256(source))
                dtbs[(name, generation)] = output

        # Check each merger against both old- and new-compiled input pairs.
        for merger in ["old", "new"]:
            for inputs in ["old", "new"]:
                label = "merge-" + merger + "-inputs-" + inputs
                output = args.output / (label + ".dtb")
                rc = run(label, [getattr(args, merger + "_ufdt"), dtbs[("mt6755", inputs)],
                                 dtbs[("k50sv1_64_bsp", inputs)], output])
                expected = baseline / "k50sv1_64_bsp.dtb.merge"
                check(label, rc == 0 and output.read_bytes() == expected.read_bytes(),
                      rc=rc, bytes=output.stat().st_size, sha256=sha256(output),
                      baseline_sha256=sha256(expected))

        bad_dts = args.output / "invalid.dts"
        bad_dts.write_text("/dts-v1/;\n/ { broken = < ; };\n")
        bad_dtb = args.output / "invalid.dtb"
        bad_dtb.write_bytes(b"not a device tree" * 4)
        missing = args.output / "file-does-not-exist"
        rejected = args.output / "rejected.dtb"
        existing_directory = args.output / "directory"
        existing_directory.mkdir(exist_ok=True)
        cases = [
            ("dtc-bad-option", "dtc", ["--not-a-dtc-option"]),
            ("dtc-missing-input", "dtc", ["-O", "dtb", "-o", rejected, missing]),
            ("dtc-invalid-input", "dtc", ["-O", "dtb", "-o", rejected, bad_dts]),
            ("dtc-output-directory", "dtc", ["-I", "dtb", "-O", "dtb", "-o", existing_directory,
                                             dtbs[("mt6755", "old")]]),
            ("ufdt-usage", "ufdt", []),
            ("ufdt-missing-base", "ufdt", [missing, dtbs[("k50sv1_64_bsp", "old")], rejected]),
            ("ufdt-missing-overlay", "ufdt", [dtbs[("mt6755", "old")], missing, rejected]),
            ("ufdt-invalid-base", "ufdt", [bad_dtb, dtbs[("k50sv1_64_bsp", "old")], rejected]),
            ("ufdt-output-directory", "ufdt", [dtbs[("mt6755", "old")],
                                                dtbs[("k50sv1_64_bsp", "old")], existing_directory]),
        ]
        for label, tool, operands in cases:
            results = {generation: run(label + "-" + generation,
                                       [getattr(args, generation + "_" + tool), *operands])
                       for generation in ["old", "new"]}
            check(label, results["old"] != 0 and results["new"] == results["old"], **results)

        # A small valid merge reaches stdio's delayed fclose() error on /dev/full.
        small_base = args.output / "small-base.dts"
        small_overlay = args.output / "small-overlay.dts"
        bad_overlay = args.output / "unresolved-overlay.dts"
        small_base.write_text("/dts-v1/;\n/ { target: node { original = <1>; }; };\n")
        small_overlay.write_text("/dts-v1/;\n/plugin/;\n/ { fragment@0 { target = <&target>; "
                                 "__overlay__ { added = <2>; }; }; };\n")
        bad_overlay.write_text(small_overlay.read_text().replace("<&target>", "<&missing>"))
        for source in [small_base, small_overlay, bad_overlay]:
            rc = run(source.stem, [args.new_dtc, "-@", "-H", "both", "-O", "dtb",
                                  "-o", source.with_suffix(".dtb"), source])
            check(source.stem + "-compile", rc == 0, rc=rc)
        for label, overlay, output in [
            ("ufdt-unresolved-symbol", bad_overlay.with_suffix(".dtb"), rejected),
            ("ufdt-delayed-write-error", small_overlay.with_suffix(".dtb"), Path("/dev/full")),
        ]:
            results = {generation: run(label + "-" + generation,
                                       [getattr(args, generation + "_ufdt"),
                                        small_base.with_suffix(".dtb"), overlay, output])
                       for generation in ["old", "new"]}
            check(label, results["new"] == 1, **results)
        report["passed"] = True
    finally:
        (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
