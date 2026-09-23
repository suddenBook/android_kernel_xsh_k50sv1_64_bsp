/*
 * Placeholder imgsensor driver for the k50sv1_64_bsp sensor list.
 *
 * Nine of the ten stock sensor-list entries name sensors that are not
 * fitted.  Their slots must stay in the kernel table so that the camera
 * HAL's index-based search lands on the right driver (see
 * k50_sensorlist.h), but there is nothing to talk to: every operation
 * fails and the alive check reports "no sensor", which is what the stock
 * drivers return for an absent part.
 */
#include <linux/kernel.h>
#include <linux/types.h>

#include "kd_camera_typedef.h"
#include "kd_imgsensor.h"
#include "kd_imgsensor_define.h"
#include "kd_imgsensor_errcode.h"
#include "k50_sensorlist.h"

#define K50_PLACEHOLDER_NO_SENSOR	0xFFFFFFFF

static MUINT32 k50_placeholder_open(void)
{
	return ERROR_SENSOR_CONNECT_FAIL;
}

static MUINT32 k50_placeholder_get_info(MSDK_SCENARIO_ID_ENUM scenario_id,
	MSDK_SENSOR_INFO_STRUCT *sensor_info,
	MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{
	return ERROR_SENSOR_CONNECT_FAIL;
}

static MUINT32 k50_placeholder_get_resolution(
	MSDK_SENSOR_RESOLUTION_INFO_STRUCT *sensor_resolution)
{
	return ERROR_SENSOR_CONNECT_FAIL;
}

static MUINT32 k50_placeholder_feature_control(
	MSDK_SENSOR_FEATURE_ENUM feature_id, MUINT8 *feature_para,
	MUINT32 *feature_para_len)
{
	if (feature_id == SENSOR_FEATURE_CHECK_SENSOR_ID &&
	    feature_para && feature_para_len &&
	    *feature_para_len >= sizeof(MUINT32))
		*(MUINT32 *)feature_para = K50_PLACEHOLDER_NO_SENSOR;

	return ERROR_SENSOR_CONNECT_FAIL;
}

static MUINT32 k50_placeholder_control(MSDK_SCENARIO_ID_ENUM scenario_id,
	MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *image_window,
	MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{
	return ERROR_SENSOR_CONNECT_FAIL;
}

static MUINT32 k50_placeholder_close(void)
{
	return ERROR_NONE;
}

static SENSOR_FUNCTION_STRUCT k50_placeholder_sensor_func = {
	.SensorOpen = k50_placeholder_open,
	.SensorGetInfo = k50_placeholder_get_info,
	.SensorGetResolution = k50_placeholder_get_resolution,
	.SensorFeatureControl = k50_placeholder_feature_control,
	.SensorControl = k50_placeholder_control,
	.SensorClose = k50_placeholder_close,
};

UINT32 K50_PLACEHOLDER_SensorInit(PSENSOR_FUNCTION_STRUCT *pfFunc)
{
	if (pfFunc)
		*pfFunc = &k50_placeholder_sensor_func;
	return ERROR_NONE;
}
