# PSM resource lifetime regression

The runner extracts the production PSM initialization, cleanup, timer admission
and all four diagnostic helpers, plus the actual OSAL thread/FIFO/wakeup-source
constructors. Host replacements provide allocation failures, kernel-thread and
timer scheduling, and an RCU reader/grace-period model.

Run the directed checks and concurrency checks separately:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_psm_lifecycle.py --kernel . --output /tmp/psm-directed
python3 drivers/misc/mediatek/connectivity/source/common/test/test_psm_lifecycle.py --kernel . --sanitizer thread --output /tmp/psm-threads
```

The 22 directed cases cover successful setup, full release, allocation failures
at each resource, thread-creation errors, retry, optional history failure,
publication before worker start, duplicate/null calls, timer rearming during
shutdown, repeated initialization, and paused worker/timer/external diagnostic
readers. The seven ThreadSanitizer cases cover concurrent initialization and
deinitialization, timer shutdown, and all four external diagnostic helpers.

The fixture schedules external readers after obtaining a record but before
taking its record lock. Cleanup must withdraw the records and wait for those
readers before destroying their locks and freeing their memory. The worker and
timer cases separately require synchronous stop/join before resources go away.
The baseline at cd4c037ecd7 passes 1/22 directed cases; the fixed source passes
22/22 directed and 7/7 concurrent cases. Source, fixture and compiler hashes are
recorded in each result.json; raw failures remain available beside the result.

The host RCU model establishes the exercised ordering; it does not emulate the
kernel scheduler or prove all external STP TX/FIFO callers have been quiesced.
Those callers still require the owning driver's normal teardown discipline.
Actual ARM64 compilation and installed radio/boot checks are separate evidence.
