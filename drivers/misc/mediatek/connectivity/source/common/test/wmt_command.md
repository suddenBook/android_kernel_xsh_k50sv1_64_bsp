# WMT userspace command lifecycle

`wmt_ctrl_ul_cmd()` and the `/dev/stpwmt` read/write callbacks share one command
buffer and one completion. A request remains owned until its response, timeout
or cancellation has been consumed by its producer. The poll-ready bit describes
an unread command; it is not the ownership lock.

The library mutex covers buffer initialization, successful user copying,
response publication and cleanup. The completion and complete command string
are initialized before the poll-ready bit is published. A zero-length read
returns zero without consuming anything. A short destination returns `EMSGSIZE`,
and a user-copy failure returns `EFAULT`; either can be followed by a successful
read of the same request. Only a complete copy changes the request to delivered.
An unsolicited or duplicate response returns `EINVAL`. A missing response
returns `ETIMEDOUT`; hardware-reset command cleanup wakes the producer with
`ECANCELED`. A second producer receives `EBUSY` until the first producer leaves.

The existing launcher reads at most 255 bytes into a cleared 256-byte buffer
and writes the response's string length, without its terminator. Command text,
the maximum length, and the ioctl layouts remain unchanged. The new library
functions are internal to the WMT module.

The legacy response has no transaction identifier. The guards reject a response
while idle, before a read, or after a response has already been accepted. They
cannot identify an old response after a later request has also been delivered
to a writer. The current launcher processes read/handle/write sequentially;
a general guarantee for independently interleaved writers requires a paired
protocol change. These tests do not claim that guarantee.

## Host verification

From the kernel root, choose unused output directories:

```sh
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_command.py \
  --output /tmp/wmt-command-directed
python3 drivers/misc/mediatek/connectivity/source/common/test/test_wmt_command.py \
  --sanitizer thread --output /tmp/wmt-command-threads
```

`--kernel /path/to/baseline` runs the same fixtures against another source tree.
The runner extracts the real producer, library helpers and read/write functions.
For an older tree, its command-specific hardware-reset clear statement is
extracted into a helper; the rest of the hardware reset is outside this fixture.
Kernel user copying, bit operations, mutexes, completions and logging are host
substitutes. Generated C, exact source/runner hashes, compiler arguments and
every case's output are retained.

The directed cases cover command loss, publication ordering, ownership through
the response, timeout/cancellation cleanup, response errors and string limits
under AddressSanitizer and UndefinedBehaviorSanitizer. The ThreadSanitizer cases
run real competing threads: a second producer after the first command has been
read; two readers delivering 2,000 uniquely numbered commands exactly once; and
reset while the user copy holds the command lock.

These are control-flow and concurrency tests. A kernel build and handset
initialization/radio tests are still required. Do not inject responses into a
running handset's command stream: that would interfere with its actual launcher.
