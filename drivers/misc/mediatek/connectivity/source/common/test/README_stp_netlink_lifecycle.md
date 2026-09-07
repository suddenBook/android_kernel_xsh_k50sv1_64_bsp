# STP netlink lifecycle regression

The runner compiles complete production `stp_dbg_nl_init`, `stp_dbg_nl_deinit`,
`stp_dbg_nl_bind` and `stp_dbg_nl_send` functions with their actual globals,
operation table and family declaration. Kernel mutexes use pthread mutexes;
generic-netlink registration, callback admission and skb delivery have small
adapters. No source-order assertions substitute for executing the functions.

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_stp_netlink_lifecycle.py --kernel . --output ../radio-review/module-lifecycle/netlink/new-run
```

Pass `--revision 69faa749fff` with a separate output directory to run the same
fixture against the original functions. The first five lifecycle cases pass on
current source; that revision passes 1/5, with these observed failures:

| Case | Original behavior |
| --- | --- |
| Register → close → register | Second registration has `n_ops=0` instead of 2 |
| Close during a paused outgoing send | Unregistration and close finish before the send |
| Close with an admitted bind callback | Closed table retains `num_bind_process=1` |
| Refused outgoing send completes after close | Closed table reaches `num_bind_process=-1` |

The fifth case covers no listeners and all five send error/early-exit paths,
then exercises lifecycle and table operations to detect retained locks.

A sixth case binds four distinct peers, refuses the first unicast and delivers
to the other three. It checks that compaction preserves those three identities
and that the following send reaches all three. Before adding the missing inner
loop `break`, the table was `[102,0,0,0]` despite a count of 3 (5/6 cases passed).
The one-line fix yields `[104,102,103,0]` and 6/6 cases pass.

Condition variables hold the sender inside unicast or hold a bind callback
after generic-netlink admission. The close thread must reach the corresponding
mutex/admission barrier before the held thread is released. A try-lock confirms
the send's mutex is still held; there are no sleeps or timing guesses. All
threads are joined before final table assertions.

The registration adapter preserves this kernel's `genl_unregister_family`
behavior (`net/netlink/genetlink.c:445` clears `n_ops`) and waits for admitted
callbacks. It does not wait for outgoing unicast. The OSAL destructor records
logical destruction while retaining valid backing pthread mutex storage until
process exit; this lets the old late-lock/count bug run without invoking
undefined pthread behavior. It is not a test of kernel mutex destruction.

Each case runs with AddressSanitizer, UndefinedBehaviorSanitizer and leak
detection, with an eight-second deadlock deadline. Result JSON records source,
host and generated-fixture hashes, compiler arguments and case results. No
kernel build, actual netlink socket or device state change is involved.
