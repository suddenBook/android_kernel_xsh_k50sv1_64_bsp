#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
OUT=${TMPDIR:-/tmp}/k50-gpiokey-test.$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM

${HOSTCC:-cc} -std=c99 -Wall -Wextra -Werror \
	-I"$ROOT" \
	"$ROOT/tools/testing/gpiokey/k50_gpiokey_test.c" \
	-o "$OUT"
"$OUT"

SOURCE="$ROOT/drivers/misc/mediatek/gpiokey/k50_gpiokey.c"
TOUCH="$ROOT/drivers/input/touchscreen/mediatek/focaltech_touch_ft8057/focaltech_core.c"

require_source()
{
	pattern=$1
	description=$2
	if ! grep -Eq "$pattern" "$SOURCE"; then
		echo "missing source contract: $description" >&2
		exit 1
	fi
}

require_source 'compatible = "mediatek,gpiokey"' 'stock Hall compatible'
require_source 'K50_HALL_INPUT_NAME.*"HALL_DEV"' 'stock input identity'
require_source 'DEVICE_ATTR_RO\(hall_status\)' 'read-only Hall status'
require_source 'KEY_WAKEUP, 1' 'KEY_WAKEUP down pulse'
require_source 'KEY_WAKEUP, 0' 'KEY_WAKEUP up pulse'
require_source 'k50_ft8057_set_hall_state' 'FT8057 C0 handoff'
require_source 'gpio_to_irq\(K50_HALL_GPIO\)' 'GPIO-to-IRQ primary mapping'
require_source 'K50_HALL_EINT_COMPAT' 'stock EINT fallback'

if grep -Fq 'gpio_set_debounce' "$SOURCE"; then
	echo 'Hall debounce must have one 7 ms implementation, not hardware plus sleep' >&2
	exit 1
fi

if grep -Eq 'EXPORT_SYMBOL|DEVICE_ATTR.*store|proc_create|debugfs|unlocked_ioctl' \
	"$SOURCE"; then
	echo 'forbidden Hall debug, write, ioctl, or module ABI surface' >&2
	exit 1
fi
if grep -Eq 'input_report_key\([^,]+, KEY_SLEEP' "$SOURCE"; then
	echo 'KEY_SLEEP must be advertised but never emitted' >&2
	exit 1
fi
if ! grep -Fq 'int k50_ft8057_set_hall_state(bool state)' "$TOUCH"; then
	echo 'FT8057 Hall setter is missing' >&2
	exit 1
fi
if ! grep -Fq 'ts->pm_state != FTS_PM_ACTIVE' "$TOUCH"; then
	echo 'FT8057 Hall setter lacks the ACTIVE-state gate' >&2
	exit 1
fi

echo 'GPIOKEY SOURCE CONTRACT: PASS'
