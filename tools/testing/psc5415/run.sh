#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
KERNEL_ROOT="$(cd "${HERE}/../../.." && pwd -P)"
OUTPUT="$(mktemp /tmp/psc5415-test.XXXXXX)"
KCONFIG_OUTPUT="$(mktemp -d /tmp/psc5415-kconfig-negative.XXXXXX)"
KCONFIG_LOG="${KCONFIG_OUTPUT}/kconfig.log"

cleanup() {
	if [[ -f "${OUTPUT}" && ! -L "${OUTPUT}" && \
	      "${OUTPUT}" == /tmp/psc5415-test.* ]]; then
		unlink -- "${OUTPUT}"
	fi
	if [[ -d "${KCONFIG_OUTPUT}" && ! -L "${KCONFIG_OUTPUT}" && \
	      "${KCONFIG_OUTPUT}" == /tmp/psc5415-kconfig-negative.* ]]; then
		rm -r -- "${KCONFIG_OUTPUT}"
	fi
}
trap cleanup EXIT

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 \
	"${HERE}/psc5415_test.c" -o "${OUTPUT}"
"${OUTPUT}"

DRIVER="${HERE}/../../../drivers/misc/mediatek/power/mt6755/psc5415.c"
ADAPTER="${HERE}/../../../drivers/misc/mediatek/power/mt6755/charging_hw_psc5415.c"
CHARGER_KCONFIG="${HERE}/../../../drivers/misc/mediatek/power/Kconfig"
CHARGER_MAKEFILE="${HERE}/../../../drivers/misc/mediatek/power/mt6755/Makefile"
POWER_MAKEFILE="${HERE}/../../../drivers/power/mediatek/Makefile"

grep -Fq 'psc5415_transfer_status(ret, PSC5415_COMBINED_READ_LENGTH)' "${DRIVER}"
grep -Fq 'psc5415_transfer_status(ret, sizeof(buffer))' "${DRIVER}"
[[ "$(grep -Fc 'client->ext_flag = original_ext_flag;' "${DRIVER}")" -eq 2 ]]
grep -Fq '.suppress_bind_attrs = true' "${DRIVER}"
grep -Fq 'client->addr != PSC5415_I2C_ADDRESS' "${DRIVER}"
grep -Fq 'ret = -EBUSY;' "${DRIVER}"
! grep -Eq '\.remove[[:space:]]*=|module_exit' "${DRIVER}"
! grep -Fq 'DEVICE_ATTR' "${DRIVER}"
! grep -Eq 'k[mz]alloc|devm_k[mz]alloc' "${DRIVER}"
! grep -Eq 'sysfs_|platform_(device|driver)_register' "${DRIVER}"
grep -Fq '[CHARGING_CMD_ENABLE_OTG] = charging_enable_otg' "${ADAPTER}"
grep -Fq 'BUILD_BUG_ON(CHARGING_CMD_NUMBER != 63)' "${ADAPTER}"
grep -Fq 'psc5415_charging_func[CHARGING_CMD_NUMBER]' "${ADAPTER}"
grep -Fq 'DEFINE_MUTEX(psc5415_command_lock)' "${ADAPTER}"
grep -Fq 'mutex_lock(&psc5415_command_lock)' "${ADAPTER}"
grep -Fq 'mutex_unlock(&psc5415_command_lock)' "${ADAPTER}"
grep -Fq 'config MTK_PSC5415_SUPPORT' "${CHARGER_KCONFIG}"
grep -Fq 'depends on I2C=y' "${CHARGER_KCONFIG}"
grep -Fq 'depends on MTK_I2C_EXTENSION' "${CHARGER_KCONFIG}"
grep -Fq 'depends on MTK_SMART_BATTERY=y' "${CHARGER_KCONFIG}"
grep -Fq 'depends on !MTK_CHARGER_INTERFACE' "${CHARGER_KCONFIG}"
grep -Fq 'psc5415.o charging_hw_psc5415.o' "${CHARGER_MAKEFILE}"
grep -Fq 'CONFIG_MTK_PSC5415_SUPPORT' "${POWER_MAKEFILE}"
grep -Fq 'switch_charging.o' "${POWER_MAKEFILE}"

printf 'PSC5415 SOURCE CONTRACT: PASS\n'

if ! KCONFIG_ALLCONFIG="${HERE}/kconfig-smart-battery-module.config" \
	make -s -C "${KERNEL_ROOT}" O="${KCONFIG_OUTPUT}" ARCH=arm64 \
	allnoconfig >"${KCONFIG_LOG}" 2>&1; then
	cat "${KCONFIG_LOG}" >&2
	exit 1
fi
grep -Fq 'CONFIG_MTK_SMART_BATTERY=m' "${KCONFIG_OUTPUT}/.config"
! grep -Eq '^CONFIG_MTK_PSC5415_SUPPORT=(y|m)$' \
	"${KCONFIG_OUTPUT}/.config"

printf 'PSC5415 KCONFIG MODULE-REJECTION: PASS\n'
