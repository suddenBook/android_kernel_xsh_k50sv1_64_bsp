Run from the kernel root with Python 3, GCC, and pthreads:

```sh
python3 tools/testing/mt6755-dvfs/test_dvfs.py
```

The 17 host cases extract the production manager functions, governor dispatch
and group aggregation, GPU voltage-enable/voltage-switch/target functions,
and OPP tables. SPM, PLL/PMIC, PBM, kernel locking, and unused AutoK boundaries
are simulated. GCC enables AddressSanitizer and UndefinedBehaviorSanitizer.

Cases cover repeated `-1`, `-EBUSY`, and partial errors; a successful request
from another group between retries; failed release; and concurrent same-group
votes. Masked/AutoK-locked requests also verify mutex release on rejection.
The concurrent case uses pthreads to hold one transition in the SPM
boundary while another caller waits for the manager mutex. A waiting request
must not mutate the in-flight vote table; releasing one consumer must retain
the other's HPM vote.

GPU cases cover off → another consumer's HPM vote → on → that consumer's
release at 676 and 520 MHz. Successful reacquisition retains the clock and
cached OPP. Failed reacquisition must return the original error, fall back
to 351 MHz regardless of the sampled voltage, synchronize cached metadata,
and release the failed vote after lowering the PLL. Cases also cover failed
cleanup, the helper's positive `0x7f` error, recovery via `mt_gpufreq_target`,
and power-off cleanup after a failed upward or downward transition. Downward
clock transitions retain truthful cache updates even if voltage release fails.

Use `--case NAME`, `--revision COMMIT`, and `--keep-generated DIR` to select
cases, demonstrate failures with earlier production code, or retain the
generated fixture and executable. These host checks do not validate SPM
firmware, PMIC/PLL timing, or target kernel integration; those require a
target build and device testing.
