# Untagged WMT command reply boundary

This change adds characterization tests only. It does not change the production
command protocol or claim that stale replies are fixed.

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_command_identity.py \
  --output /tmp/wmt-command-identity
```

The runner extracts the actual command producer, broker, cancellation and device
read/write functions. Its ten directed cases run both unchanged and through a
test-only open-affinity wrapper that also remembers the latest generation read
on each open. The wrapper is a model, not a proposed kernel implementation.
Host substitutes supply user copies, completion scheduling and locks.

The unchanged protocol accepts A's late `ok` after command B is delivered, even
when B's actual response is failure. Open affinity can reject A's response when
B was read through another open. It cannot distinguish these same-open histories:

1. A times out or is cancelled; the same open reads B; A's delayed `ok` arrives.
2. A completes; the same open reads B; a duplicate of A's `ok` arrives.
3. B completes normally and returns its own `ok`.

At the final write, the bytes, open identity and latest per-open generation are
identical. A generation only stored in kernel file state is overwritten by read
B. The reply itself must carry a transaction identity, or the protocol must forbid
all further commands on that open after such ambiguity. Merely keeping an expired
request until some untagged reply arrives can instead block future commands
indefinitely when that reply never arrives.

Controls check ordinary success/error responses, rejection before delivery and
before the next command, cancellation, zero/short reads, user-copy failures and
retry. Twenty expected observations under ASan/UBSan describe both the working
guards and the remaining stale-reply acceptance. `result.json` explicitly records
`production_fix: false`; successful characterization is not successful repair.
Source/runner/fixture hashes, compiler arguments and case output are retained.

## Product compatibility boundary

The local K50 product copies `vendor/bin/wmt_launcher` from its retained vendor
binary, and `rootdir/etc/init/init.wmt.rc` starts that executable. Its local source
tree has no `wmt_launcher` Android.mk/Android.bp module or launcher C/C++ source.
The verified native-provider inventory contains source-built `wmt_loader`, not
`wmt_launcher`. The source loader operates `/dev/wmtdetect` initialization ioctls;
the source WLAN assistant operates `/dev/wmtWifi`. Neither implements the
`/dev/stpwmt` command read/response loop.

The kernel issues `srh_patch`, `srh_rom_patch`, `update_patch_version`, and the
UART-only `open_stp`, `close_stp`, `baud_*` commands through `wmt_ctrl_ul_cmd()`.
Preserving those command handlers, patch ioctls, properties and failure behavior
requires a faithful launcher implementation before changing its wire protocol.

A future repair must roll out the command broker and launcher together: version
the transport, deliver a non-reused request identifier, echo it with every reply,
and reject cancelled, expired or already-completed identifiers. Untagged fallback
would retain this ambiguity. The current binary cannot be assumed to understand
a tagged envelope; a kernel-only protocol switch would risk connectivity startup.
No partial kernel mitigation is included here, and no device responses were
injected during this assessment.
