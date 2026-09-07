# OSAL initialization failures

The thread constructor now preserves a failed `kthread_create` errno and clears
its error pointer. Previously the wrapper returned success and later attempted
to wake an `ERR_PTR`. FIFO initialization now checks descriptor allocation,
releases the descriptor when data allocation fails, and propagates the error to
its caller. Reinitializing a FIFO also releases the old descriptor. Wake-source
initialization leaves its initialized flag clear when allocation fails.

All production thread-create callers in this connectivity tree check a negative
or nonzero return. PSM is the sole production FIFO-init caller and requests an
internally allocated buffer. PSM and WMT platform initialize wake sources; the
platform currently ignores the return. These changes keep successful return
values and normal thread/FIFO/wake-source behavior unchanged. They do not audit
all callers' remaining unwind behavior or change external FIFO-buffer ownership.

Run from the kernel tree, using a new output directory:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_osal_init.py \
  --output /tmp/osal-init
```

`--kernel /path/to/baseline` uses the same tests on older source. The fixture
extracts the actual OSAL structures and thread, FIFO and wake-source functions.
Host substitutes implement kernel allocation, kthread handles, wake sources and
FIFO storage. Twelve ASan/UBSan cases cover two kthread error codes, both FIFO
allocation stages, FIFO reinitialization/roundtrip, invalid external capacity,
wake-source failure/idempotence, retries and invalid arguments. The runner
retains generated C, compiler arguments and source/fixture/runner hashes.

The real kernel scheduler, allocator and wake-source implementation are not run.
Leak sanitizer is disabled so baseline failures can retain resources for an
explicit allocation-count assertion. A kernel build and installed tests remain
necessary.
