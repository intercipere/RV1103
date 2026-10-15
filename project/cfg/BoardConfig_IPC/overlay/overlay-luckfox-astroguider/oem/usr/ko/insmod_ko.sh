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
# Row time is (true HTS 2500) x hts_mult / 102 MHz pixel rate = 24.51 us x
# hts_mult, measured with v4l2-ctl --stream-count (2026-09-17). An older table
# here used 27.5 us from the vendor's wrong hts_def; these are corrected.
#
#   hts_mult | row time | ExposureMax | shortest frame (1296+64 rows)
#   ---------+----------+-------------+------------------------------
#          1 |  24.5 us |      1.61s  |   33 ms
#          3 |  73.5 us |      4.82s  |  100 ms
#          4 |  98.0 us |      6.42s  |  133 ms   <- shipped
#         12 |   294 us |     19.27s  |  400 ms
#         52 |  1275 us |     83.5s   |  1.73 s   (52 is the hard maximum:
#                                                  HTS is 16-bit, base 1250)
#
# The right-hand column is the cost: row time scales everything, so a long
# ceiling makes every frame slow, short exposures included -- the guide loop
# and the /guide preview cannot run faster than one frame per shortest frame.
# 4 is the shipped value (2026-10-07): guiding realistically needs at most
# ~5s, and 4 is the smallest multiplier that clears it, at 7.5 frames/s. It
# was 12 (19.27s ceiling, 2.5 frames/s) before. This also caps Alpaca clients'
# ExposureMax. Takes effect at the next boot; no rebuild needed.
__insmod sc3336.ko hts_mult=4

__insmod video_rkcif.ko
__insmod phy-rockchip-csi2-dphy-hw.ko
__insmod phy-rockchip-csi2-dphy.ko

__rmmod_camera_sensor

echo 1 > /sys/module/video_rkcif/parameters/clr_unready_dev

udevadm control --start-exec-queue

# insmod wifi driver background
$(pwd)/insmod_wifi.sh &
