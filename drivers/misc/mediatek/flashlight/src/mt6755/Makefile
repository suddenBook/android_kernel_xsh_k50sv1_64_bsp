#
# Copyright (C) 2015 MediaTek Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#

#
# Makefile for misc devices that really don't fit anywhere else.
#


obj-$(CONFIG_MTK_FLASHLIGHT)    += flashlight_top.o

flashlight_top-y				+= kd_flashlightlist.o
flashlight_top-y				+= strobe_main_sid2_part2.o
flashlight_top-y				+= strobe_sub_sid2_part1.o
flashlight_top-y				+= strobe_sub_sid2_part2.o
flashlight_top-y				+= sub_strobe.o
flashlight_top-y				+= strobe_main_sid1_part2.o
flashlight_top-y				+= strobe_part_id.o
flashlight_top-y				+= strobe_sub_sid1_part2.o
flashlight_top-y				+= strobe_main_sid2_part1.o

obj-$(CONFIG_MTK_FLASHLIGHT)    += flashlight_device.o
flashlight_device-y             += constant_flashlight/leds_strobe.o
