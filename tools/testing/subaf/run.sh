#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
KERNEL_ROOT="$(cd "${HERE}/../../.." && pwd -P)"
OUTPUT="$(mktemp /tmp/k50-subaf-test.XXXXXX)"

cleanup() {
	if [[ -f "${OUTPUT}" && ! -L "${OUTPUT}" && \
	      "${OUTPUT}" == /tmp/k50-subaf-test.* ]]; then
		unlink -- "${OUTPUT}"
	fi
}
trap cleanup EXIT

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 \
	"${HERE}/k50_subaf_test.c" -o "${OUTPUT}"
"${OUTPUT}"

SUB="${KERNEL_ROOT}/drivers/misc/mediatek/lens/sub"
LENS="${KERNEL_ROOT}/drivers/misc/mediatek/lens"
MAIN_DW="${KERNEL_ROOT}/drivers/misc/mediatek/lens/main/common/dw9761af/DW9761AF.c"
LIST="${KERNEL_ROOT}/drivers/misc/mediatek/lens/main/inc/lens_list.h"

grep -Fq '#define DW9761AF_SetI2Cclient DW9761AF_SetI2Cclient_Main' "${LIST}"
grep -Fq '#define DW9761AF_Ioctl DW9761AF_Ioctl_Main' "${LIST}"
grep -Fq '#define DW9761AF_Release DW9761AF_Release_Main' "${LIST}"
if grep -REq 'DW9761AF_(SetI2Cclient|Ioctl|Release)_Sub' "${LENS}"; then
	echo 'DW9761AF was incorrectly aliased onto SUBAF anywhere in the lens tree' >&2
	exit 1
fi
if grep -RFq 'CONFIG_MTK_LENS_DW9761AF_SUPPORT' "${SUB}"; then
	echo 'DW9761AF was incorrectly enabled in the Sub tree' >&2
	exit 1
fi
grep -Fq 'DW9761AF_Ioctl' "${MAIN_DW}"
if grep -Fq 'DW9761AF_Ioctl_Sub' "${MAIN_DW}"; then
	echo 'MAINAF source exposes a forbidden Sub alias' >&2
	exit 1
fi

printf 'SUBAF SOURCE CONTRACT: PASS\n'
