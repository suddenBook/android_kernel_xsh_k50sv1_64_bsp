# WMT command v2 transaction and metadata boundary

The paired launcher and kernel use `include/uapi/linux/mtk_wmt_cmd.h` verbatim.
Its SHA256 is
`4ea77989d01fa91b6cf2377aea59f679f02b3c9714b3052fa3ac014a2eec5c13`.
This replaces the untagged command mailbox and per-record metadata ioctls on the
selected MT6755 path. It requires the paired launcher; an old launcher cannot
provide command service to this kernel.

## Contract

Every read/write transfers one record with the 32-byte little-endian header:

| Offset | Field | Width |
| --- | --- | --- |
| 0 | magic `0x32544d57` | u32 |
| 4 | version `2` | u16 |
| 6 | kind | u16 |
| 8 | session ID | u64, aligned to 8 bytes |
| 16 | transaction ID | u64, aligned to 8 bytes |
| 24 | payload bytes | u32 |
| 28 | result | s32 |

Kind 1 contains 1..255 command bytes without NUL. Kind 2 is a status reply with
no payload. A negative status can fail any command; successful `srh_patch` and
`srh_rom_patch` require kinds 3 and 4 respectively. Positive results, extra
frames/trailing bytes and inconsistent lengths are rejected.

A list is count/reserved (u32 each, reserved zero), followed by exactly count
264-byte records. Each record contains a u32 index, four opaque address bytes,
and a nonempty NUL-terminated name[256]. Normal lists contain 1..10 unique
sequences covering 1..count, in any order. ROM lists contain 0..5 unique types
0..4. Empty ROM lists explicitly preserve optional absence. Maximum read/write
sizes are 287/2680 bytes.

`WMT_IOCTL_CMD2_SESSION` is `0xc020a040` in both native and compat mode. Its
32-byte structure is version/action, aligned u64 session, read/write maxima,
flags/reserved. Input maxima, flags and reserved are zero. BIND uses session
zero and returns its session and maxima. Repeated BIND on the same file
description is idempotent; another open receives `-EBUSY`. UNBIND must echo the
current session or receives `-ESTALE`. Failed input/output copies leave binding
and request ownership unchanged; an output fault may consume an unused ID.

An ordinary open remains usable for control ioctls. Only the explicitly bound
file description can receive or answer commands. Duplicated/inherited
descriptors share that description. File identity is checked in addition to the
wire IDs, so guessing another owner's current token does not authorize a reply.
The built-in adapter allocates monotonic nonzero session IDs across module
reloads. Transactions increment within each session. Both counters refuse
overflow instead of reusing an ID.

The broker permits one producer: idle -> queued -> delivered -> terminal ->
idle. A request can queue before BIND; only a still-live request is attached to
the new session. The publishing producer alone retires its request. Until then,
another producer receives `-EBUSY`, including after a reply has completed it.
Completion initialization/publication, reply acceptance, reset, timeout, unbind
and release serialize under the broker mutex. The terminal result is signalled
while holding that mutex, preventing an old completion from signalling a
subsequent request.

The six-second deadline starts at publication. Read is an atomic mailbox
operation, including on a file opened without `O_NONBLOCK`. An unbound file gets
`-ENOTCONN`; no pending delivery gets `-EAGAIN`; insufficient capacity gets
`-EMSGSIZE`; a copy fault gets `-EFAULT` without consuming delivery. Expiry before
read gives `-EAGAIN`. Expiry during a successful copy gives `-ETIMEDOUT` without
committing delivery: ignore the copied bytes and continue serving the bound
session. A second reader cannot receive the same request. Poll exposes readable
queued commands and writable delivered commands only to their owner.

Write bounds length, calls the real `memdup_user()` once, and validates/allocates
against that private copy. Final acceptance rechecks file, session, transaction,
delivery, terminal state, expected kind and deadline. Stale replies return
`-ESTALE`; malformed replies remain correctable before the deadline. A timeout
while acquiring a metadata cache mutex returns `-ETIMEDOUT`. Successful write
returns the whole record length. Reset reports `-ECANCELED`, owner disconnect
`-ECONNRESET`, and library shutdown `-ESHUTDOWN` to the pending producer, unless
another terminal decision has already won. Closing an unrelated open has no
command effect. VFS release follows the final reference, so prompt service stop
uses explicit UNBIND when another syscall still owns a reference to the file.

## Metadata and resource lifetime

The normal reply is allocated as one validated array, ordered by sequence.
Acceptance replaces array/count/readiness under the cache mutex before waking
the producer. No per-record publication remains. ROM records are all allocated
before acceptance, then previously empty type slots are filled under the ROM
mutex. The first **accepted** record of each type stays until deinit. Existing
types survive a later accepted retry; failed or stale searches cannot fill an
empty slot. Zero ROM records do not invent a WMT sentinel, so later getters can
search again when the WMT type remains missing.

Lock order is broker mutex -> one metadata mutex. Getters copy name/address
while holding their cache mutex and never take the broker mutex. Allocation and
userspace input copying precede the broker mutex. Publication cannot fail
halfway: all validation/allocation has finished, and the deadline is checked
again after acquiring the cache mutex.

`wmt_lib_get_patch_info()` remains a readiness marker. The selected SoC startup
reads count/records after its successful search on the wmtd producer, and its
download loop completes before that producer starts another search. The marker
is not a borrowed record pointer. Normal array replacement cannot free storage
during an individual getter copy. Shutdown closes command admission before
joining workers, draining operation queues and waiting for borrowed operation
references. Normal and ROM caches are then freed before `gDevWmt` is cleared.
The former early normal-cache free in outer `WMT_exit` was moved to this phase.

Legacy SET_PATCH_NAME/NUM/INFO/ROM_INFO are rejected with `-EOPNOTSUPP` in native
and compat dispatch for every open, including before BIND. Legacy text writes
have no fallback. Chip queries, HIF/power control and launcher-kill controls
retain their roles. Vendor/active-version ioctls remain separate administrative
inputs; they are not outputs of the original search handlers. An unsupported
launcher command, including `update_patch_version` when unsupported, must get a
tagged negative status. External UART/property actions are outside the atomic
kernel metadata transaction.

The FW_DBGLOG ioctl also now propagates `wmt_dbg_fwinfor_from_emi(0, 1, 0)`'s
result. Its collector lifetime repair belongs to the paired debug change; this
commit does not alter `wmt_dbg.c` or the ioctl arguments.

## Source-derived verification

Run from the kernel checkout, using fresh output directories:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_command_v2.py \
  --kernel . --output /tmp/wmt-command-v2
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_command_v2.py \
  --kernel . --output /tmp/wmt-command-v2-tsan --tsan
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_command_v2_legacy.py \
  --kernel . --revision 372a643505f6b0aab0b3adbd150ecb9d9291d8d9 \
  --output /tmp/wmt-command-v2-legacy
```

The candidate runner copies complete production broker, session allocator,
`memdup_user`, VFS read/write/open/close/poll/compat functions, metadata
publication/getters and control-consumer functions. Only the relevant complete
case bodies of the large ordinary ioctl dispatcher are extracted into a small
dispatch adapter. Production function bodies are not rewritten. The UAPI and
private cache/broker declarations are taken from source. Hashes, exact compiler
commands, generated fixtures, sanitizer logs and each outcome are retained.

The host replaces kernel allocation, user copies, VFS references, completions,
events and time. Allocations have a strict ownership ledger. Partial user-copy
faults, every ROM record allocation failure, deadline crossings and cancellation
during preparation have directed hooks. Single-thread histories use deterministic
completion/time adapters. The five pthread schedules exercise shared-owner
readers, an old copied reply held across cancellation and the next read,
reply/reset ordering, and a getter held while a replacement attempts to acquire
the cache mutex. Each scenario ends with successful fresh start/bind/request and
complete cleanup. TSan startup mapping failures, if encountered, are retained
separately and retried; they are not counted as executed test cases.

The library-init, shutdown and callback-lifetime fixtures retain their original
coverage with small adapters for the new broker lifecycle calls. The first two
also assert that cache cleanup runs only after both thread handles are cleared
and all checked-out operation references have returned. They do not substitute
for the separate broker behavior tests.

The legacy runner intentionally records counterexamples. It executes the
existing 20 identity observations (production and an explicitly test-only open
affinity model) plus six metadata histories against pinned source. Five real
late/duplicate reply histories remain unsafe in the old implementation; an open
affinity model still cannot distinguish the three same-open histories. The six
metadata histories demonstrate late completion of an expired partial list,
ready cache surviving failed/expired acknowledgement, mixing a later normal
cache, filling the ROM WMT cache gate late, and a failed ROM search retaining
the first record on retry. These are overlapping histories, not a count of
distinct vulnerabilities. The original `test_wmt_command.py`,
`test_wmt_command_identity.py`, and `test_wmt_patch_info.py` remain legacy-ABI
fixtures; use this v2 runner for the new ABI and the pinned legacy runner for
the historical controls.

Host checks do not execute hardware, Android property writes, firmware decoding,
module loading or the entire driver core. Final ARM64 checks must use the
preserved target build flags and compile the changed library, device and
built-in-adapter translation units. The paired launcher/service and hardware
validation are separate required integration evidence. Other silicon's legacy
single-patch launcher behavior is not claimed to be compatible with the retired
metadata ioctls.
