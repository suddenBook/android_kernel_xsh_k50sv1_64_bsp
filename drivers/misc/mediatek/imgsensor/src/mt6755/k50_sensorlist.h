/*
 * k50sv1_64_bsp sensor list.
 *
 * The shipped 3.18.119 kernel compiles ten imgsensor drivers, and its
 * camera HAL (libcameracustom.so) carries the same ten entries in the same
 * order.  ImgSensorDrv::impSearchSensor() hands the kernel the index of the
 * HAL entry it is probing (KDIMGSENSORIOC_X_SET_DRIVER), so the kernel
 * table must match the stock one index for index: the rear IMX145 is found
 * at index 1 and the front GC5025 at index 6 (work evidence E-161).
 *
 * Only the two fitted sensors carry a real driver.  The other eight keep
 * their stock id and name so that indices, the per-socket enable lists in
 * the device tree and the power-on table line up, and bind to a placeholder
 * that never finds a sensor.  Order, ids and names were read from the stock
 * vmlinux data (kdSensorList at 0xffffffc00112e400).
 */
#ifndef __K50_SENSORLIST_H__
#define __K50_SENSORLIST_H__

#define K50_GC5025MAIN_SENSOR_ID	0x5026
#define K50_SP2508_SENSOR_ID		0x2508

UINT32 K50_PLACEHOLDER_SensorInit(PSENSOR_FUNCTION_STRUCT *pfFunc);

/* Stock kdSensorList[] for k50sv1_64_bsp, index for index. */
#define K50_STOCK_SENSOR_LIST \
	{IMX135_SENSOR_ID, "imx135_mipi_raw", K50_PLACEHOLDER_SensorInit}, \
	{IMX145_SENSOR_ID, SENSOR_DRVNAME_IMX145_MIPI_RAW, IMX145_MIPI_RAW_SensorInit}, \
	{K50_SP2508_SENSOR_ID, "sp2508_mipi_raw", K50_PLACEHOLDER_SensorInit}, \
	{OV5670MIPI_SENSOR_ID, "ov5670_mipi_raw", K50_PLACEHOLDER_SensorInit}, \
	{GC2355_SENSOR_ID, "gc2355_mipi_raw", K50_PLACEHOLDER_SensorInit}, \
	{K50_GC5025MAIN_SENSOR_ID, "gc5025main_mipi_raw", K50_PLACEHOLDER_SensorInit}, \
	{GC5025_SENSOR_ID, SENSOR_DRVNAME_GC5025_MIPI_RAW, GC5025_MIPI_RAW_SensorInit}, \
	{IMX278_SENSOR_ID, "imx278_mipi_raw", K50_PLACEHOLDER_SensorInit}, \
	{S5K5E2YA_SENSOR_ID, "s5k5e2ya_mipi_raw", K50_PLACEHOLDER_SensorInit}, \
	{GC2235_SENSOR_ID, "gc2235_mipi_raw", K50_PLACEHOLDER_SensorInit},

#endif /* __K50_SENSORLIST_H__ */
