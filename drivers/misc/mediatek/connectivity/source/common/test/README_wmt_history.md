# OSAL operation-history snapshots

WMT, its Wi-Fi worker, PSM and BTM record scalar operation fields while they own
an operation. The history worker prints the copied record; it does not borrow
or dereference the stored operation address or parameter pointers.

The history printer previously allocated with `GFP_KERNEL` while holding an IRQ
spinlock. Its worker also cleared the shared snapshot pointer without that lock.
There was no history teardown: allocated rings leaked, and WMT could zero its
embedded dump work while it was queued or running.

Initialization now reserves both the history ring and one reusable snapshot.
Printing requires no allocation. The history lock covers snapshot publication
and a busy flag, so a new print cannot overwrite data still being read by the
worker. Busy requests continue to be coalesced. Empty histories do not queue work.

Teardown first disables saving and new snapshots under that lock, then cancels
or joins the worker and frees both buffers. Scheduling occurs under the same
lock so teardown cannot miss a publisher. Normal WMT, PSM and BTM teardown and
initialization failures release their histories. Queue resets preserve the
history, including snapshots of earlier operations. Initialization and teardown
must be serialized by the owner; its storage must remain valid until concurrent
API callers are quiescent. This does not repair unrelated thread, timer or
callback teardown in these drivers.

## Host checks

From the kernel tree:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_history.py \
  --output /tmp/wmt-history-directed
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_history.py \
  --sanitizer thread --output /tmp/wmt-history-thread
```

Use `--kernel /path/to/baseline` for the same fixture against older source. Each
output directory must be new. The runner extracts the actual OSAL history
functions and structures and the complete production ring implementation. Host
substitutes provide allocation, logging, locking and a workqueue with a pending
bit that clears before callback execution. The fixture counts allocations and
records any `GFP_KERNEL` allocation while a substituted spinlock is held. It
cannot make the host allocator behave like kernel reclaim or execute IRQs.

Eight ASan/UBSan cases cover immutable wrapped-ring snapshots, allocation
context, queued/running teardown, first/second allocation failure, invalid
capacity, empty history and repeated initialization/teardown. Two TSan cases
run 20,000 saves and print attempts alongside the worker, with and without
concurrent teardown. The baseline's missing teardown is represented by a no-op;
those failures prove retained allocations/work, not a performed kernel unload.
Memory leak sanitizer is disabled because failing baseline cases intentionally
exit with retained buffers; explicit allocation counts test cleanup.

Generated C, source/fixture/runner hashes, compiler commands and case outputs
are retained by the runner. These checks do not execute the complete WMT/PSM/BTM
initialization, reset or module-unload sequence. A consistent module rebuild
and installed validation remain required because the shared history structure
changes size.
