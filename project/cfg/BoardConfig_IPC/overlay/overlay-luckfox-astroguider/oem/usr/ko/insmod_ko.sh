#!/bin/sh
#
# OpenAstroGuider: trimmed insmod_ko.sh
# Raw V4L2 capture only -- loads rkcif + CSI2 PHY + the sensor in use.
# Skips ISP, RGA, video codec, NPU, audio, motor and rockit/rve entirely.
cmd=`realpath $0`
_DIR=`dirname $cmd`
cd $_DIR

udevadm control --stop-exec-queue

__insmod()
{
	if [ -f "$1" ];then
		insmod $@
	fi
}

__rmmod_camera_sensor()
{
	for item in `echo "sc3336"`
	do
		if lsmod | grep $item | awk '{print $3}' |grep -w 0;then
			rmmod $item
		fi
	done
}

__insmod rk_dvbm.ko

__insmod videobuf2-memops.ko
__insmod videobuf2-common.ko
__insmod videobuf2-v4l2.ko
__insmod videobuf2-vmalloc.ko
__insmod videobuf2-cma-sg.ko

# Add the real sensor driver here (e.g. imx327.ko) alongside sc3336.ko when the PCB build lands.
#
# hts_mult scales the sensor's line period, and with it BOTH the maximum
# exposure and the minimum frame period. The exposure ceiling is
# VTS_MAX (0xffff rows) x row_time, so this is the only lever left once
# SC3336_VTS_MAX is already at the 16-bit register maximum.
#
# Measured on real hardware (2026-09-17):
#
#   hts_mult | row time | ExposureMax | shortest capture (10ms request)
#   ---------+----------+-------------+--------------------------------
#          1 |  27.5 us |       1.80s |  ~0.2s
#         12 |   329 us |      21.6s  |  ~1.0s
#         52 |  1427 us |      93.5s  |  ~4.2s     (52 is the hard maximum:
#                                                   HTS is 16-bit, base 1250)
#
# The right-hand column is the cost: row time scales everything, so a long
# ceiling makes short exposures slow too (focusing and framing suffer, not
# guiding, where the exposure itself dominates). 12 is the shipped default as
# a deliberate balance -- 21.6s is already far past anything guiding needs,
# while ~1s to return a short frame stays usable. Set it to 1 to restore stock
# behaviour, or 52 if a single very long sub-exposure matters more than
# interactive responsiveness. Takes effect at the next boot; no rebuild needed.
__insmod sc3336.ko hts_mult=12

__insmod video_rkcif.ko
__insmod phy-rockchip-csi2-dphy-hw.ko
__insmod phy-rockchip-csi2-dphy.ko

__rmmod_camera_sensor

echo 1 > /sys/module/video_rkcif/parameters/clr_unready_dev

udevadm control --start-exec-queue

# insmod wifi driver background
$(pwd)/insmod_wifi.sh &
