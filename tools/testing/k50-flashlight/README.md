# K50 flashlight host regression

Run `python3 tools/testing/k50-flashlight/run.py` with a host C compiler and pthreads.
Set `TMPDIR` to control where the temporary executable is created.

The runner compiles the complete production driver body with the real MT6353
register definitions and flashlight ioctl header. Only kernel service calls are
substituted. It also compiles the actual `setFlashDrv()` selector and index helpers
to verify open failure propagation, callback publication and busy-owner retention.
The complete `flashlight_ioctl_core()` is exercised with full and partial user-copy
faults, normal torch ioctls, and UNINIT cleanup after successful or failed shutdown.
The PMIC trace oracle contains the 14 register writes decoded from
the stock K50 arm64 flash implementation. Tests cover fixed current settings,
torch and timed exposure, invalid timeout values, failures at every initialization
write, partial enable/off/release failures, concurrent open, and queued/running
timeout work during release and reopen. A blocked worker uses real pthreads;
the test runner terminates a deadlock after 20 seconds. AddressSanitizer and
UndefinedBehaviorSanitizer are enabled.

These tests verify the driver contract and cancellation ordering. Actual light
output and camera capture timing still require a device test after booting the
new kernel.

The LED-class cases cover queued completion, exclusivity with camera ioctls,
PMIC failures and readback, PBM-before-enable ordering, independent low-battery
sources, ON overtaking forced OFF, shutdown and teardown. These use the actual
callbacks and status formatter, with controllable work dispatch and PMIC faults.
