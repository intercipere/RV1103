#!/bin/bash

#################################################
# 	Board Config
#################################################
export LF_ORIGIN_BOARD_CONFIG=BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico-IPC.mk
# Target CHIP
export RK_CHIP=rv1106

# app config
export RK_APP_TYPE="RKIPC_RV1103"

# Config CMA size in environment
export RK_BOOTARGS_CMA_SIZE="24M"

# Kernel dts
export RK_KERNEL_DTS=rv1103g-luckfox-pico.dts

#################################################
#	BOOT_MEDIUM
#################################################

# Target boot medium
export RK_BOOT_MEDIUM=sd_card

# Uboot defconfig fragment
export RK_UBOOT_DEFCONFIG_FRAGMENT=rk-emmc.config

# specify post.sh for delete/overlay files
# export RK_PRE_BUILD_OEM_SCRIPT=rv1103-spi_nor-post.sh

# config partition in environment
# RK_PARTITION_CMD_IN_ENV format:
#     <partdef>[,<partdef>]
#       <partdef> := <size>[@<offset>](part-name)
# Note:
#   If the first partition offset is not 0x0, it must be added. Otherwise, it needn't adding.
# OpenAstroGuider: shrunk boot (was 32M; boot.img is only ~3.7M) and userdata
# (was 256M; /tmp is the RAM-backed scratch space, userdata only needs to hold
# small config/calibration files) to fit a 128MB SD card, simulating the SPI
# NAND target's storage budget. rootfs ("-") takes whatever remains.
export RK_PARTITION_CMD_IN_ENV="32K(env),512K@32K(idblock),512K(uboot),8M(boot),16M(userdata),-(rootfs)"

# config partition's filesystem type (squashfs is readonly)
# emmc:    squashfs/ext4
# nand:    squashfs/ubifs
# spi nor: squashfs/jffs2
# RK_PARTITION_FS_TYPE_CFG format:
#     AAAA:/BBBB/CCCC@ext4
#         AAAA ----------> partition name
#         /BBBB/CCCC ----> partition mount point
#         ext4 ----------> partition filesystem type
export RK_PARTITION_FS_TYPE_CFG=rootfs@IGNORE@ext4,userdata@/userdata@ext4

# config filesystem compress (Just for squashfs or ubifs)
# squashfs: lz4/lzo/lzma/xz/gzip, default xz
# ubifs:    lzo/zlib, default lzo
# export RK_SQUASHFS_COMP=xz
# export RK_UBIFS_COMP=lzo

#################################################
#	TARGET_ROOTFS
#################################################

# Target rootfs
export LF_TARGET_ROOTFS=buildroot

# Buildroot defconfig
export RK_BUILDROOT_DEFCONFIG=luckfox_pico_defconfig

#################################################
# 	Defconfig
#################################################

# Target arch
export RK_ARCH=arm

# Target Toolchain Cross Compile
export RK_TOOLCHAIN_CROSS=arm-rockchip830-linux-uclibcgnueabihf

#misc image
export RK_MISC=wipe_all-misc.img

# Uboot defconfig
export RK_UBOOT_DEFCONFIG=luckfox_rv1106_uboot_defconfig

# Kernel defconfig
export RK_KERNEL_DEFCONFIG=luckfox_rv1106_linux_defconfig

# Config sensor IQ files
# RK_CAMERA_SENSOR_IQFILES format:
#     "iqfile1 iqfile2 iqfile3 ..."
# ./build.sh media and copy <SDK root dir>/output/out/media_out/isp_iqfiles/$RK_CAMERA_SENSOR_IQFILES
export RK_CAMERA_SENSOR_IQFILES="sc4336_OT01_40IRC_F16.json sc3336_CMK-OT2119-PC1_30IRC-F16.json"
#export RK_CAMERA_SENSOR_IQFILES="sc4336_OT01_40IRC_F16.json sc3336_CMK-OT2119-PC1_30IRC-F16.json sc530ai_CMK-OT2115-PC1_30IRC-F16.json"

# Config sensor lens CAC calibrattion bin files
export RK_CAMERA_SENSOR_CAC_BIN="CAC_sc4336_OT01_40IRC_F16"
#export RK_CAMERA_SENSOR_CAC_BIN="CAC_sc4336_OT01_40IRC_F16 CAC_sc530ai_CMK-OT2115-PC1_30IRC-F16"

# build ipc web backend
# export RK_APP_IPCWEB_BACKEND=y

# enable install app to oem partition
# export RK_BUILD_APP_TO_OEM_PARTITION=y

# enable rockchip test
# export RK_ENABLE_ROCKCHIP_TEST=y

#################################################
# 	PRE and POST
#################################################

# specify pre.sh for delete/overlay files
# OpenAstroGuider: strips the ISP/media/NPU/audio stack (raw V4L2 capture only);
# folds in the deletions from the stock luckfox-buildroot-oem-pre.sh too, since
# only one script can be named here.
export RK_PRE_BUILD_OEM_SCRIPT=luckfox-astroguider-oem-pre.sh

# specify post.sh for delete/overlay files
export RK_PRE_BUILD_USERDATA_SCRIPT=luckfox-userdata-pre.sh

# declare overlay directory
# overlay-luckfox-astroguider: trims RkLunch.sh/insmod_ko.sh to raw V4L2 capture only
# (no rkipc, no ISP/RGA/MPP/NPU/audio/motor kernel modules) and disables
# iptables/telnet/sshd/micinit at boot.
export RK_POST_OVERLAY="overlay-luckfox-config overlay-luckfox-buildroot-init overlay-luckfox-buildroot-shadow overlay-luckfox-astroguider"

#################################################
# 	OpenAstroGuider: restore trimmed buildroot defconfig
#################################################
# sysdrv/source/buildroot/ is entirely gitignored (sysdrv/.gitignore), so the
# debloated luckfox_pico_defconfig living there does not survive a fresh
# clone or a from-scratch vendor-source re-extraction. The real, tracked copy
# lives alongside this board config; this board config is sourced by
# project/build.sh (see `[ -L "$BOARD_CONFIG" ] && source $BOARD_CONFIG`)
# before the buildroot defconfig is symlinked into place
# (__LINK_DEFCONFIG_FROM_BOARD_CFG), so copying it here self-heals every
# build. Edit the tracked copy, not the vendor-tree one, to persist changes.
_LF_BOARDCFG_DIR="$(dirname "$(realpath "${BASH_SOURCE[0]}")")"
_LF_TRACKED_DEFCONFIG="$_LF_BOARDCFG_DIR/luckfox_pico_defconfig"
_LF_VENDOR_DEFCONFIG="$_LF_BOARDCFG_DIR/../../../sysdrv/source/buildroot/buildroot-2023.02.6/configs/luckfox_pico_defconfig"
if [ -f "$_LF_TRACKED_DEFCONFIG" ]; then
	mkdir -p "$(dirname "$_LF_VENDOR_DEFCONFIG")"
	cp -f "$_LF_TRACKED_DEFCONFIG" "$_LF_VENDOR_DEFCONFIG"
fi
unset _LF_TRACKED_DEFCONFIG _LF_VENDOR_DEFCONFIG