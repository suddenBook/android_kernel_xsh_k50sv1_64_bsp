# STP teardown resource regression

`test_stp_teardown.py` extracts the complete production BTM, STP and debug
teardown functions. Tracked host objects model kernel timer deletion, thread
joining, work cancellation and allocation lifetime. A callback admitted before
its barrier still checks the resources it uses; freeing those resources first
fails. This models kernel API contracts, without emulating the scheduler,
transport or actual timer handlers.

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_stp_teardown.py --kernel . --output /tmp/stp-teardown
```

The ten cases cover pending/running debug timers and log work, partial debug
allocation, BTM callbacks retaining PSM/debug, partial STP construction, the STP
TX timer, repeat teardown and teardown before construction. The current source
passes 10/10 with ASan/UBSan and leak detection. `--revision 69faa749fff` runs the
same adapters against the old complete functions and fails all ten cases.
Netlink concurrency and WMT prepare/finish retention have separate actual-source
fixtures. These host results do not establish a successful device unload.
