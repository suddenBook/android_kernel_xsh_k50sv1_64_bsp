# Outer WMT initialization failure regression

`test_wmt_outer_init.py` extracts the unmodified production `WMT_init()` and
`WMT_exit()` bodies, initialization status enum, and principal globals from
`common_main/linux/wmt_dev.c`. It also extracts the debug proc setup/remove,
asynchronous submission/callback, and function-control handler from `wmt_dbg.c`.
The host fixture supplies tracked constructors,
destructors, callback registrations, and failure results. Each acquired resource
has an allocation, and cleanup rejects releases without ownership or with live
dependants. AddressSanitizer, UndefinedBehaviorSanitizer, and leak detection run
for every case.

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_outer_init.py --kernel . --output /tmp/wmt-outer-init
```

The four build variants cover dynamic/static character-device nodes and locally
initialized/externally initialized transports, with both display notification
configurations. The 76 cases exercise successful SoC/combo initialization and
exit, duplicate initialization, exit before initialization, every enabled failure
stage, retry after each failure, and sequential failures followed by success.
Assertions check the original error result, no success notification, restored
initialization status, retained major number for retry, dependency-ordered cleanup
order, and no calls to later constructors or unstarted cleanup functions.

The active configuration acquires HIF SDIO, STP, a character-device region,
cdev, class, and device before starting the WMT library. Failed HIF registration
does not invoke HIF exit; failed STP initialization only releases the locally
owned HIF registration. Failures in later stages unwind the completed stages.
Externally initialized HIF remains owned by its caller after failed WMT startup.

The library constructor can retain partial resources after failure. The fixture
requires `wmt_lib_deinit_prepare()` before STP exit, retaining library resources
until `wmt_lib_deinit_finish()`. Earlier failures that never attempted library
initialization must not invoke these phases. STP exit occurs exactly once on
each path. Separate library/STP tests exercise their internal barriers.

Publication checks require work, STEP and proc locks to exist before callbacks
can be reached. Exit checks require proc/display withdrawal before cancellation
of power work, debug queue drain and STEP teardown before final power-off, and
retention of library resources while STP is shut down. A held display callback
and a final proc submission model work left at the withdrawal boundary.

The pending-debug case submits two work objects through the actual debug
submission function. They execute the actual callback and function-control
handler when the private queue is destroyed, before library or STEP teardown.
The fixture models a separate system queue, so the old `schedule_work()` path
leaves both requests pending and fails. Work and proc adapters model the kernel
barrier contracts; this test does not emulate the kernel scheduler or physically
power off hardware. The exit power-failure case verifies continued cleanup and
does not claim that a failed shutdown succeeded.

The current source passes 76/76 cases. Restoring only the original debug
submission function from `69faa749fff`, while retaining the fixed publication
and cleanup prerequisites, fails the four pending-debug variants (72/76 pass).
`result.json` records source/fixture hashes, compiler commands, and every case
result. The same runner accepts `--kernel` pointing to a baseline checkout.
Real ARM64 compilation with the product command and `-Werror` is separate
evidence from these host checks.

The optional `--legacy-library-step-cleanup` flag is retained for historical
baseline comparisons predating outer ownership of STEP cleanup.
