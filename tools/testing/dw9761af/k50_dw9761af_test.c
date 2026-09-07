#define K50_DW9761AF_HOST_TEST
#include "../../../drivers/misc/mediatek/lens/main/common/dw9761af/k50_dw9761af.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message)
{
	fprintf(stderr, "DW9761AF TEST FAILURE: %s\n", message);
	exit(1);
}

static void require(int condition, const char *message)
{
	if (!condition)
		fail(message);
}

int main(void)
{
	require(K50_DW9761AF_I2C_WRITE == 0x18, "MAINAF 8-bit write is 0x18");
	require(K50_DW9761AF_I2C_7BIT == 0x0c, "DT camera_main_af@0c");
	require(K50_DW9761AF_MACRO == 1023, "10-bit DAC");
	require(strcmp(K50_DW9761AF_DRVNAME, "DW9761AF_DRV") == 0,
		"stock log tag is DW9761AF_DRV");
	require(strcmp(K50_DW9761AF_AFDRV, "DW9761AF") == 0,
		"HAL motor name is DW9761AF");
	require(k50_dw9761af_addr_ok(0x18), "0x18 accepted");
	require(!k50_dw9761af_addr_ok(0x1c), "SUBAF 0x1c is not MAIN");
	require(k50_dw9761af_pos_from_bytes(
		k50_dw9761af_pos_msb(0x145),
		k50_dw9761af_pos_lsb(0x145)) == 0x145,
		"10-bit position round-trips");
	require(k50_dw9761af_pos_msb(1023) == 0x03, "max MSB is 2 bits");
	require(K50_DW9761AF_REG_PD == 0x02, "PD register is 0x02");
	require(K50_DW9761AF_REG_POS == 0x03, "DAC starts at 0x03");
	require(K50_DW9761AF_SAC == 0x60, "SAC mode 0x60");
	require(K50_DW9761AF_FREQ == 0x3e, "Tvib 0x3e");
	require(K50_DW9761AF_PRELOAD == 0x73, "preload 0x73");
	printf("DW9761AF HOST CONTRACT: PASS\n");
	return 0;
}
