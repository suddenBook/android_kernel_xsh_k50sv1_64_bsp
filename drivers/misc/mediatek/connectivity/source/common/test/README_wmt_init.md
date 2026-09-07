# WMT transport initialization tests

Run the directed tests from the kernel root:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_init.py \
  --output /tmp/wmt-init-directed
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_init.py \
  --sanitizer thread --output /tmp/wmt-init-concurrent
```

The output directories must not already exist. `--kernel PATH` selects another
kernel checkout so the same contracts can be checked against the original code.
The runner records source and fixture hashes, compiler arguments, and every
case's output. It imports the function extractor from `test_wmt_command.py`.

The harness extracts the production HIF types, mode constants, parsing/apply
helpers, patch-name setter, framebuffer callback, and the two initialization
ioctl cases. Host substitutes implement user copies, allocation, the operation
pool, queue submission, transport selection, and work scheduling. Directed cases
use AddressSanitizer and UndefinedBehaviorSanitizer. They check valid UART, SDIO,
and BTIF modes with both FM transports; invalid-mode state preservation;
allocation, copy, pool and submission failures; retry; duplicate initialization;
and a display-unblank event waiting for initialization.

The ThreadSanitizer fixture runs 100 rounds of two competing initializers and
one framebuffer notification. Each round requires one committed configuration,
one pending-power-on handoff, and identical queued and applied HIF data. The
pool substitute has its own mutex; it must not supply serialization missing
from the actual initialization code. All test threads finish before the next
round resets the simulated driver.

The pool substitute models the existing `wmt_lib_put_act_op` ownership contract:
submission consumes the operation even on failure. An unsuccessful submission
leaves initialization retryable but can leave the prepared transport selected;
these tests do not assert rollback of that selection. No-op-pool and invalid-mode
failures must leave the previous configuration and transport unchanged.

These host checks cover the extracted control flow. They do not execute kernel
workqueues, the real operation consumer, firmware, or display and radio hardware.
The changed translation units also need the target ARM64 compiler, followed by
boot and radio tests of the installed kernel.
