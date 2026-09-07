#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
KERNEL_ROOT="$(cd "${HERE}/../../.." && pwd -P)"
OUTPUT="$(mktemp /tmp/k50-imgsensor-test.XXXXXX)"

cleanup() {
	if [[ -f "${OUTPUT}" && ! -L "${OUTPUT}" && \
	      "${OUTPUT}" == /tmp/k50-imgsensor-test.* ]]; then
		unlink -- "${OUTPUT}"
	fi
}
trap cleanup EXIT

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 \
	"${HERE}/k50_imgsensor_test.c" -o "${OUTPUT}"
"${OUTPUT}"

IMX="${KERNEL_ROOT}/drivers/misc/mediatek/imgsensor/src/mt6755/imx145_mipi_raw/imx145mipiraw_Sensor.c"
GC="${KERNEL_ROOT}/drivers/misc/mediatek/imgsensor/src/mt6755/gc5025_mipi_raw/gc5025mipi_Sensor.c"
LIST="${KERNEL_ROOT}/drivers/misc/mediatek/imgsensor/src/mt6755/kd_sensorlist.h"
IDS="${KERNEL_ROOT}/drivers/misc/mediatek/imgsensor/inc/kd_imgsensor.h"
DEFCONFIG="${KERNEL_ROOT}/arch/arm64/configs/k50sv1_64_bsp_defconfig"
DEBUG_DEFCONFIG="${KERNEL_ROOT}/arch/arm64/configs/k50sv1_64_bsp_debug_defconfig"

grep -Fq 'i2c_addr_table = {0x20, 0xff}' "${IMX}"
grep -Fq '.i2c_write_id = 0x20' "${IMX}"
grep -Fq 'IMX145_MIPI_RAW_SensorInit' "${IMX}"
if (( $(grep -Fc 'kdSetI2CSpeed(imgsensor_info.i2c_speed)' "${IMX}") < 2 )); then
	echo 'IMX145 does not set the stock 300 kHz speed on read/write helpers' >&2
	exit 1
fi
grep -Fq 'i2c_addr_table = {0x6e, 0xff}' "${GC}"
grep -Fq '.i2c_write_id = 0x6e' "${GC}"
grep -Fq 'GC5025_MIPI_RAW_SensorInit' "${GC}"
grep -Fq 'Makefile.custom' \
	"${KERNEL_ROOT}/drivers/misc/mediatek/imgsensor/src/mt6755/imx145_mipi_raw/Makefile"
grep -Fq 'Makefile.custom' \
	"${KERNEL_ROOT}/drivers/misc/mediatek/imgsensor/src/mt6755/gc5025_mipi_raw/Makefile"
grep -Fq 'IMX145_MIPI_RAW_SensorInit' "${LIST}"
grep -Fq 'GC5025_MIPI_RAW_SensorInit' "${LIST}"
grep -Fq '#define IMX145_SENSOR_ID                        0x0145' "${IDS}"
grep -Fq '#define GC5025_SENSOR_ID                        0x5025' "${IDS}"
grep -Fq '"imx145_mipi_raw"' "${IDS}"
grep -Fq '"gc5025_mipi_raw"' "${IDS}"
grep -Fq 'CONFIG_CUSTOM_KERNEL_IMGSENSOR="imx145_mipi_raw gc5025_mipi_raw"' \
	"${DEFCONFIG}"
grep -Fq 'CONFIG_CUSTOM_KERNEL_IMGSENSOR="imx145_mipi_raw gc5025_mipi_raw"' \
	"${DEBUG_DEFCONFIG}"
if grep -Fq 'gc5025main' "${GC}"; then
	echo 'unfitted GC5025-main variant was imported' >&2
	exit 1
fi
if grep -Eq 's5k2p8_mipi_raw|ov8858_mipi_raw' \
	"${DEFCONFIG}" "${DEBUG_DEFCONFIG}"; then
	echo 'unfitted Nokia camera menu remains enabled' >&2
	exit 1
fi

if [[ -n "${K50_IMX145_OBJECT:-}" ]]; then
	if [[ ! -f "${K50_IMX145_OBJECT}" || -L "${K50_IMX145_OBJECT}" ]]; then
		echo 'K50_IMX145_OBJECT must be a regular object file' >&2
		exit 1
	fi
	if "${CROSS_COMPILE:-}nm" -u "${K50_IMX145_OBJECT}" | \
		grep -qw write_cmos_sensor_8; then
		echo 'IMX145 object leaves write_cmos_sensor_8 unresolved' >&2
		exit 1
	fi
fi

printf 'IMGSENSOR SOURCE CONTRACT: PASS\n'
