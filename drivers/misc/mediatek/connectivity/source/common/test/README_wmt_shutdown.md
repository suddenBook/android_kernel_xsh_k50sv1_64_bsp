# WMT operation pool shutdown regression

Run from the kernel tree with an unused output directory:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_shutdown.py --kernel . --output /tmp/wmt-shutdown-address
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_shutdown.py --kernel . --output /tmp/wmt-shutdown-thread --sanitizer thread
```

The runner compiles the production operation structures, ring macros, pool guards,
checkout/release/submit helpers, both worker loops, reset drain, and complete
`wmt_lib_init` and both library teardown phases without rewriting their bodies.
This pool fixture has no STP producer, so its wrapper invokes prepare then
finish directly; the callback/STP fixtures test the barrier between them. Pthreads
replace kernel mutexes, waitqueues, completions, and thread primitives. The host
allocator counts outstanding payloads and rejects clearing device storage while
a checkout remains. Queue-lock access and destruction also verify initialization.

Sixteen cases exercise pending active and Wi-Fi worker queues, active worker
completion, retained completed and timed-out senders, pre-submit borrowers,
enqueue/wakeup racing shutdown, refused late Wi-Fi handoff, reset draining during
shutdown, final recycling in progress, and pool reopening. Initialization failures
before pool setup, after pool setup, and at worker startup keep admission closed.
Barriers make the concurrent schedules deterministic; timeouts turn deadlocks
into failures. Both sanitizers run the same cases.

Timer/work substitutes check initialization and stop-before-cancel ordering.
They do not execute timer callbacks. The host serializes a worker wait predicate
with its queue lock to avoid treating ordinary kernel waitqueue observation as a
host C data race. Unrelated platform, transport, PSM, and partial-initialization
cleanup are substituted; these tests make no claim about their complete lifetime.
They do not access hardware or validate kernel scheduler behavior.

Each output directory contains the generated fixture, compiler log, per-case
logs, and `result.json` with source, fixture, runner and host hashes.
