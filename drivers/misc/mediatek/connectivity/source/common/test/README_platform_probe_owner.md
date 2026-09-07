# Platform probe owner regression

Build 17 binds `18070000.consys` to `mtk_wmt` and correctly removes the manual
bind/unbind attributes, but `/sys/bus/platform/drivers/mtk_wmt/module` is absent
on both fresh and normal boots. WMT supplies `.driver.owner = THIS_MODULE`.
In this 3.18 tree, `platform_driver_probe()` calls the registration macro from
the built-in platform core; its `THIS_MODULE` is NULL. The registration helper
then overwrites the caller's owner. `module_add_driver()` cannot create its
module/driver links without an owner or an alternate module name.

The fix passes the existing owner to `__platform_driver_register()`. It keeps
the exported probe function and its signature, synchronous binding, deferred
probe rejection, disabled manual binding and failed-bind rollback. Callers with
a NULL owner remain built-in. Other callers retain the owner they supplied;
this does not infer a module for callers that omitted the field.

`test_platform_probe_owner.py` extracts the actual registration, probe and
unregister functions from `drivers/base/platform.c`. The host adapter deliberately
defines the platform core's `THIS_MODULE` as NULL and observes ownership at
registration and removal. Eight ASan/UBSan cases cover module and built-in
success, no-device rollback and registration failure, retry after rollback and
distinct module owners. All eight pass with the fix. The build-17 source passes
the three built-in cases and fails all five module cases at owner preservation.

Run from any directory:

```sh
python3 test_platform_probe_owner.py --kernel /path/to/kernel --output /new/output
python3 test_platform_probe_owner.py --kernel /path/to/kernel --git-revision 372a643505f6b0aab0b3adbd150ecb9d9291d8d9 --output /new/baseline-output
```

The test uses deterministic bus/list adapters; it does not load or unload a
module. Source review of this tree establishes the missing sysfs ownership
relationship, not an observed use-after-free. WMT callback draining and resource
unregistration still provide their own teardown synchronization. The actual
`platform.o` compiles with the frozen ARM64 GCC 4.9 flags and `-Werror`; a full
kernel build and device module-link check remain required for adoption.
