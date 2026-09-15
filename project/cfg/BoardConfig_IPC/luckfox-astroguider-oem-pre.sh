#!/bin/bash
#
# OpenAstroGuider: strip the ISP/media/NPU/audio stack from the oem partition.
# Raw V4L2 capture only needs rkcif + the CSI2 PHY + the sensor driver in use;
# everything below is dead weight from the stock RKIPC_RV1103 app bundle.

function write_version() {
    # OpenAstroGuider: stamp the exact build that produced this image into
    # the rootfs itself, so "which build is actually on this SD card" is
    # answerable by asking the device (adb, or the Alpaca API's
    # driverversion field) instead of trusting which dated IMAGE/*_RELEASE_TEST
    # folder you happened to flash from.
    if [ -n "$RK_PROJECT_PACKAGE_ROOTFS_DIR" ]; then
        local git_rev
        git_rev=$(git -C "$SDK_ROOT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)
        if [ -n "$(git -C "$SDK_ROOT_DIR" status --porcelain 2>/dev/null)" ]; then
            git_rev="${git_rev}-dirty"
        fi
        mkdir -p "$RK_PROJECT_PACKAGE_ROOTFS_DIR/etc"
        echo "built=$(date -u +%Y-%m-%dT%H:%M:%SZ) git=$git_rev" \
            >"$RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/openastroguider-version"
        echo "Wrote version file: $(cat "$RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/openastroguider-version")"
    else
        echo "write_version: RK_PROJECT_PACKAGE_ROOTFS_DIR not set, skipping"
    fi
}

function lf_rm() {
    for file in "$@"; do
        if [ -e "$file" ]; then
            echo "Deleting: $file"
            rm -rf "$file"
        fi
    done
}

function remove_data()
{
    # unused kernel modules: ISP, RGA, video codec, NPU, audio, motor, rockit/rve
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/video_rkisp.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/rga3.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/mpp_vcodec.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/rknpu.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/snd-soc-rv1106.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/motor.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/rockit.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/rve.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/hpmcu_wrap.bin

    # unused camera sensor modules (only sc3336 is in use on this platform)
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/imx415.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/os04a10.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/sc4336.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/sc530ai.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/gc2053.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/sc200ai.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/sc401ai.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/sc450ai.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/techpoint.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/mis5001.ko
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/ko/mia1321.ko

    # unused media/ISP/NPU/audio userspace libraries
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librkaiq.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librockit.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librockit_full.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librockit_tiny.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librockiva.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libivs.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librkpostisp.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libsmartIr.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libaec_bf_process.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librkdemuxer.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librkmuxer.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librkaudio.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librkaudio_common.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librkaudio_detect.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librknnmrt.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librve.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librga.so
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librockchip_mpp.so*

    # rkipc binary and every unused sample/test/demo binary that shipped with it
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/rkipc
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/sample_*
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/simple_*
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/rk_mpi_*
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/mpp_info_test
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/mpi_enc_test
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/vpu_api_test
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/rgaImDemo
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/dumpsys
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/rkaiq_3A_server
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/rkaiq_tool_server
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/j2s4b_dev
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/modetest

    # rkipc UI/ISP-tuning assets
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/share/isp_iqfiles
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/share/avs_calib
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/share/vqefiles
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/share/rkipc-*.ini
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/share/image.bmp
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/share/simsun_en.ttf
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/share/speaker_test.wav

    # carried over from the stock luckfox-buildroot-oem-pre.sh (this board's
    # RK_PRE_BUILD_OEM_SCRIPT slot can only point at one script, so these are
    # folded in here rather than dropped)
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/*.aiisp
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/*.data
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libdrm*
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libdrm_rockchip*
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libkms*
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libfreetype*
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libiconv*
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librkAVS*
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libjpeg*
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libpng*
}

#=========================
# run
#=========================
remove_data
write_version
