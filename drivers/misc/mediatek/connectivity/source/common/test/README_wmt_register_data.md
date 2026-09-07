# WMT register value ownership regression

Run `python3 test_wmt_register_data.py --kernel <kernel> --output <new-directory>`.
The same runner accepts the parent baseline and the candidate source tree.

The fixture extracts the active `wmt_lib_reg_rw` and `wmt_lib_efuse_rw` callers
(the old disabled register implementation is excluded) and both production core
consumers. It substitutes operation submission, PSM and firmware transport. ASan
with stack-use-after-return detection reproduces consumer access to the original
caller's expired stack in timeout and cancellation schedules. Other cases retain
read/write values, exact efuse command bytes, consumer errors, wake failure and
NULL rejection, including immediate operation-slot reuse after completion.

Register and efuse values now travel in their existing operation data field.
The raw register transport still receives a worker-local `UINT32 *`; completed
results return through the owned scalar snapshot. Efuse's existing behavior is
preserved: its command contains the supplied value and the read caller returns
that supplied value, since the original core consumer did not decode an efuse
reply value. No userspace or firmware layout changes are made.

The fixture's delayed consumer models both a queued timeout and a cancellation
that returns before value access. It is not a real reset, firmware execution or
scheduler test. The separate operation/cancellation tests cover terminal-state
and reference publication. This test does not cover other addressed buffers.
