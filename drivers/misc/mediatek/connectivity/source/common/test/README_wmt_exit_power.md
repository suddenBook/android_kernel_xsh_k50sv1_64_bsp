# WMT power-off error and retry regression

`test_wmt_exit_power.py` extracts complete production `opfunc_pwr_off()` and
`wmt_ctrl_hw_pwr_off()` functions. The core function calls the real control
function through a small dispatcher; only the platform operation and STP close
are substituted. Each hardware attempt is counted and has an injected result.

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_exit_power.py --kernel . --output /tmp/wmt-exit-power
```

Eight cases cover successful shutdown, negative and positive hardware errors
followed by successful retry, retry when STP is already closed, continued
hardware shutdown after an STP close error, the existing power-off gate, and
already-off behavior. A failed hardware call must preserve its exact result,
leave the control PWR bit set and the core in its intermediate POWER_ON state,
then reach the hardware adapter again on retry. These checks independently
reject either original state bug; returning zero from a retry that skipped the
hardware adapter is a failure. POWER_ON here means shutdown is unconfirmed; the
test does not claim that physical hardware is still powered.

The runner uses AddressSanitizer, UndefinedBehaviorSanitizer, and leak detection,
and records source hashes and compiler commands. `--kernel` also accepts a
baseline checkout. The tests do not exercise real clocks, regulators, firmware
or worker scheduling.

When the exit helper exists, the runner also extracts its complete production
body and runs six more cases (14 total). A pending BTM reset can set power ON
before the modeled join returns; both initially ON and initially OFF states
must then reach the actual core/control shutdown. The remaining cases cover an
idle OFF state, no available operation, PSM failure, and hardware-error retry,
including balanced operation and awake references. Removing only the helper's
BTM join fails five cases (9/14 pass). The operation adapter dispatches directly
to the real core function; queue ownership is covered by the WMT pool fixture.
Device validation remains separate.
