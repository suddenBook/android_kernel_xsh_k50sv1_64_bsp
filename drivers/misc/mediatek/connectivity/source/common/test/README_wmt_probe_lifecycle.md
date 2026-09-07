# MT6755 probe ownership and failure tests

`test_wmt_probe_lifecycle.py` exercises complete functions extracted from the
checked-out kernel. The fixture models a fixed MT6755 device using the local
3.18 driver-core and devres contracts. It does not access hardware.

## Production contract

MT6755 requires a successful probe before `mtk_wcn_consys_hw_init()` succeeds.
Its ops table opts into `platform_driver_probe()`, which this kernel provides
for a device that is already registered and cannot be hot-plugged. Other IC ops
tables default to the existing `platform_driver_register()` behavior.

The four register mappings use `devm_ioremap()` after OF address translation.
This preserves the nonexclusive mapping of shared TOPCKGEN and SPM registers;
it does not introduce the exclusive resource claim of
`devm_ioremap_resource()`. Clock and regulator handles use the local managed
acquisition APIs. The common probe checks its temporary EMI mapping before
clearing memory and checks MT6755 coredump, regulator and pinctrl acquisition
results. Missing optional pinctrl (`-ENODEV`) and absent optional GPIOs remain
allowed. A deferred GPIO provider prevents this probe from succeeding.

On any checked failure, the probe unmaps the retained coredump mapping, clears
MT6755 resource handles and common globals, and returns the error. The driver
core then releases managed resources. A successful probe publishes `g_pdev`
only after these stages complete. `remove()` performs the same withdrawal
before driver-core devres release. Hardware deinit unregisters an owned driver
once; failed registration and repeated deinit do not unregister it again.

The GPS phandle and child-node references are released on each lookup path.
A missing GPS pin property retains the invalid-pin sentinel. Coredump restore
reuses an existing mapping, and coredump unmap clears its published pointer
before releasing the mapping.

### Error propagation and retries

This kernel's `really_probe()` normally hides a probe failure from driver
registration. The MT6755 path records the result of `mtk_wmt_probe()` and uses
the fixed-device registration helper's bound-device check. An error raised
inside `mtk_wmt_probe()` therefore reaches its caller with its original errno,
including `-EPROBE_DEFER` from a clock, regulator or pin provider.

The driver is unregistered after such a failure. No automatic retry is promised:
the caller must retry WMT initialization after the provider becomes available.
The helper suppresses bind/unbind attributes and replaces the probe callback
with the kernel's failure callback after the registration attempt.

The latch cannot observe failures before `mtk_wmt_probe()` is entered. For
example, an OF clock-default or PM-domain failure in `platform_drv_probe()`
leaves no bound device and is reported as `-ENODEV`, as is a missing matching
device. A registration error such as driver-group creation failure keeps its
own errno. If group creation fails after binding, this kernel already calls
`remove()` and releases devres; WMT must not unregister the failed registration
again.

## Running

Run from the kernel root, using a new output directory for each run:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_probe_lifecycle.py \
  --kernel . --output /tmp/wmt-probe-candidate

python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_probe_lifecycle.py \
  --kernel . --revision 7f5c87f22c23f59607c020158f1e072473d66e9a \
  --output /tmp/wmt-probe-baseline
```

The baseline command is expected to return nonzero. `--case NAME` selects a
case and may be repeated. Set `CC` to select a host C compiler. The runner uses
AddressSanitizer and UndefinedBehaviorSanitizer with leak detection enabled.
Each output directory contains the generated source, binary, compile log,
individual case logs and `result.json`. The result records exact source,
adapter-context, fixture, runner and host-template hashes. Failed cases also
record an assertion, ASan, UBSan, timeout or other exit classification and its
diagnostic; full output remains in the corresponding log.

## Coverage and source fidelity

The 54 cases cover successful probe/remove and a fresh retry after each
scenario; cleanup before probe, duplicate init and repeated cleanup; driver
registration failures, no matching device, and registration rollback both
after binding and after a failed probe; all four OF register translations,
managed-allocation failures and mapping failures; clock allocation and provider
errors; absent, zero-sized or undersized EMI and both EMI mapping failures;
all four regulator allocations and provider errors; pinctrl allocation,
provider errors and its optional absence; GPS property/fallback/absence and
node references; deferred and absent optional GPIOs; repeated coredump restore
and unmap; failure before the WMT probe; and legacy unbound registration.

The fixture inserts complete production functions for:

- common probe, remove, hardware init/deinit and restore, including the cleanup
  helper when present;
- MT6755 register, clock and regulator acquisition, EMI protection/remapping,
  coredump mapping, ops selection and handle cleanup;
- `devm_ioremap()`, `devm_clk_get()`, managed regulator acquisition and
  `devm_pinctrl_get()`, including their actual release callbacks;
- `platform_driver_probe()`, `platform_drv_probe()`, its failure callback and
  `platform_drv_remove()`.

The ops types and MT6755 ops initializer are extracted. Unused hardware ops are
stubbed from their source declarations. Host adapters replace lower-level
allocation, provider lookup, OF access, register I/O and driver-core traversal.
They retain the relevant behavior of this tree's `drivers/base/dd.c`,
`drivers/base/driver.c`, `drivers/base/devres.c` and `drivers/of/address.c`:
probe errors are masked by registration, managed resources are released after
probe failure or after remove, release is in reverse acquisition order, and
driver-group rollback undoes a successful bind. The register resource sizes
match `arch/arm64/boot/dts/mt6755.dts`.

A resource ledger rejects invalid, duplicate or omitted releases. It checks
that global handles have already been withdrawn at the first driver-core
devres release and before the coredump mapping is unmapped. Each scenario then
requires a complete successful initialization/removal cycle with no resources
or node references retained. Baseline cases can stop at an earlier violated
invariant than the failure named by the case. Their total is a count of failed
scenarios, not a count of independent defects. In particular, the baseline
does not contain the new managed-map/regulator allocation sites, so those
injections are bypassed and the expected failure contract is violated.

## Boundaries

The host configuration follows the target's OF, CCF and regulator API paths.
The selected MT6755 ops have no dedicated-log, runtime-PM storage, reset-control
or DEVAPC registration callback. Hibernation is disabled in the target config.
Other IC resource acquisition and those optional callbacks need their own
lifecycle validation; the new MT6755 cleanup hook does not repair their
private globals.

These are serialized constructor/destructor tests. They do not establish
whole-driver concurrent-init safety, callback draining, live power-off
sequencing, or hardware register/EMI-MPU restoration. They do not emulate
provider internals, MMIO behavior or the scheduler. The existing callback and
pool shutdown suites cover their respective concurrency protocols. Real
ARM64 compilation and device validation remain separate evidence.

`test_wmt_platform_init.py` remains the platform-level regression suite. Its
abstract IC ops leave the new probe-required flag false and deliberately reject
any attempt to use the fixed-device probe adapter. This MT6755 suite supplies
the actual probe coverage; the platform suite checks wake-lock, stub publication
and backend ownership around the updated common hardware entry points.
