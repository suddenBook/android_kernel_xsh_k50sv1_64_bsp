#define K50_IMGSENSOR_HOST_TEST
#include "../../../drivers/misc/mediatek/imgsensor/inc/kd_imgsensor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message)
{
	fprintf(stderr, "IMGSENSOR TEST FAILURE: %s\n", message);
	exit(1);
}

static void require(int condition, const char *message)
{
	if (!condition)
		fail(message);
}

int main(void)
{
	require(IMX145_SENSOR_ID == 0x0145, "IMX145 id is 0x0145");
	require(GC5025_SENSOR_ID == 0x5025, "GC5025 id is 0x5025");
	require(strcmp(SENSOR_DRVNAME_IMX145_MIPI_RAW, "imx145_mipi_raw") == 0,
		"live CAM[1] name is imx145_mipi_raw");
	require(strcmp(SENSOR_DRVNAME_GC5025_MIPI_RAW, "gc5025_mipi_raw") == 0,
		"live CAM[2] name is gc5025_mipi_raw");
	require(strcmp(SENSOR_DRVNAME_IMX145_MIPI_RAW, "imx145mipiraw") != 0,
		"do not collapse to Nokia-style imx145mipiraw");
	printf("IMGSENSOR HOST CONTRACT: PASS\n");
	return 0;
}
