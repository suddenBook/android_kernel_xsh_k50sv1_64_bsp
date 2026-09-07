#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
KERNEL_ROOT="$(cd "${HERE}/../../.." && pwd -P)"
OUTPUT="$(mktemp /tmp/k50-dw9761af-test.XXXXXX)"

cleanup() {
	if [[ -f "${OUTPUT}" && ! -L "${OUTPUT}" && \
	      "${OUTPUT}" == /tmp/k50-dw9761af-test.* ]]; then
		unlink -- "${OUTPUT}"
	fi
}
trap cleanup EXIT

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 \
	"${HERE}/k50_dw9761af_test.c" -o "${OUTPUT}"
"${OUTPUT}"

DRIVER="${KERNEL_ROOT}/drivers/misc/mediatek/lens/main/common/dw9761af/DW9761AF.c"
HEADER="${KERNEL_ROOT}/drivers/misc/mediatek/lens/main/common/dw9761af/k50_dw9761af.h"
LIST="${KERNEL_ROOT}/drivers/misc/mediatek/lens/main/inc/lens_list.h"
INFO="${KERNEL_ROOT}/drivers/misc/mediatek/lens/main/inc/lens_info.h"
MAIN="${KERNEL_ROOT}/drivers/misc/mediatek/lens/main/main_lens.c"
MAKEFILE="${KERNEL_ROOT}/drivers/misc/mediatek/lens/main/Makefile"
TOP_MAKEFILE="${KERNEL_ROOT}/drivers/misc/mediatek/lens/Makefile"
KCONFIG="${KERNEL_ROOT}/drivers/misc/mediatek/lens/Kconfig"
DEFCONFIG="${KERNEL_ROOT}/arch/arm64/configs/k50sv1_64_bsp_defconfig"
DEBUG_DEFCONFIG="${KERNEL_ROOT}/arch/arm64/configs/k50sv1_64_bsp_debug_defconfig"

grep -Fq 'DW9761AF_Ioctl' "${DRIVER}"
grep -Fq 'DW9761AF_Release' "${DRIVER}"
grep -Fq 'DW9761AF_SetI2Cclient' "${DRIVER}"
grep -Fq -- '------write pos register-----' "${DRIVER}"
grep -Fq 'K50_DW9761AF_I2C_WRITE' "${DRIVER}"
grep -Fq 'DW9761AF_Ioctl_Main' "${LIST}"
grep -Fq 'AFDRV_DW9761AF' "${INFO}"
grep -Fq 'AFDRV_DW9761AF' "${MAIN}"
grep -Fq 'common/dw9761af/DW9761AF.o' "${MAKEFILE}"
grep -Fq 'config MTK_LENS_DW9761AF_SUPPORT' "${KCONFIG}"
grep -Fq '# CONFIG_MTK_LENS_DW9761AF_SUPPORT is not set' "${DEFCONFIG}"
grep -Fq '# CONFIG_MTK_LENS_DW9761AF_SUPPORT is not set' "${DEBUG_DEFCONFIG}"
grep -Fq 'ret == 2 ? 0 : (ret < 0 ? ret : -EIO)' "${DRIVER}"
grep -Fq 'ret != 3' "${DRIVER}"
grep -Fq 'return -EFAULT' "${DRIVER}"
if grep -Eq 'main2/|sub/' "${TOP_MAKEFILE}"; then
	echo 'unfitted MAIN2 or SUBAF stack remains linked' >&2
	exit 1
fi
if grep -E '^mainaf-y.*common/' "${MAKEFILE}" | \
	grep -Ev 'common/(dw9761af|bu6424af)/' | grep -q .; then
	echo 'an unexpected MAINAF implementation remains linked' >&2
	exit 1
fi
if grep -Fq 'proc_create("driver/MAINAF"' "${MAIN}"; then
	echo 'writable MAINAF factory proc surface remains enabled' >&2
	exit 1
fi
if grep -Eq 'EXPORT_SYMBOL|dontaudit' "${DRIVER}" "${HEADER}"; then
	echo 'DW9761AF exposes a forbidden symbol or policy workaround' >&2
	exit 1
fi

printf 'DW9761AF SOURCE CONTRACT: PASS\n'
