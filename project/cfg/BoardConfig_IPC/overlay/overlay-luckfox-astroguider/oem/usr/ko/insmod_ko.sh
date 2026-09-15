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

# Add the real sensor driver here (e.g. imx290.ko) alongside sc3336.ko when the PCB build lands.
__insmod sc3336.ko

__insmod video_rkcif.ko
__insmod phy-rockchip-csi2-dphy-hw.ko
__insmod phy-rockchip-csi2-dphy.ko

__rmmod_camera_sensor

echo 1 > /sys/module/video_rkcif/parameters/clr_unready_dev

udevadm control --start-exec-queue

# insmod wifi driver background
$(pwd)/insmod_wifi.sh &
