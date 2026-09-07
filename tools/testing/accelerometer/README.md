These host regressions extract the production accelerometer/MIR3DA functions
from this kernel checkout and compile them with GCC, AddressSanitizer, and
UndefinedBehaviorSanitizer. They require Python 3, GCC, and a POSIX host with
pthreads. They use scripted kernel and bus boundaries and do not access a
device.

Run from the kernel root:

```sh
python3 tools/testing/accelerometer/test_accelerometer.py
python3 tools/testing/accelerometer/test_polling_lifetime.py
```

The first script covers 24 acquisition, diagnostic, and polling-period cases.
It preserves the production axis conversion and uses this kernel's 64-bit
division and clamp macros. The second script covers 11 polling-lifetime cases,
including 50 enable/disable cycles in its restart case.

Both accept `--kernel PATH`, `--revision COMMIT`, and `--keep-generated DIR`.
The lifetime script also accepts repeated `--case NAME` options. Use an earlier
revision to reproduce the assertions against its original function bodies;
failure is the expected result for an unfixed defect. Temporary executables
are removed unless `--keep-generated` is requested.

The lifetime fixture uses real pthreads and condition variables to pause the
production worker before its final timer decision, inside timer arming, or
inside a successful/failed sensor read. It also pauses a timer callback before
it queues work. The simulated timer cancellation waits for its callback, and
the simulated work cancellation removes pending work and waits for a running
worker, following `hrtimer_cancel()` and `cancel_work_sync()` in this tree.
Fixture scheduler state is synchronized; assertions bound waits and reports.

Against the source before the lifetime fix, the four worker races each leave
an armed timer after disable returns. Expiring that timer invokes the sensor
callback again. The fixture models MIR3DA's automatic power-on behavior and
records that additional power transition.
The fixed source leaves no timer, callback, pending work, or running work and
performs no sensor access after disable returns.

The production synchronization has two parts:

1. `acc_poll_lock` serializes closing `is_polling_run` with the final decision
   to arm the timer. An arm completed before closure is canceled; an attempted
   arm after closure is rejected. Timer target calculation remains outside
   this short critical section.
2. After releasing that lock, shutdown cancels the timer and then drains the
   work it could have queued. Power-off happens after both operations finish.
   Initial startup publishes polling and first-sample state before arming.

The immediate configuration callers are `acc_store_active()`,
`acc_store_enable_nodata()`, and `acc_store_batch()`. All call
`acc_enable_and_batch()` while holding `acc_op_mutex`, which serializes initial
startup and shutdown. Polling follows the active data request; power follows
the union of data and nodata requests. The regressions cover disabling nodata
while data remains active, disabling data while nodata remains active, and a
nodata-only request. They also verify that a failed power-off leaves polling
drained and can be retried.

These checks cover the sysfs polling lifecycle and its immediate callbacks.
Target builds, actual device cadence, kernel lock debugging, and full driver
probe/removal teardown are separate validation steps.
