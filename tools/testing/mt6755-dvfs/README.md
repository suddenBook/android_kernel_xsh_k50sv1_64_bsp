Run from the kernel root with Python 3, GCC, and pthreads:

```sh
python3 tools/testing/mt6755-dvfs/test_dvfs.py
```

The host harness extracts the production manager functions and governor
dispatch, including the actual kicker group masks and aggregation. Only the
SPM/hardware, kernel locking, and unused AutoK boundaries are simulated. GCC
enables AddressSanitizer and UndefinedBehaviorSanitizer.

Cases cover repeated `-1`, `-EBUSY`, and partial errors; a successful request
from another group between retries; failed release; and concurrent same-group
votes. Masked/AutoK-locked requests also verify mutex release on rejection.
The concurrent case uses pthreads to hold one transition in the SPM
boundary while another caller waits for the manager mutex. A waiting request
must not mutate the in-flight vote table; releasing one consumer must retain
the other's HPM vote.

Use `--case NAME`, `--revision COMMIT`, and `--keep-generated DIR` to select
cases, demonstrate failures with earlier production code, or retain the
generated fixture and executable. These host checks do not validate SPM
firmware, PMIC/PLL timing, or target kernel integration; those require a
target build and device testing.
