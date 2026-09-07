#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
KERNEL_ROOT="$(cd "${HERE}/../../.." && pwd -P)"
OUTPUT="$(mktemp /tmp/k50-cam-cal-test.XXXXXX)"

cleanup() {
	if [[ -f "${OUTPUT}" && ! -L "${OUTPUT}" && \
	      "${OUTPUT}" == /tmp/k50-cam-cal-test.* ]]; then
		unlink -- "${OUTPUT}"
	fi
}
trap cleanup EXIT

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 \
	"${HERE}/k50_cam_cal_test.c" -o "${OUTPUT}"
"${OUTPUT}"

HEADER="${KERNEL_ROOT}/drivers/misc/mediatek/cam_cal/inc/k50_cam_cal.h"
MAIN="${KERNEL_ROOT}/drivers/misc/mediatek/cam_cal/src/legacy/mt6755/s5k2p8_eeprom/s5k2p8_eeprom.c"
SUB="${KERNEL_ROOT}/drivers/misc/mediatek/cam_cal/src/legacy/mt6755/ov8858_eeprom/ov8858_eeprom.c"
DEFCONFIG="${KERNEL_ROOT}/arch/arm64/configs/k50sv1_64_bsp_defconfig"
DEBUG_DEFCONFIG="${KERNEL_ROOT}/arch/arm64/configs/k50sv1_64_bsp_debug_defconfig"

grep -Fq 'k50_cam_cal.h' "${MAIN}"
grep -Fq 'k50_cam_cal.h' "${SUB}"
grep -Fq 'K50_CAMCAL_DRV1_BUS' "${MAIN}"
grep -Fq 'K50_CAMCAL_DRV1_NAME' "${MAIN}"
grep -Fq 'K50_CAMCAL_SLAVE_8BIT' "${MAIN}"
grep -Fq 'I2C_BOARD_INFO(CAM_CAL_DRVNAME, K50_CAMCAL_SLAVE_7BIT)' "${MAIN}"
grep -Fq 'K50_CAMCAL_DRV2_BUS' "${SUB}"
grep -Fq 'K50_CAMCAL_DRV2_NAME' "${SUB}"
grep -Fq 'K50_CAMCAL_SLAVE_8BIT' "${SUB}"
grep -Fq 'I2C_BOARD_INFO(CAM_CAL_DRVNAME, K50_CAMCAL_SLAVE_7BIT)' "${SUB}"
grep -Fq 'CONFIG_CUSTOM_KERNEL_CAM_CAL_DRV="s5k2p8_eeprom ov8858_eeprom"' \
	"${DEFCONFIG}"
grep -Fq 'CONFIG_CUSTOM_KERNEL_CAM_CAL_DRV="s5k2p8_eeprom ov8858_eeprom"' \
	"${DEBUG_DEFCONFIG}"
if grep -Eq 'EXPORT_SYMBOL|dontaudit' "${HEADER}"; then
	echo 'CAM_CAL identity header exposes a forbidden symbol or workaround' >&2
	exit 1
fi

printf 'CAMCAL SOURCE CONTRACT: PASS\n'
