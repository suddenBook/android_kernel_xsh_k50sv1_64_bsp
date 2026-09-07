#!/bin/sh
# Focused source gates for the FT8057 candidate. Run from any directory.
set -eu

driver_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel_root=$(CDPATH= cd -- "$driver_dir/../../../../.." && pwd)
core="$driver_dir/focaltech_core.c"
transport="$driver_dir/focaltech_i2c.c"
header="$driver_dir/focaltech_core.h"
mtk_tpd="$kernel_root/drivers/input/touchscreen/mediatek/mtk_tpd.c"
tpd_header="$kernel_root/drivers/input/touchscreen/mediatek/tpd.h"
power_header="$kernel_root/include/linux/input/mtk_tpd_power.h"
panel="$kernel_root/drivers/misc/mediatek/lcm/ft8057s_inx_hdplus1560/ft8057s_inx_hdplus1560.c"

require_fixed()
{
	needle=$1
	file=$2
	if ! grep -Fq -- "$needle" "$file"; then
		echo "missing required source invariant: $needle" >&2
		exit 1
	fi
}

function_body()
{
	name=$1
	file=$2
	sed -n "/^static .* $name(/,/^}/p" "$file"
}

require_order()
{
	body=$1
	shift
	previous=0
	for token in "$@"; do
		line=$(printf '%s\n' "$body" | grep -n -F -m1 -- "$token" | \
			cut -d: -f1 || true)
		if [ -z "$line" ] || [ "$line" -le "$previous" ]; then
			echo "missing or misordered source invariant: $token" >&2
			exit 1
		fi
		previous=$line
	done
}

require_order_occurrences()
{
	body=$1
	shift
	for token in "$@"; do
		line=$(printf '%s\n' "$body" | grep -n -F -m1 -- "$token" | \
			cut -d: -f1 || true)
		if [ -z "$line" ]; then
			echo "missing or misordered source invariant: $token" >&2
			exit 1
		fi
		body=$(printf '%s\n' "$body" | tail -n "+$((line + 1))")
	done
}

require_regex()
{
	pattern=$1
	file=$2
	if ! grep -E -q -- "$pattern" "$file"; then
		echo "missing required source invariant: $pattern" >&2
		exit 1
	fi
}

reject_tree()
{
	pattern=$1
	if grep -R -E -n --exclude=static-gates.sh -- "$pattern" \
		"$driver_dir"; then
		echo "forbidden optional feature found: $pattern" >&2
		exit 1
	fi
}

require_fixed '{ 0x19, 0x86, 0x42, 0x86, 0x42, 0x86, 0xC2, 0x00, 0x00 }' "$core"
require_fixed '{ 0x19, 0x86, 0x32, 0x86, 0x32, 0x86, 0xC2, 0x00, 0x00 }' "$core"
require_fixed '{ 0x28, 0x80, 0x57, 0x80, 0x57, 0x80, 0xA7, 0x00, 0x00 }' "$core"
require_regex '^#define FTS_BUS_BUFFER_SIZE[[:space:]]+4096$' "$header"
require_regex '^#define FTS_I2C_SLAVE_ADDR[[:space:]]+0x38$' "$header"
require_regex '^#define FTS_I2C_RETRIES[[:space:]]+3$' "$header"
require_regex '^#define FTS_X_MAX[[:space:]]+720$' "$header"
require_regex '^#define FTS_Y_MAX[[:space:]]+1560$' "$header"
require_regex '^#define FTS_REG_POWER_MODE[[:space:]]+0xA5$' "$header"
require_regex '^#define FTS_REG_POWER_SLEEP[[:space:]]+0x03$' "$header"
require_regex '^#define FTS_REG_PEN_MODE[[:space:]]+0xC0$' "$header"
require_fixed 'return -EIO;' "$transport"
require_fixed 'if (!ret)' "$transport"
require_fixed 'memcpy(read_buf, ts->bus_rx_buf, read_len);' "$transport"
require_fixed 'ret = tpd_prepare_suspend();' "$panel"
require_fixed 'event_type == 0x02' "$core"

pen_mode=$(sed -n '/^int fts_enter_pen_mode(/,/^}/p' "$core")
if printf '%s\n' "$pen_mode" | grep -E -q 'if[[:space:]]*\(!enable\)'; then
	echo 'Hall false path must not return before programming pen mode' >&2
	exit 1
fi
if [ "$(printf '%s\n' "$pen_mode" | grep -Fc 'fts_write_reg(')" -ne 1 ] || \
	[ "$(printf '%s\n' "$pen_mode" | grep -Fc 'fts_read_reg(')" -ne 1 ]; then
	echo 'Hall pen-mode update must perform exactly one write and one read' >&2
	exit 1
fi
require_order_occurrences "$pen_mode" \
	'u8 requested = !!enable;' \
	'ret = fts_write_reg(ts, FTS_REG_PEN_MODE, requested);' \
	'if (ret < 0)' \
	'return ret;' \
	'ret = fts_read_reg(ts, FTS_REG_PEN_MODE, &readback);' \
	'if (ret < 0)' \
	'return ret;' \
	'return readback == requested ? 0 : -EIO;'

event_count_branch=$(sed -n '/} else if (event_type == 0x02)/,/} else {/p' \
	"$core")
if ! printf '%s\n' "$event_count_branch" | grep -Fq 'snapshot = true;'; then
	echo 'V3.4 event-count packets must rebuild the active slot snapshot' >&2
	exit 1
fi

if sed -n '/^struct tpd_driver_t {/,/^};/p' "$tpd_header" | \
	grep -Fq 'prepare_suspend'; then
	echo 'prepare callback must not change the exported tpd_driver_t ABI' >&2
	exit 1
fi
require_fixed 'static int (*tpd_prepare_list[TP_DRV_MAX_COUNT])' "$mtk_tpd"
require_fixed 'int tpd_set_prepare_suspend(struct tpd_driver_t *tpd_drv,' "$mtk_tpd"
require_fixed 'int tpd_prepare_suspend(void);' "$power_header"
if grep -Fq 'tpd_set_prepare_suspend' "$power_header"; then
	echo 'public power header must expose only the prepare call' >&2
	exit 1
fi

read_path=$(sed -n '/^int fts_read(/,/^}/p' "$transport")
if ! printf '%s\n' "$read_path" | grep -B1 -F \
	'memcpy(read_buf, ts->bus_rx_buf, read_len);' | grep -Fq 'if (!ret)'; then
	echo 'receive data must only reach the caller after a complete transfer' >&2
	exit 1
fi

phase_two=$(sed -n '/static void fts_suspend_callback/,/^}/p' "$core")
if printf '%s\n' "$phase_two" | \
	grep -E 'fts_(read|write|bus)(_|\()|i2c_|master_(send|recv)|i2c_transfer'; then
	echo 'phase 2 must never perform direct or indirect bus I/O' >&2
	exit 1
fi
require_order "$phase_two" \
	'fts_release_all_fingers(ts);' \
	'tpd_gpio_output(GTP_RST_PORT, 0);' \
	'msleep(5);' \
	'lcm_enp2_setting(false);'

phase_one=$(function_body fts_prepare_suspend_callback "$core")
require_order "$phase_one" \
	'WRITE_ONCE(ts->pm_state, FTS_PM_PREPARING);' \
	'fts_irq_disable_sync(ts);' \
	'fts_write_reg(ts, FTS_REG_POWER_MODE, FTS_REG_POWER_SLEEP);'

resume=$(function_body fts_resume_callback "$core")
require_order "$resume" \
	'lcm_enp2_setting(true);' \
	'fts_reset(200);' \
	'fts_wait_valid(ts);' \
	'fts_restore_hall_mode(ts);' \
	'WRITE_ONCE(ts->pm_state, FTS_PM_ACTIVE);' \
	'fts_irq_enable(ts);'

panel_suspend=$(function_body lcm_suspend "$panel")
require_order "$panel_suspend" \
	'tpd_prepare_suspend();' \
	'push_table(suspend_setting, ARRAY_SIZE(suspend_setting));'

for callback in fts_prepare_suspend_callback fts_suspend_callback \
	fts_resume_callback; do
	body=$(function_body "$callback" "$core")
	require_order "$body" \
		'mutex_lock(&fts_lifetime_lock);' \
		'ts = fts_data;' \
		'mutex_lock(&ts->pm_lock);'
done

remove=$(function_body fts_i2c_remove "$core")
require_order "$remove" \
	'mutex_lock(&fts_lifetime_lock);' \
	'fts_data = NULL;' \
	'fts_irq_disable_sync(ts);' \
	'input_unregister_device(ts->input_dev);' \
	'fts_bus_exit(ts);' \
	'mutex_unlock(&fts_lifetime_lock);'

reject_tree 'request_firmware|firmware_upgrade|auto_upgrade|proc_create|debugfs_create'
reject_tree 'FTS_(GESTURE|PROXIMITY|ESDCHECK|TEST|PEN)_EN'

echo 'FT8057 static gates: PASS'
