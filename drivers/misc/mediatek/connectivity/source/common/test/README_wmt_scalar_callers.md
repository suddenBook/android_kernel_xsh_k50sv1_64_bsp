# Thermal and desense operation-result ownership

From the kernel root, with new output directories:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_scalar_callers.py \
  --output /tmp/wmt-scalar-callers-candidate
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_scalar_callers.py \
  --revision cd4c037ecd7 --output /tmp/wmt-scalar-callers-baseline
```

`--kernel PATH` selects another kernel tree; `--revision` extracts a fixed Git
commit without modifying that tree. The same runner tests both versions.

Thermal and desense callers previously retained `pOpData = &pOp->op` across the
consuming submission call. Thermal success could return another request's value,
and its failure fallback could overwrite a reused slot or an unfinished consumer's
reply. Both callers could log another operation's fields. They now use the owned
scalar result snapshot for all access after submission, including error fallbacks.

The runner extracts both complete production callers, the production submission,
completion, cancellation and current-operation publication helpers, and their
operation structures and enums. Host substitutes provide queues, signals, locks,
PSM and consumer replies. Logging formats every argument and checks the caller's
operation ID and type after submission; it is not replaced by a no-op.

Thirty ASan/UBSan cases cover thermal read, thermal enable and desense. Each runs
success and failed completion with immediate slot reuse, cancellation and timeout
while the consumer retains its reference, wake failure, an empty pool, unsupported
or invalid commands, enqueue failure and coredump rejection. The checks verify
return values, logged fields, unchanged reused or consumer-owned data, and final
reference release. The cancellation case allows the consumer to finish after the
caller returns. Generated C, source and runner hashes, compiler commands and case
output are retained.

These are controlled host schedules, not real firmware, IRQ or chip-reset tests.
They exercise scalar ownership and do not extend the lifetime of buffers addressed
by other operation fields. Real ARM64 compilation and installed validation remain
separate checks.
