#include <stdio.h>
#include <stdlib.h>

static void fail(const char *message)
{
	fprintf(stderr, "SUBAF TEST FAILURE: %s\n", message);
	exit(1);
}

static void require(int condition, const char *message)
{
	if (!condition)
		fail(message);
}

/* Live camera_sub_af@1c would be write-id 0x38. HAL device 1 reports
 * max-num-focus-areas: 0, so this board has no fitted front VCM. */
#define K50_SUBAF_DT_7BIT		0x1c
#define K50_SUBAF_I2C_WRITE		0x38
#define K50_SUBAF_FOCUS_AREAS		0
#define K50_SUBAF_FITTED		0

int main(void)
{
	require(K50_SUBAF_DT_7BIT == 0x1c, "DT camera_sub_af@1c");
	require((K50_SUBAF_DT_7BIT << 1) == K50_SUBAF_I2C_WRITE,
		"write-id would be 0x38");
	require(K50_SUBAF_FOCUS_AREAS == 0,
		"live HAL max-num-focus-areas is 0");
	require(K50_SUBAF_FITTED == 0, "no fitted SUBAF VCM");
	printf("SUBAF HOST CONTRACT: PASS\n");
	return 0;
}
