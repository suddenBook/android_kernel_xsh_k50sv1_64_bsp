#!/usr/bin/env python3
"""Run the complete K50 flashlight driver against fake PMIC, timer and work APIs."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

tests = Path(__file__).resolve().parent
root = tests.parents[2]
driver = root / "drivers/misc/mediatek/flashlight/src/mt6755/constant_flashlight/leds_strobe.c"
body = re.sub(r'^#include .*$', '', driver.read_text(), flags=re.M)
dispatcher_path = root / "drivers/misc/mediatek/flashlight/src/mt6755/kd_flashlightlist.c"
dispatcher = dispatcher_path.read_text()


def function(name):
    start = re.search(r'^(?:static )?(?:int|long) ' + name + r'\(', dispatcher, re.M).start()
    end = dispatcher.index('\n}', start) + 2
    return dispatcher[start:end] + '\n'


selector = ''.join(function(name) for name in
                   ("getSensorDevIndex", "getStrobeIndex", "getPartIndex", "setFlashDrv",
                    "flashlight_ioctl_core"))
with tempfile.TemporaryDirectory(prefix="k50-flashlight-") as tmp:
    source = Path(tmp) / "test.c"
    source.write_text((tests / "host.h").read_text() + body + selector + (tests / "test.c").read_text())
    executable = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-O1", "-pthread",
                    "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(root / "drivers/misc/mediatek/flashlight/inc"),
                    "-I", str(root / "drivers/misc/mediatek/include/mt-plat/mt6755/include"),
                    str(source), "-o", str(executable)], check=True)
    raise SystemExit(subprocess.run([str(executable)], timeout=20).returncode)
