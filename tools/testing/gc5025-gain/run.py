#!/usr/bin/env python3
"""Compile the current GC5025 set_gain() with a register-write mock."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path, help="build/log directory under bringup/scratch")
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
driver = root / "drivers/misc/mediatek/imgsensor/src/mt6755/gc5025_mipi_raw/gc5025mipi_Sensor.c"
source = driver.read_text()
start = source.index("#define ANALOG_GAIN_1")
end = source.index("static void ihdr_write_shutter_gain", start)
gain_code = source[start:end]
assert gain_code.count("static kal_uint16 set_gain(") == 1

prefix = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint16_t kal_uint16;
typedef uint32_t kal_uint32;
#define LOG_INF(...) ((void)0)
static unsigned regs[256], raw_b1, writes, invalid_writes;
static void write_cmos_sensor(kal_uint16 address, kal_uint16 value)
{
    unsigned bit;
    switch (address) {
    case 0xfe: bit = 1; break;
    case 0xb6: bit = 2; break;
    case 0xb1: bit = 4; break;
    case 0xb2: bit = 8; break;
    default: invalid_writes++; return;
    }
    writes |= bit;
    /* Real readback proves b1 retains only its low four bits. */
    if (address == 0xb1) {
        raw_b1 = value;
        regs[address] = value & 0x0f;
    } else {
        regs[address] = value & 0xff;
    }
}
'''

tests = r'''
int main(void)
{
    unsigned previous = 0, drops = 0, invalid = 0, overflow = 0, anchors = 0;
    for (unsigned gain = 1; gain <= UINT16_MAX; gain++) {
        memset(regs, 0, sizeof(regs));
        raw_b1 = writes = invalid_writes = 0;
        (void)set_gain((kal_uint16)gain);
        unsigned digital = (regs[0xb1] << 6) | (regs[0xb2] >> 2);
        unsigned effective = digital * (regs[0xb6] ? 92 : 64);
        if (invalid_writes || writes != 15 || regs[0xfe] != 0 ||
            regs[0xb6] > 1 || (regs[0xb2] & 3))
            invalid++;
        if (raw_b1 > 0x0f) {
            if (!overflow)
                printf("FAIL b1 overflow: gain=%u b1=0x%x\n", gain, raw_b1);
            overflow++;
        }
        if (effective < previous) {
            if (!drops)
                printf("FAIL monotonic: gain=%u effective %u -> %u (units 1/4096x)\n",
                       gain, previous, effective);
            drops++;
        }
        /* Preserve unity, the analog transition, and the full safe ceiling. */
        if ((gain <= 64 && (digital != 64 || regs[0xb6] != 0)) ||
            (gain == 91 && (digital != 91 || regs[0xb6] != 0)) ||
            (gain == 92 && (digital != 64 || regs[0xb6] != 1)) ||
            (gain >= 1471 && (digital != 1023 || regs[0xb6] != 1)))
            anchors++;
        if (gain == 1471 || gain == 1472 || gain == 1536 || gain == UINT16_MAX)
            printf("gain=%u b1-write=0x%x b1-read=0x%x b2=0x%x b6=%u digital=%u effective=%.8fx\n",
                   gain, raw_b1, regs[0xb1], regs[0xb2], regs[0xb6], digital,
                   effective / 4096.0);
        previous = effective;
    }
    printf("GC5025 gain 1..65535: drops=%u b1-overflow=%u invalid-writes=%u anchor-failures=%u\n",
           drops, overflow, invalid, anchors);
    return drops || overflow || invalid || anchors;
}
'''

args.output.mkdir(parents=True, exist_ok=True)
generated = args.output.resolve() / "gain-test.c"
binary = args.output.resolve() / "gain-test"
generated.write_text(prefix + gain_code + tests)
subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra",
                "-Werror", "-fsanitize=undefined", str(generated), "-o", str(binary)],
               check=True)
result = subprocess.run([str(binary)], capture_output=True, text=True)
report = (f"driver={driver}\nsource-sha256={hashlib.sha256(source.encode()).hexdigest()}\n"
          f"set-gain-sha256={hashlib.sha256(gain_code.encode()).hexdigest()}\n"
          + result.stdout + result.stderr)
(args.output / "result.txt").write_text(report)
print(report, end="")
raise SystemExit(result.returncode)
