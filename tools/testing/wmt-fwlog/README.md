# WMT firmware log collector lifecycle

The retained `wmt_dbg_fwinfor_from_emi(0, 1, 0)` never left its streaming
loop. `WMT_IOCTL_FW_DBGLOG_CTRL` therefore held the enabling launcher thread
indefinitely; sending disable before joining also left a race with a worker
which had not yet entered its enabling ioctl. Allocation and EMI mapping
failures leaked the collector lock or buffer, and a producer-index delta could
overflow the 8 KiB `gEmiBuf` configuration.

## Internal collector contract

The existing arguments distinguish the two callers without changing the ioctl
number or payload:

| Caller | Arguments | Behavior |
| --- | --- | --- |
| Firmware log ioctl | `par1=0, par2=1, par3=0` | Drain one producer-index snapshot and return. |
| Proc command `0x19`, trace stream | `par1=0x19, par2=1` | Repeat bounded passes while EMI debug mode is exactly 1. |
| Proc command `0x19`, manual read | `par2 != 1` | Read the control words and at most 4 KiB of trace data. |

The paired launcher repeatedly invokes the bounded ioctl while logging is
requested. Its stop path joins that loop before a final disable operation so
a delayed worker cannot enable logging after the final disable. The paired
ioctl dispatcher must propagate the collector result. Those caller changes
are separate from this patch.

The cursor persists across calls and is protected by `g_dbg_emi_lock`. A pass
reads the producer index once, rejects indices at or above 32768, and advances
the cursor only after a copied chunk. Each copy is bounded by `sizeof(gEmiBuf)`.
Wrap retains the existing format's excluded final byte: payload occupies
`[0, 32767)`. A manual offset of 32767 wraps to zero; negative offsets, offsets
above 32767, and negative lengths return `-EINVAL`.

The collector uses interruptible locking, checks `signal_pending(current)`
before work and between trace chunks, and checks the debug-mode word before
each chunk. A proc stream releases the mutex before its interruptible 100 ms
sleep and obtains fresh EMI mappings on each pass. Disable returns success;
a signal returns `-EINTR`. Missing EMI metadata or mappings return `-ENODEV`,
allocation failure returns `-ENOMEM`, and an invalid producer index returns
`-ERANGE`. Each acquired lock and allocation is released on every exit.
The 100 ms polling period is not a real-time completion guarantee: the current
bounded copy/print pass or another mutex holder may still need to finish.

The collector maps the control and trace areas directly because the retained
`wmt_lib_get_fwinfor_from_emi` returns `NULL` even after a successful copy and
cannot report mapping failure. No other callers of that helper are changed.

## Run the focused checks

From the kernel repository, with Clang and its sanitizers installed:

```sh
python3 tools/testing/wmt-fwlog/run.py --output /tmp/wmt-fwlog-large
python3 tools/testing/wmt-fwlog/run.py --small-buffer --output /tmp/wmt-fwlog-small
python3 tools/testing/wmt-fwlog/run.py --thread --small-buffer --output /tmp/wmt-fwlog-thread
python3 tools/testing/wmt-fwlog/run.py --baseline --small-buffer --output /tmp/wmt-fwlog-baseline
```

Each output directory must be new. The default and small-buffer runs each
contain 26 cases under ASan/UBSan with leak detection. The TSan run selects
three concurrent lifecycle cases. Six baseline controls use commit
`372a643505f6b0aab0b3adbd150ecb9d9291d8d9`; all six must fail the new contract
without a test timeout. Their expected failures are an endless ioctl, lost
allocation/mapping/signal errors, and two excessive-copy attempts. Only the
baseline host's old `msleep` adapter escapes after three sleeps with `-ELOOP`;
the extracted production loop is not rewritten.

The runner extracts the complete printer, trace helper, collector, OSAL lock
wrappers, and retained EMI-copy helper, plus their buffer and lock declarations.
It records source, extracted-function, fixture, runner, and host SHA-256 hashes.
The extracted functions occur byte-for-byte in the generated fixture. Host
adapters model mutexes with pthreads, MMIO words with atomic access, allocation
faults, bounded copies, and a task's pending-signal predicate. Both ordinary
and fatal pending-signal cases reach the same kernel predicate; these tests
do not deliver actual kernel signals.

Cases cover empty/progressing drains, a moving producer after the snapshot,
full-ring chunking, wrap, invalid indices, allocation and each mapping failure,
manual bounds, cancellation at entry/mapping/chunk/sleep/lock wait, disable,
mapping loss between passes, and an ioctl while a proc stream is asleep. Every
candidate case checks zero live allocations, zero held locks and a cleared
shared print pointer, then verifies a subsequent successful drain.

These checks exercise the actual collector bodies with host OS/MMIO adapters.
They do not emulate firmware writes, establish device log completeness, or
prove a module-unload lifecycle. Compile the changed unit with the target
kernel's generated headers and flags before integration; an ARM64 object
compile and a complete integrated kernel build are distinct checks.
