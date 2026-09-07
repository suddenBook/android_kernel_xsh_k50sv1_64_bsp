# WMT library initialization unwind regression

After failed `wmt_lib_init`, `WMT_init` runs library prepare, STP exit, then
library finish. Cleanup must
therefore distinguish a created thread from an uncreated thread, and initialized
platform resources and mutexes from zeroed storage. It must also join asynchronous
work and release owned allocations before clearing `gDevWmt` so a retry is safe.

The library records completion of core, embedded primitives, platform, PS, and
optional IDC initialization. Existing timer and pool flags retain their roles.
`osal_thread_destroy` performs the single `kthread_stop` and clears each handle;
calling `osal_thread_stop` followed by destroy would stop the same task twice.
Pool closure, worker joins, queued-reference completion, and the wait for checked
out operations still precede primitive destruction. A spinlock closes delayed
assertion publication before `cancel_work_sync` joins that work.

STEP initialization belongs to `WMT_init`. Its destructor must run from the
successful outer `WMT_exit`, after proc/debug work is drained and before final
power-off. Library failure unwind never owns STEP.

Run from the kernel tree, choosing unused output directories:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_lib_init.py --kernel . --output /tmp/wmt-lib-init-address
CC=clang python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_lib_init.py --kernel . --output /tmp/wmt-lib-init-thread --sanitizer thread --case assert-publication-serializes-with-close --case running-assert-finishes-before-clear --case late-assert-is-refused
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_lib_init.py --kernel . --revision e978fd0b295 --output /tmp/wmt-lib-init-baseline
```

The runner extracts the complete production library init, both teardown phases, and delayed
assertion publication functions. It also extracts actual OSAL thread create,
stop, destroy, and history init/deinit bodies. Baseline and candidate use the same
adapters and assertions. Each output includes source hashes, generated fixture,
compiler command/log, per-case logs, and `result.json`.

Twenty-seven cases cover cleanup before init; configuration failure; NULL, ENOMEM,
and EINTR results for both thread creations; core, platform, PS, and both thread
start failures; each of four optional history allocations; optional IDC failure;
SOC and COMBO success; pending timer/history work; sparse patch version allocations;
assertion publication racing closure; an assertion callback running during cleanup;
and refused assertion publication both before init and after cleanup. The new
case holds an actual `wmt_lib_btm_cb` after the operation pool is idle: prepare
must retain callback resources until its return, then finish may release them.
The same case fails with `--revision 69faa749fff`. Every case
then retries successful initialization and cleanup, followed by repeated cleanup.
Strict adapters reject uninitialized mutex destruction, duplicate task joins,
IDC unregistration without successful registration, and device clearing while
resources or allocations remain. Barriers control the concurrent overlap cases.
ASan/UBSan is the default; the optional TSan invocation above selects the three
assertion teardown cases. `test_wmt_shutdown.py` separately exercises the actual
pool helpers and both workers in sixteen ownership schedules.

The adapters model constructor contracts, not hardware. In this tree, embedded
OSAL primitives cannot fail for non-NULL arguments. Event and signal cleanup,
core clearing, and the current PS deinit no-op remain safe on zeroed storage in
the adapters, as they are in production. Core and PS init currently return zero;
platform init propagates setup failures. Valid thread handles also make thread
run succeed. Injected errors for the non-failing callees exercise the library's
existing return branches rather than
claiming those callees currently report such faults. Configuration failure is
modeled before retained configuration allocation, matching the propagated failure
paths in `wmt_conf_read_file`/`wmt_conf_parse`. The actual history constructors
handle their injected allocation failures without failing library initialization.

Platform initialization and unwind have their own actual-source fixture.
The assertion callback body is substituted with a resource-using callback;
these tests cover admission and joining, not assertion payload concurrency. The
new fixture substitutes empty queue draining because operation ownership is
covered by the existing shutdown fixture. Neither suite exercises hardware,
module loading, arbitrary concurrent init calls, or kernel scheduling internals.
