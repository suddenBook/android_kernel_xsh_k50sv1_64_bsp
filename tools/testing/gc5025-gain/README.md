# GC5025 gain regression

Run from the kernel root, with an output path under the bringup scratch tree:

```sh
python3 tools/testing/gc5025-gain/run.py ../../bringup/scratch/gc5025-gain
```

The test extracts and compiles the current driver's gain constants and
`set_gain()` without rewriting the function. The mock applies the measured
four-bit `b1` readback behavior. It checks all inputs 1–65535 for monotonic
encoded gain, valid register writes, unity/analog-transition values, and
saturation at the maximum safe encoding across inputs 1471/1472. It needs
Python 3 and a host C compiler with UBSan; `CC` overrides the compiler.

Generated C, binary and results stay in the supplied output directory. This
checks register arithmetic; it does not model image brightness or sensor I2C.
