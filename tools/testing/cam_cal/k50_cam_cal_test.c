#define K50_CAMCAL_HOST_TEST
#include "../../../drivers/misc/mediatek/cam_cal/inc/k50_cam_cal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message)
{
	fprintf(stderr, "CAMCAL TEST FAILURE: %s\n", message);
	exit(1);
}

static void require(int condition, const char *message)
{
	if (!condition)
		fail(message);
}

int main(void)
{
	require(K50_CAMCAL_SLAVE_8BIT == 0xA2, "8-bit write-id is 0xA2");
	require(K50_CAMCAL_SLAVE_7BIT == 0x51, "live 7-bit address is 0x51");
	require((K50_CAMCAL_SLAVE_7BIT << 1) == K50_CAMCAL_SLAVE_8BIT,
		"7-bit 0x51 is 8-bit 0xA2");
	require(K50_CAMCAL_DRV1_BUS == 2, "CAM_CAL_DRV1 is i2c-2");
	require(K50_CAMCAL_DRV2_BUS == 0, "CAM_CAL_DRV2 is i2c-0");
	require(strcmp(K50_CAMCAL_DRV1_NAME, "CAM_CAL_DRV1") == 0,
		"live name CAM_CAL_DRV1");
	require(strcmp(K50_CAMCAL_DRV2_NAME, "CAM_CAL_DRV2") == 0,
		"live name CAM_CAL_DRV2");
	require(k50_camcal_slave_ok(0xA2), "0xA2 accepted");
	require(!k50_camcal_slave_ok(0xA0), "0xA0 is not this board");
	printf("CAMCAL HOST CONTRACT: PASS\n");
	return 0;
}
