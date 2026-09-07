# WMT operation result and completion ownership

From the kernel root:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_operation.py \
  --output /tmp/wmt-op-directed
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_operation.py \
  --sanitizer thread --output /tmp/wmt-op-concurrent
```

The output directories must be new. `--kernel PATH` selects a comparison
checkout; the same final runner can exercise the original code. Source,
fixture and runner hashes and every case's output are retained.

The consuming submission API can return its operation slot to the pool before
the caller runs again. Callers must not dereference that slot after submission.
`wmt_lib_put_act_op_result` copies a completed operation's fields while the
submitting thread still owns a reference. Before completion, the output retains
only the submitted fields. Completion with an error still supplies its diagnostic
fields. A wait that timed out returns failure even if the consumer finishes
before the submitting thread resumes.

Completion publication itself also needs the consumer's reference. The regular
worker, Wi-Fi worker and reset drain retain it until signaling finishes. The
regular worker keeps its reference across dispatch, including the core's ID
change for Wi-Fi probe/remove. The Wi-Fi worker receives a separate reference
before publication. After dispatch, the regular worker caches the updated ID
before releasing its reference, so logging and the exit check cannot access a
recycled slot. Submission caches its original ID and wait target before
publication, so timeout diagnostics do not read a consumer's changing fields.

The 20 directed ASan/UBSan cases execute the production submit helpers, regular
worker loop, Wi-Fi on/off dispatch branches and three ioctl branches. The
baseline comparison adapts the old submission followed by
the caller's field read; that adapter is test code, not a pre-existing kernel
function. The baseline completion fragment comes directly from the original
worker. User copies, PSM, scheduling and firmware replies are controlled host
substitutes. Two cases place the waiting thread's final release inside completion
publication to check the consumer's reference lifetime. The three actual ioctl
branches verify return lengths survive immediate operation-slot recycling.
The Wi-Fi cases exercise worker completion before dispatch returns, delayed
completion, queue failure and the core's operation ID mutation. An exit case
checks the worker's final operation access against immediate slot recycling.

The ThreadSanitizer case runs two senders and one consumer through 2,000 numbered
requests, reusing one operation slot. Its queue and completion locks are host
substitutes. For the original consuming API, a legal scheduling point between
submission return and the caller's copy exposes the unowned result read. The
candidate copies before returning the slot. The test requires every reply to
belong to its own request, all references released, and no sanitizer race.

These checks do not execute firmware or real kernel scheduling. ARM64 compilation
and installed boot/radio tests are separate requirements. Copying `OSAL_OP_DAT`
does not deep-copy buffers addressed by its fields; timeout lifetime of caller
buffers, shared loopback storage and diagnostic borrowed-operation pointers
remain separate audit work. No hardware error injection is needed for these
controlled reference schedules.
