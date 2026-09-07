#!/bin/sh
# Exercise negative Kconfig/link closures against a known full baseline.
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 BASELINE_CONFIG CROSS_COMPILE_PREFIX" >&2
	exit 2
fi

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
kernel_root=$(CDPATH= cd -- "$test_dir/../../../../../.." && pwd)
baseline=$(readlink -f "$1")
cross_compile=$(readlink -f "$2")
fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/k50-ft8057-kconfig.XXXXXXXX")

cleanup()
{
	rm -rf -- "$fixture_root"
}
trap cleanup EXIT HUP INT TERM

run_merge()
{
	output=$1
	fragment=$2
	mkdir -p "$output"
	(
		cd "$kernel_root"
		ARCH=arm64 CROSS_COMPILE="$cross_compile" \
			scripts/kconfig/merge_config.sh -O "$output" \
			"$baseline" "$fragment"
	) >"$output/merge.log" 2>&1
}

module_output="$fixture_root/tpd-module"
run_merge "$module_output" "$test_dir/kconfig-tpd-module.fragment"
grep -Fq 'CONFIG_TOUCHSCREEN_MTK=m' "$module_output/.config"
if grep -Fq 'CONFIG_TOUCHSCREEN_MTK_FOCALTECH_FT8057=y' \
	"$module_output/.config"; then
	echo 'FT8057 must not be selectable with module-only MTK TPD' >&2
	exit 1
fi

panel_output="$fixture_root/unselected-panel"
run_merge "$panel_output" "$test_dir/kconfig-unselected-panel.fragment"
grep -Fq 'CONFIG_TOUCHSCREEN_MTK_FOCALTECH_FT8057=y' \
	"$panel_output/.config"
grep -Fq 'CONFIG_K50_LCM_BIAS=y' "$panel_output/.config"
make -s -C "$kernel_root" O="$panel_output" ARCH=arm64 \
	CROSS_COMPILE="$cross_compile" LOCALVERSION= -j4 \
	drivers/misc/mediatek/lcm/ >"$panel_output/build.log" 2>&1
nm "$panel_output/drivers/misc/mediatek/lcm/built-in.o" | \
	grep -Eq ' [Tt] lcm_enp2_setting$'

panel_only_output="$fixture_root/panel-only"
run_merge "$panel_only_output" "$test_dir/kconfig-panel-only.fragment"
if grep -Fq 'CONFIG_K50_LCM_BIAS=y' "$panel_only_output/.config"; then
	echo 'panel-only fixture must exercise the LCM-list provider path' >&2
	exit 1
fi
make -s -C "$kernel_root" O="$panel_only_output" ARCH=arm64 \
	CROSS_COMPILE="$cross_compile" LOCALVERSION= -j4 \
	drivers/misc/mediatek/lcm/ >"$panel_only_output/build.log" 2>&1
provider_count=$(nm "$panel_only_output/drivers/misc/mediatek/lcm/built-in.o" | \
	awk '$3 == "lcm_enp2_setting" { count++ } END { print count + 0 }')
if [ "$provider_count" -ne 1 ]; then
	echo "panel-only path built $provider_count bias providers" >&2
	exit 1
fi

echo 'FT8057 negative Kconfig gates: PASS'
