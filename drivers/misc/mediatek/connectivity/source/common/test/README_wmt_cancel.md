# WMT reset cancellation and active-operation access

From the kernel root, with new output directories:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_cancel.py \
  --output /tmp/wmt-cancel-directed
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_cancel.py \
  --sanitizer thread --output /tmp/wmt-cancel-concurrent
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_operation.py \
  --output /tmp/wmt-operation-regression
```

`--kernel PATH` selects the comparison checkout. The same runner extracts the
candidate helpers or the original reset-wakeup fragment. Its baseline snapshot
adapter models the old getters followed by the callers' ID access. The fixture
retains source, imported-runner, generated-fixture and compiler identities.

A reset wakeup does not establish that the consumer has finished writing its
reply. WMT operations now record pending, completed or cancelled state under one
spinlock. Cancellation and completion publish at most one terminal signal.
The submitting thread copies result fields only from completed work; cancelled
work returns failure with its submitted fields, even if the consumer finishes
before the sender resumes. The consumer still holds its reference until its
actual completion path returns, and returns the slot after dropping that final
reference outside the spinlock.

The same lock protects current/worker pointer publication and withdrawal.
Publishing workers retain their reference until their pointer is cleared.
Reset signaling occurs inside that protected lifetime. Diagnostics take only an
ID snapshot while the pointer is protected. The core's Wi-Fi ID changes use
`WRITE_ONCE`, paired with the snapshot's `READ_ONCE`. The remaining borrowed
getter is restricted to the core dispatcher on the publishing wmtd thread;
it is not an interface for another thread to retain a request pointer.

Nine directed ASan/UBSan cases cover early reset wakeup, late success/error,
already-completed work, repeated cancellation, absent pointers, asynchronous
work, withdrawal and worker-ID snapshots. The first TSan case cancels while a
consumer continues writing reply fields. The second runs one publisher through
20,000 operation-slot reuses while two observers cancel and sample both active
IDs. Both use the extracted production helpers. Host pthread locks substitute
for the kernel spinlock and completion/queue machinery.

The existing 20 operation-ownership cases and 2,000-request TSan fixture also
remain required. Their directed completion substitute now invokes the actual
production completion helper rather than writing the result/reference directly.

These fixtures do not execute a full chip reset, reset notifiers, IRQ scheduling
or firmware. ARM64 compilation and installed boot/radio checks are separate.
Cancellation still returns before the consumer finishes; copying scalar result
fields does not extend the lifetime of buffers referenced by operation fields.
Stack/shared payload ownership and operation-history readers remain separate
audit work. The new state field is internal kernel bookkeeping, with no ioctl
or firmware wire-layout change.
