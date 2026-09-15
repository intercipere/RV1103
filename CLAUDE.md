# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

This is the Luckfox Pico SDK (based on Rockchip's RV1103/RV1106 vendor SDK): a full embedded Linux
build system covering U-Boot, kernel, Buildroot/Busybox rootfs, Rockchip media libraries (ISP/MPP/RGA/
rockit/IVA/IVE), and IPC (camera) reference applications, all cross-compiled for ARM targets. It is not
a typical application repo — most work here is board/kernel/driver configuration and cross-compilation,
not host-side application development.

Currently configured board (`.BoardConfig.mk` symlink): `RV1103_Luckfox_Pico`, SD_CARD boot medium,
Buildroot rootfs, app type `RKIPC_RV1103`. This can change if `./build.sh lunch` is re-run.

## Build commands

All builds go through `./build.sh` (symlinked to `project/build.sh`) from the repo root. A board must be
selected first with `./build.sh lunch` (writes `.BoardConfig.mk`), which picks: hardware variant → boot
medium (SD_CARD/SPI_NAND/EMMC) → rootfs type (Buildroot/Busybox). The active selection is discoverable
via `./build.sh info`.

Before building, the cross toolchain must be sourced once per shell:
```
cd tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/
source env_install_toolchain.sh
```

Common targets:
```
./build.sh check           # sanity-check the build environment/toolchain
./build.sh                 # full one-click build for the currently lunched board
./build.sh uboot           # build U-Boot only         -> output/image/{MiniLoaderAll.bin,uboot.img}
./build.sh kernel          # build kernel only          -> output/image/boot.img
./build.sh rootfs          # build rootfs only
./build.sh media           # build Rockchip media libs  -> output/out/media_out
./build.sh app             # build reference apps (depends on media)
./build.sh sysdrv          # uboot + kernel + rootfs
./build.sh all             # uboot + kernel + rootfs + recovery image
./build.sh firmware        # repackage everything already built -> output/image
./build.sh recovery        # build recovery image
./build.sh factory         # build factory image
./build.sh updateimg       # build update image
./build.sh ota             # pack update_ota.tar
```

Clean equivalents exist per component: `./build.sh clean [uboot|kernel|driver|rootfs|sysdrv|media|app|recovery]`,
or `./build.sh clean` for everything.

Config (menuconfig-style) entry points:
```
./build.sh kernelconfig     # kernel menuconfig, saves defconfig
./build.sh buildrootconfig  # buildroot menuconfig, saves defconfig (buildroot rootfs only)
```

After changing `media` or `app`, re-run `./build.sh firmware` to repackage the image — those targets do
not auto-repackage.

Symlinks worth knowing about at the repo root:
- `.BoardConfig.mk` → the active board config under `project/cfg/BoardConfig_IPC/`
- `build.sh` → `project/build.sh`
- `rkflash.sh` → `project/rkflash.sh` (flashing the target board over USB)
- `config/buildroot_defconfig`, `config/kernel_defconfig`, `config/dts_config` → the currently active
  defconfigs/device tree inside `sysdrv/source/`, resolved via `.BoardConfig.mk`

There is no unit test suite in the traditional sense; "testing" a change means building the affected
component and (per the README's Windows-copy warning) flashing/booting real hardware, or checking
target-side behavior via `rkipc`/media samples.

## Architecture / directory layout

- `project/` — build orchestration. `project/build.sh` is the real entry point; `project/cfg/BoardConfig_IPC/`
  holds one `.mk` file per (hardware variant × boot medium × rootfs) combination plus pre/post OEM shell
  hooks (`*-pre.sh`, `*-post.sh`) that patch config or overlay files during the build. `project/app/`
  contains the reference/IPC applications (`rkipc`, `rk_smart_door`, `uvc_app_tiny`, `wifi_app`,
  `ipcweb`, `fastboot_client`, `component`) each with their own Makefile/CMake build.
- `sysdrv/` — the low-level system stack: `sysdrv/source/{uboot,kernel,buildroot,mcu}` are the actual
  U-Boot, Linux kernel, Buildroot, and MCU firmware source trees; `sysdrv/Makefile` drives their builds;
  `sysdrv/drv_ko/` and `sysdrv/out/` hold built kernel modules/output.
  Kernel defconfig lives at `sysdrv/source/kernel/arch/arm/configs/`, DTS at
  `sysdrv/source/kernel/arch/arm/boot/dts/`, buildroot defconfig at
  `sysdrv/source/buildroot/buildroot-2023.02.6/configs/`.
- `media/` — Rockchip media/vision stack, each a semi-independent library with its own Makefile:
  `isp`, `mpp` (media process platform / codec), `rga` (2D graphics accel), `rockit`, `iva`/`ive`
  (vision algorithms), `libdrm`, `libv4l`, `alsa-lib`, `sysutils`, `security`, `common_algorithm`,
  `avs`, `rkpostisp`, `luckfox` (Luckfox-specific glue), `samples`. `media/Makefile.param` and
  per-subdir Makefiles wire these into `./build.sh media`.
- `output/` and `IMAGE/` — build artifacts. `output/image/` holds the individual boot/rootfs/uboot
  images; `output/out/` holds intermediate per-component output; `IMAGE/<board>_<timestamp>_<tag>/`
  holds final packaged firmware releases produced by `./build.sh firmware`.
- `tools/` — host-side tooling: `tools/linux/toolchain/` has the cross-compilation toolchain(s) (source
  `env_install_toolchain.sh` before building); `tools/linux` and `tools/windows` hold flashing/packing
  utilities used by `rkflash.sh` and the firmware packer.
- `config/` — symlinks into the currently active kernel defconfig, buildroot defconfig, and DTS (see
  above), for quick inspection without walking into `sysdrv/source/`.

## Key conventions

- Board/app/feature selection is entirely driven by shell-exported `RK_*` / `LF_*` variables set in the
  chosen `BoardConfig-*.mk` file (e.g. `RK_CHIP`, `RK_APP_TYPE`, `RK_BOOT_MEDIUM`, `RK_KERNEL_DTS`,
  `RK_PARTITION_CMD_IN_ENV`, `LF_TARGET_ROOTFS`, `LF_WIFI_SSID`/`LF_WIFI_PSK`). To change board behavior,
  edit or select the matching `.mk` file rather than passing flags to `build.sh` directly.
- Partition layout and filesystem types per partition are configured declaratively in the BoardConfig
  file via `RK_PARTITION_CMD_IN_ENV` and `RK_PARTITION_FS_TYPE_CFG` — not in a separate partition table
  file.
- WiFi default credentials for Buildroot images are set via `LF_WIFI_SSID`/`LF_WIFI_PSK` in the
  board config file, not in the rootfs overlay.
- Do not copy/extract this source tree on Windows before building on Linux — the README notes this can
  silently strip executable bits or break symlinks used by the build.

## OpenAstroGuider project context

This clone (`https://github.com/intercipere/RV1103`, forked from `LuckfoxTECH/luckfox-pico`) is being
adapted into a custom astronomy guide camera: a mono IMX290 sensor on a custom PCB around a Rockchip
RV1106G3 SoC (256MB RAM), replacing an off-the-shelf colour IMX290 USB module capped at 0.5s exposures.
Goal is long exposures without relying on PHD2 live stacking. Current prototyping platform is the Luckfox
Pico (RV1103, 64MB RAM — proved too tight for buffering multiple full-res frames) with a Waveshare SC3336
camera module standing in for the IMX290 until the PCB exists.

Architectural decisions:
- **Raw-only capture, no ISP.** Bypass rkaiq/rockit entirely — capture straight off the rkcif raw node
  (`/dev/video0`), sensor controls via the subdev (`/dev/v4l-subdev2`). No on-device image processing;
  raw 10-bit Bayer frames are shipped to the PC for processing.
- **Transport: HTTP/Alpaca over a USB gadget, using RNDIS (not ECM or UVC)** — RNDIS needs no driver
  install on Windows 10/11, unlike ECM.
- **Output format: FITS**, for astronomy capture/stacking software compatibility.
- Exposure/blanking are measured in **rows, not seconds** (~27.45µs/row on this sensor/clock config).
  Exposure is capped by frame length (1296 rows + vertical blanking) — `vertical_blanking` must be raised
  before a longer exposure will actually apply.

Validated sensor/driver facts (empirical, not from docs — trust these over datasheet assumptions):
- RV1103 compact RAW10 packing (`bits_lsb`) is **little-endian bit order within each byte AND
  little-endian bit assembly within each pixel's 10 bits** (`np.unpackbits(..., bitorder="little")`,
  ascending powers of two). Big-endian assumptions produce pixel-scrambled noise that looks identical
  across all four Bayer phases — that's the signature of this specific bug, not a phase-offset bug.
- `/tmp` on the board is a RAM disk on a 64MB device — multi-frame captures must be pulled off-device in
  batches, never buffered fully on-device.
- SC3336 shares IMX290's architecture: exposure capped by frame length, driver likely enforces a
  `vblank` ceiling below what the silicon allows. Pushing that ceiling on the SC3336 is the concrete
  go/no-go signal for whether this platform tolerates the exposures the real IMX290 build needs.
- Colour rendering: `cv2.cvtColor(..., cv2.COLOR_BayerRG2RGB_EA)` + 16-bit PNG output, implemented in
  `grab.py`.

**Debloat (done, verified 2026-09-15, SD_CARD/Buildroot config).** Stock `RKIPC_RV1103` image was
199MB rootfs.img / ~103MB unpacked content, booting the full ISP/media/NPU/audio stack and launching
`rkipc` at boot for a raw-capture use case that needs none of it. Rebuilt to **43.3MB rootfs.img / 22MB
unpacked** (rootfs content 72M→~19M, oem 19M→3.5M) with no ISP/RGA/MPP/NPU/audio/rkipc process running
and only `video_rkcif.ko` + `phy-rockchip-csi2-dphy*.ko` + `sc3336.ko` + `videobuf2-*` inserted at boot.
Mechanism (all additive, no shared vendor Makefiles touched — see the full plan/rationale at
`/home/fabian/.claude/plans/keen-yawning-phoenix.md`):
- `project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-astroguider/` — trimmed `RkLunch.sh` (drops the
  rkipc.ini/iqfiles bootstrap and the `rkipc &` launch) and `insmod_ko.sh` (only inserts the modules
  above, plus backgrounded `insmod_wifi.sh`), and no-op stubs for `S35iptables`/`S50telnet`/`S50sshd`/
  `S60micinit`. Wired in via `RK_POST_OVERLAY` in the board config (last in the list, so it wins over
  the stock `overlay-luckfox-buildroot-init` copies of `S50sshd`/`S60micinit`).
- `project/cfg/BoardConfig_IPC/luckfox-astroguider-oem-pre.sh` — `RK_PRE_BUILD_OEM_SCRIPT`, deletes the
  ISP/RGA/MPP/NPU/audio/motor/rockit/rve kernel modules, unused sensor drivers, rkipc + all
  `sample_*`/`simple_*`/`rk_mpi_*` demo binaries, and rkipc's ISP-tuning/UI assets from the OEM output
  before packaging. This replaced (and folds in the deletions from) the stock
  `luckfox-buildroot-oem-pre.sh`, since that board-config slot only holds one script name.
- `sysdrv/source/buildroot/buildroot-2023.02.6/configs/luckfox_pico_defconfig` — dropped Python3 (+ all
  `PYTHON_*` packages), OpenSSH/GnuTLS/libcurl/wget, iptables/nftables, ntp, and a long tail of unused
  CLI tools (htop/nano-adjacent/p7zip/iperf/socat/rsync/lrzsz/bmon/dialog/dtc/libdrm/freetype/evtest).
  Kept `libv4l`/`libv4l-utils`, `dhcpcd` (Ethernet), `bash`, `e2fsprogs` (load-bearing for first-boot
  rootfs resize in `S20linkmount`), `nano`.
  **Caveat:** `sysdrv/source/buildroot/` is gitignored (whole vendor source trees are) — this edit lives
  on disk but won't survive a fresh clone or a from-scratch source re-extraction; if that ever happens,
  reapply from the plan file above.
- Not touched (deliberately, to keep this low-risk): kernel Kconfig (ISP/RGA/MPP/NPU/audio drivers still
  *compile*, they're just never inserted — a further win is possible here but touches the kernel
  dependency graph) and `RK_APP_TYPE` (kept as `RKIPC_RV1103` since it's what wires up the SC3336/rkcif
  driver combination).
- Remote access: SSH/telnet disabled: **adb is the shell/debug channel**; usb0 RNDIS (already
  `dr_mode="peripheral"` in the DTS, already `RNDIS_EN=on`/`ADB_EN=on` in the stock `S50usbdevice`) is
  reserved for the future Alpaca HTTP server. WiFi kernel modules/firmware were kept (per project
  decision) even though unused today.
- Not yet measured: actual boot-time-to-shell improvement and behavior on real hardware — the above is
  build-verified (clean build, correct file deletions/overlays confirmed in the build log) but not yet
  flashed/booted. Do that next, then re-run `grab.py` end-to-end to confirm the capture path.

USB gadget/networking: **already wired, not "in progress" as previously noted here** — the DTS OTG node
already has `dr_mode = "peripheral"` and the stock `S50usbdevice` configfs script already has
`RNDIS_EN=on` and `ADB_EN=on` (usb0 comes up at a static IP via `run_binary()` in that script). What's
still open: an on-device HTTP/Alpaca server to actually serve capture over that link — nothing built yet
beyond the transport being ready. First milestone is `curl`/browser triggering a capture over usb0,
replacing the adb-only workflow.

PC-side tooling already written: `grab.py` (working capture/unpack/preview — sets sensor controls,
captures N frames, pulls raw over adb/ssh, unpacks RAW10, writes PNG) plus diagnostic scripts from
earlier exploration: `isp_grab.py`, `noise.py`, `bayer_phase.py`, `planes.py`, `period.py`, `bitorder.py`,
`to_fits.py`.

Working style for this project: prefer empirical validation over guessing (measure before cutting, use
dark frames as ground truth); push back on unverified fixes offered as definitive — get the actual
measurement or check.

Open next steps: (1) flash the debloated image and verify boot time + `grab.py` capture path on real
hardware (build-verified only so far); (2) push the SC3336 exposure ceiling as far as it goes — the
go/no-go signal for long exposures; (3) minimal on-device HTTP/Alpaca server, first milestone `curl`
working from a PC over the already-working usb0 RNDIS link; (4) optional follow-up: disable
ISP/RGA/MPP/NPU/audio in kernel Kconfig too (they currently still compile, just aren't loaded) for a
further flash-size cut; (5) separately, a custom SPI NAND board config for the 64–128MB deployment
target (see the two candidate `.mk` files noted in earlier project notes) — not started, independent of
the SD_CARD debloat above; (6) longer term, retarget the whole stack to RV1106G3 (256MB RAM) once
validated on RV1103.
