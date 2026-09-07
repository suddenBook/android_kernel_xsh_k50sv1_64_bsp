# FocalTech FT8057 source candidate for k50sv1_64_bsp.

ccflags-y += -I$(srctree)/drivers/input/touchscreen/mediatek
ccflags-y += -I$(srctree)/drivers/misc/mediatek/lcm/inc

obj-$(CONFIG_TOUCHSCREEN_MTK_FOCALTECH_FT8057) += focaltech_ft8057.o
focaltech_ft8057-y := focaltech_core.o focaltech_i2c.o
