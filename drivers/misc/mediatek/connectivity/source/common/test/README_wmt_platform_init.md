# WMT platform initialization unwind regression

`wmt_plat_init` previously discarded two actual failure results: OSAL wake source
allocation can return `-ENOMEM`, and the SoC helper returns the error from
`platform_driver_register`. The library consequently treated platform setup as
successful without its wake source or registered driver. After a driver
registration failure, later platform cleanup also attempted to unregister a
driver that had not been registered.

The platform constructor now checks those results and unwinds its own completed
stages before returning failure. This is required by the library lifecycle:
`wmt_lib_init` acquires platform ownership only after a successful return and its
failure cleanup therefore skips `wmt_plat_deinit`. Wake source and mutex setup and
hardware initialization precede callback publication. Stub registration rejection
unwinds the successful hardware setup and wake resources. Normal cleanup withdraws
callbacks first, selects the backend saved at successful initialization, and
releases each owned resource once. Duplicate init is rejected with `-EBUSY`;
cleanup before init or after completed cleanup does no work.

The direct stub unregistration helper also clears `cmb_stub_trigger_assert_cb`.
It previously cleared the other callback fields while leaving this one callable.
The regression retains the published assertion dispatcher before unregistration
and calls it afterward to show that it no longer invokes the old callback.

Run from the kernel tree using unused output directories:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_platform_init.py --kernel . --output /tmp/wmt-platform-candidate
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_platform_init.py --kernel . --revision 54bdf406ee9963e3b926d8f19fc2bb4c1a3975eb --output /tmp/wmt-platform-baseline
```

The runner extracts complete production functions from the platform constructor
and destructor, SoC hardware init/deinit and clock query, COMBO hardware
init/deinit, OSAL wake source and mutex constructors/destructors, stub registration
and unregistration, the assertion/thermal dispatchers, and the complete built-in
bridge section, including its table, synchronization state, helpers, and
dispatchers. Callback structures and types are extracted from their
production headers. Function bodies are not rewritten. A preprocessor alias
allows one wrapper to inject a stub API rejection by passing invalid input to the
actual stub registration function. Other faults occur at host substitutes for
`wakeup_source_register` and `platform_driver_register`.

Twenty-three ASan/UBSan cases cover SOC and COMBO successful setup, default,
custom, or invalid power timings, optional/configured clock selection, wake
allocation failure, SoC registration `-ENOMEM` and `-EBUSY`, framework registration
rollback, stub rejection for both backends, cleanup before init and repeated
cleanup, duplicate init, changed chip detection
after ownership is acquired, callback publication/withdrawal order, complete
callback withdrawal, invalid stub input, failure retry, and successful reinit.
Every case finishes with a successful initialization and cleanup cycle. Failure
cases require resources to be released before any caller-side cleanup, matching
the actual library contract. Output includes the fixture, compiler log, per-case
logs, and source/fixture/runner/host hashes in `result.json`.

The driver and wake allocation failures are real return paths in the extracted
callees. COMBO init only installs a timing structure and cannot fail in this tree;
valid stub registration also cannot fail its argument validation. Stub rejection
is explicitly an API-boundary injection to exercise the constructor's final
unwind branch. A negative clock-query result is an optional capability result and
remains nonfatal.

`platform_driver_register` reports driver registration, not successful device
probe. The fixture models a successful probe with an allocated coredump mapping
and models framework cleanup through its unregister adapter; it does not execute
the hardware-specific probe or test its independent resource/error paths. One
case models `driver_register` rolling back a registered driver after
`driver_add_groups` fails, so the failed constructor must not unregister it again.
The SoC helper's retained operations-table pointer after failed registration refers
to static storage and owns no allocation. Init/deinit are assumed serialized by
the outer WMT lifecycle. The bridge's host synchronization adapters check lock
pairing and require each drain to find no active callbacks in this sequential
fixture. Statically initialized bridge locks are not counted as platform-owned
mutex resources. The actual concurrent callback barrier is exercised by
`test_wmt_callback_lifetime.py`; this suite does not claim TSan coverage.
Platform probe cleanup remains separate work. These tests use no hardware and
make no flash, efuse, or patch writes.
