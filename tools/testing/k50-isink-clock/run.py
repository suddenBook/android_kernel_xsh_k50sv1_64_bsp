#!/usr/bin/env python3
"""Exercise the production MT6353 dummy load and K50 flash against a register model."""
import argparse
import os
from pathlib import Path
import re
import subprocess


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-dir", type=Path, required=True,
                    help="directory for generated sources and the host executable")
args = parser.parse_args()
output = args.output_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
tests = Path(__file__).resolve().parent
root = tests.parents[2]
mediatek = root / "drivers/misc/mediatek"
flash = (mediatek / "flashlight/src/mt6755/constant_flashlight/leds_strobe.c").read_text()
dlpt = (mediatek / "pmic/mt6353/pmic_throttling_dlpt.c").read_text()
fields = (mediatek / "pmic/mt6353/upmu_common.c").read_text()


def function(source, name):
    match = re.search(r"^(?:static )?(?:int|void) " + name + r"\([^;]*?\)\n\{",
                      source, re.M)
    if match is None:
        raise ValueError(f"Production function missing: {name}")
    end = source.index("\n}", match.start()) + 2
    return source[match.start():end] + "\n"


# Select the MT6353 implementation, not the following MT6351 alternative.
mt6353 = re.search(r"#if defined\(CONFIG_MTK_PMIC_CHIP_MT6353\)\n"
                   r"/\* for jade minus \*/\n(.*?)\n#else", dlpt, re.S)
if mt6353 is None:
    raise ValueError("MT6353 dummy-load branch missing")
table_start = flash.index("struct flash_reg {")
table_end = flash.index("\n};", flash.index("flash_init_regs[]")) + 3
production = flash[table_start:table_end] + "\n"
production += "".join(function(flash, name) for name in
                      ("flash_write", "flash_disable", "flash_enable", "flash_init"))
production += function(mt6353.group(1), "enable_dummy_load")

# Preserve the actual enum-to-register table entries used by these functions.
entries = []
for flag in sorted(set(re.findall(r"\bPMIC_[A-Z0-9_]+\b", production))):
    entry = re.search(r"\{\s*" + flag + r"\s*,[^{}]+\}", fields)
    if entry is None:
        raise ValueError(f"Production PMIC field missing: {flag}")
    entries.append(entry.group())
(output / "pmic_fields.inc").write_text(
    "static const PMU_FLAG_TABLE_ENTRY fields[] = {\n" +
    ",\n".join(entries) + "\n};\n")
(output / "production.inc").write_text(production)
executable = output / "test"
subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-O1",
                "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-I", str(output), "-I",
                str(mediatek / "include/mt-plat/mt6755/include"),
                str(tests / "test.c"), "-o", str(executable)], check=True)
raise SystemExit(subprocess.run([str(executable)], timeout=20).returncode)
