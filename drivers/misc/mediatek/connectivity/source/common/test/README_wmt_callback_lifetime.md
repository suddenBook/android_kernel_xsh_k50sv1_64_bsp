# WMT platform callback lifetime regression

The k50sv1_64_bsp configuration builds `connadp` and the WMT thermal zone into
the kernel while `wmt_drv` is a module. Reading the thermal zone's `temp` sysfs
attribute reaches `wmt_thz_get_temp` in `mtk_ts_wmt.c`, then the built-in
`mtk_wcn_cmb_stub_query_ctrl`, the module's `_mtk_wcn_cmb_stub_query_ctrl`,
`wmt_plat_thermal_ctrl`, and `wmt_dev_tm_temp_query`. The thermal callback can
remain on a CPU after its individual WMT operations have returned their pool
references. This path takes no module reference on entry. The thermal zone's
default polling interval is zero; the sysfs path does not depend on polling.

Clearing a function pointer does not wait for an already admitted callback.
Previously, `WMT_exit` destroyed the temperature lock before unregistering the
bridge, and library cleanup could destroy resources while a module callback
was still returning. Operation-pool draining alone cannot close that interval.

The built-in bridge now snapshots the complete callback table and acquires an
active reference under an IRQ-saving spinlock. It releases that reference only
after the module callback has returned to built-in code. Unregistration clears
the table under that same spinlock and then waits for the count to reach zero.
A writer mutex prevents registration from reopening admission during the wait;
replacement registration also drains the previous table before publishing.
No callback runs under the spinlock or the writer mutex. `WMT_exit` drains
before its first resource destructor, and `wmt_lib_deinit_prepare` drains before closing
the operation pool or stopping workers, including initialization failure unwind.
Later platform unregistration and repeated cleanup can safely drain again.

Registration and unregistration require sleepable context and must not be
called from a callback they would wait for. Nested dispatch remains supported.
The WMT owner still serializes its initialization and teardown through resource
destruction; the writer mutex protects bridge publication, not that entire
owner lifecycle. This protects the three built-in bridge callbacks. It does
not add references to unrelated direct WMT APIs or make arbitrary simultaneous
changes to the module's private callback tables safe.

Callback context was checked against this source tree. Thermal commands can
wait for worker completion and power-save coordination. Assertion handling
reaches sleepable debug locks through `wmt_ctrl_trg_assert` and
`stp_dbg_set_keyword`. Clock dump dispatch reaches the selected SoC hook and
STEP actions; MT6755 supplies no clock-failure hook, but configured STEP actions
can reach `wmt_step_sleep_or_delay` and sleep. The bridge's reader bookkeeping
preserves the caller's IRQ state and does not introduce a sleeping lock. Each
payload's existing context requirements still apply. The atomic-context host
case checks bridge bookkeeping with an empty SoC hook and nonblocking STEP
adapter; it does not claim that all clock dump payloads are IRQ-safe.

Run from the kernel tree with unused output directories:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_callback_lifetime.py --kernel . --output /tmp/wmt-callback-address
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_callback_lifetime.py --kernel . --output /tmp/wmt-callback-thread --sanitizer thread
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_callback_lifetime.py --kernel . --revision 54bdf406ee9963e3b926d8f19fc2bb4c1a3975eb --output /tmp/wmt-callback-baseline
```

The runner extracts the complete production bridge section, callback typedefs,
module stub registration and dispatchers, platform dispatchers and deinit,
`mtk_wcn_consys_clock_fail_dump`, `wmt_dev_tm_temp_query`, `WMT_exit`, library
callback registration wrappers, `wmt_lib_trigger_assert`, and both library
teardown phases.
It does not rewrite those function bodies. Compiler function-entry/exit hooks
place deterministic barriers at the real module dispatcher's first entry or
last return. The independent active-module-frame counter lets the test reject
cleanup even after the thermal command adapter's operation reference is zero.

Nineteen cases cover empty and partial tables; actual thermal, assertion, and
clock dispatch; sleeping thermal work; each final module return; waiting for
the last of three simultaneous callbacks; refused calls after closure;
replacement and concurrent registration/unregistration; nested dispatch;
IRQ-state preservation; normal outer teardown; library failure cleanup with
and without platform ownership; registration after closure; and a callback
acquired before its module body starts. Both ASan/UBSan and TSan pass all 19.
The same baseline fixture passes 6/19: thirteen schedules reject premature
writer return. In the final-return thermal schedule, baseline unregistration
returns with one module frame and zero operation references. Baseline outer
and library teardown reach 67 and 49 cleanup calls respectively while that
frame is paused; the candidate reaches none until the frame is released.

Outputs contain the generated fixture, compile command and log, per-case logs,
source and fixture hashes, and `result.json`. Host adapters model synchronization
with pthread primitives and C atomics. Driver resource destructors only record
their ordering; thermal commands, assertion keyword handling, and STEP payloads
are typed substitutes. The existing library initialization, outer initialization,
and operation-shutdown fixtures retain their own assertions and use a no-op
bridge-unregister adapter because this fixture tests the real bridge barrier.
These host tests establish source control flow and forced interleavings; they
do not emulate ARM64 memory ordering, firmware, or a live module unload.
