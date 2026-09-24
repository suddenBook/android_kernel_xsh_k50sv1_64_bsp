# K50 ISINK shared-clock regression

Run from the kernel root with a host C compiler:

```sh
python3 tools/testing/k50-isink-clock/run.py --output-dir /path/to/bringup/scratch/isink-clock
```

The runner extracts the actual MT6353 `enable_dummy_load()` implementation,
K50 flash initialization table and hardware helpers, and their enum-to-register
entries from `upmu_common.c`. It compiles them with the real PMIC register header
against a masked register model under AddressSanitizer and UndefinedBehaviorSanitizer.
Generated sources and the executable remain in the supplied output directory.

Five scenarios cover torch activation before and during a dummy-load cycle,
repeated cleanup, an idle dummy cycle, and cleanup with either initial shared-clock
state. They verify that ISINK0/1 and their configuration survive, while all eleven
private-channel/CHRIND cleanup writes retain their order, values and register effects.

The shared driver clock follows the other clients' ungate-only convention. A
dummy-load cycle therefore leaves it enabled even if it was initially gated.
These tests do not measure standby power, PMIC electrical behavior or emitted light.
