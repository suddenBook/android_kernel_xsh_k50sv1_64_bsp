#!/bin/sh
# Confirm that the prepare seam does not change existing MTK TPD symbol CRCs.
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 BASELINE_MTK_TPD_O CANDIDATE_MTK_TPD_O" >&2
	exit 2
fi

baseline=$1
candidate=$2

symbol_crc()
{
	object=$1
	symbol=$2
	nm "$object" | awk -v target="__crc_$symbol" '$3 == target { print $1 }'
}

for symbol in tpd_driver_add tpd_driver_remove; do
	baseline_crc=$(symbol_crc "$baseline" "$symbol")
	candidate_crc=$(symbol_crc "$candidate" "$symbol")
	if [ -z "$baseline_crc" ] || [ -z "$candidate_crc" ]; then
		echo "missing CRC for $symbol" >&2
		exit 1
	fi
	if [ "$baseline_crc" != "$candidate_crc" ]; then
		echo "$symbol CRC changed: $baseline_crc -> $candidate_crc" >&2
		exit 1
	fi
	done

echo 'MTK TPD existing-symbol ABI gates: PASS'
