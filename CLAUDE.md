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
Pico (RV1103, **64MB physical RAM, ~32MB usable** — confirmed via `dmesg`:
`Memory: 32368K/65536K available ... 24576K cma-reserved`; the chip has the full 64MB, but 24MB is
reserved for the Contiguous Memory Allocator (`RK_BOOTARGS_CMA_SIZE="24M"`, almost certainly sized for
the stock ISP/RGA/MPP/rockit pipeline this project already stripped out), leaving `MemTotal`/`malloc()`
with ~32MB — confirmed the hard way via a real OOM crash during Alpaca driver testing, see below — proved
too tight for buffering multiple full-res frames) with a Waveshare SC3336
camera module standing in for the IMX290 until the PCB exists. **The Luckfox Pico board and the SC3336
are both temporary prototyping stand-ins, not the production target** — production is the custom PCB
(RV1106G3 + IMX290), not Luckfox hardware. The SC3336's known exposure ceiling is an accepted limitation
for prototyping, not a bug to chase. Long term (low priority — not blocking current work), Luckfox-specific
references/mechanisms should be phased out as the project moves onto the custom PCB; don't over-invest in
Luckfox-board-specific tooling beyond what prototyping needs right now.

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
- `/tmp` on the board is a RAM disk on a ~32MB device (see corrected figure above) — multi-frame captures must be pulled off-device in
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
  **Persistence fixed (2026-09-15):** `sysdrv/source/buildroot/` is gitignored (whole vendor source
  trees are), so this file didn't survive a fresh clone on its own. A tracked copy now lives at
  `project/cfg/BoardConfig_IPC/luckfox_pico_defconfig`, and the board config (`BoardConfig-SD_CARD-...
  -IPC.mk`) copies it into the gitignored vendor path itself every time `./build.sh` sources it (before
  `__LINK_DEFCONFIG_FROM_BOARD_CFG` consumes it) — self-healing, verified by deleting the vendor copy and
  confirming `./build.sh check` restored it. **Edit the tracked copy, not the vendor-tree one.**
- Not touched (deliberately, to keep this low-risk): kernel Kconfig (ISP/RGA/MPP/NPU/audio drivers still
  *compile*, they're just never inserted — a further win is possible here but touches the kernel
  dependency graph) and `RK_APP_TYPE` (kept as `RKIPC_RV1103` since it's what wires up the SC3336/rkcif
  driver combination).
- Remote access: SSH/telnet disabled: **adb is the shell/debug channel**; usb0 RNDIS (already
  `dr_mode="peripheral"` in the DTS, already `RNDIS_EN=on`/`ADB_EN=on` in the stock `S50usbdevice`) is
  reserved for the future Alpaca HTTP server. WiFi kernel modules/firmware were kept (per project
  decision) even though unused today.
- **Hardware-verified (2026-09-15, via adb on the actual board):** `/dev/video0` and `/dev/v4l-subdev2`
  present; `lsmod` shows exactly `phy_rockchip_csi2_dphy`, `phy_rockchip_csi2_dphy_hw`, `video_rkcif`,
  `sc3336`, `rk_dvbm` — nothing else; no `rkipc`/`rockit`/`rkisp` process running; usb0 RNDIS up at
  172.32.0.70; adb reachable. Matches the plan's intent exactly.

**Partition layout for a 128MB target (done, verified 2026-09-15).** Stock `RK_PARTITION_CMD_IN_ENV` gave
`userdata` 256M and `boot` 32M — the `userdata` allocation alone was 2x a 128MB SD card's total capacity,
so the old table could never fit regardless of rootfs size. Shrunk to `boot=8M` (actual `boot.img` is only
~3.7M) and `userdata=16M` (config/calibration only — `/tmp` is the RAM-backed scratch space, not
userdata). Total flashable image (`sd_update.img`) is now **~69MB**, verified via a real
`./build.sh firmware` run, comfortably inside even a conservative ~122MiB-usable 128MB card.

**Boot speed: measured, and one hardware constraint discovered (2026-09-15).**
- Real measured time, via a live `adb shell reboot` + polling for adb to come back: **~12.4s**, split into
  ~7.2s before Linux's own `/proc/uptime` clock starts (BootROM + SPL/TPL + U-Boot + kernel decompression)
  and ~5.2s of kernel + early userspace (rcS through `S50usbdevice` + USB gadget enumeration handshake).
  More than half the time is in the pre-kernel phase, which none of the init-script trimming touches.
- Fixed: `etc/init.d/S99usb0config` (now a no-op stub in `overlay-luckfox-astroguider`) was a leftover
  Rockchip QA-harness script that force-assigned usb0 a *different* static IP (`.93`) than
  `S50usbdevice` already sets (`.70`), guaranteeing a retry loop every boot.
- Added `quiet` to `CONFIG_CMDLINE` in `sysdrv/source/kernel/arch/arm/configs/luckfox_rv1106_linux_defconfig`
  (`CONFIG_CMDLINE_EXTEND=y`, so it appends rather than replaces) to cut serial-console printk overhead
  during the kernel phase. **Build-verified only** — kernel rebuilds and the string is confirmed baked
  into `vmlinux`/`zImage`, but not yet measured on hardware (see flashing constraint below).
- Rockchip's "thunderboot" fast-boot infrastructure exists in this SDK's DTS tree
  (`rv1106-thunder-boot-emmc.dtsi`, `rv1126-thunder-boot-spi-nor.dtsi`) and would likely be the biggest
  remaining lever on the pre-kernel phase, but it's wired for eMMC/SPI-NOR only, not this SD_CARD config —
  real candidate for the eventual RV1106G3/SPI-NAND retarget, not attempted here (kernel/DTS/U-Boot-level
  work, same risk class as the Kconfig changes deferred above).
- **Hardware constraint discovered:** this Luckfox Pico variant has **no onboard eMMC/NAND** — confirmed
  via Rockchip's `upgrade_tool`: it detects an eMMC controller but `Flash Size: 0MB` (unpopulated). It's
  SD-card-only. `upgrade_tool`'s rockusb protocol (reachable via `reboot(RESTART2, "loader")`, which maps
  to `BOOT_BL_DOWNLOAD` via the `syscon-reboot-mode` DT node — confirmed working, gets the board into
  Maskrom over USB with no physical button) only targets *internal* flash controllers, so **USB flashing
  is not possible on this board**. Every image update requires physically pulling the SD card and
  writing `output/image/sd_update.img` with `dd`/balenaEtcher from a PC.

USB gadget/networking: **already wired, not "in progress" as previously noted here** — the DTS OTG node
already has `dr_mode = "peripheral"` and the stock `S50usbdevice` configfs script already has
`RNDIS_EN=on` and `ADB_EN=on` (usb0 comes up at a static IP via `run_binary()` in that script).

**Alpaca driver: working vertical slice, hardware-verified (2026-09-15).** Lives in `alpaca/` at the repo
root — full design, module layout, and verification detail in `alpaca/README.md`; summary here. Runs
*on the RV itself* (that's the point of Alpaca vs. classic ASCOM/COM), in C (no re-adding Python), using
vendored CivetWeb (MIT, trimmed to ~760KB source) for HTTP. Discovery (UDP :32227), the management API,
the common device API, and a full camera exposure cycle (`startexposure`→`imageready`→`imagearray`) all
tested end-to-end against the real SC3336 via `adb forward`+`curl`: correct 1296×2304 output, valid
10-bit pixel range, `ExposureMax`/`ExposureMin` computed live from V4L2 control ranges matching the
project's independently-validated 27.45µs/row exactly (not hardcoded, so it tracks the exposure-ceiling
investigation below automatically). A `sensor_desc_t` abstraction (`alpaca/src/sensor.h`) supports both
color (Bayer) and monochrome sensors by design, with SC3336 fully populated and an IMX290 entry stubbed
in as a TODO placeholder — adding a sensor is a new table entry, not new dispatch logic.
`alpacad` is baked into the image itself (stripped binary + `S60alpacad` init script, both via the same
`overlay-luckfox-astroguider` mechanism as the rest of the debloat work) — **reboot survival confirmed on
real hardware** (physically reflashed, power-cycled, `ps` showed `alpacad` already running), not just
build-verified.

**Networking fixed: IPv4 link-local, not a fixed IP (2026-09-15).** The stock `S50usbdevice` hardcodes
usb0 to `172.32.0.70/16`, which meant every connecting PC needed someone to manually assign a matching
static IP — a real production problem, discovered the hard way (host-side interface names change every
reconnect since the gadget's host MAC is randomized each boot). Fixed with a new `S51usb0-linklocal`
script that just clears the static address and lets `dhcpcd` (already running) take over — it already
correctly does DHCP-then-IPv4LL/RFC3927 fallback (confirmed via its own log), it was just racing with and
losing to the static assignment. No new networking code needed. Verified stable on real hardware (settled
on the same address twice, 5s apart). Windows/macOS/Linux all self-assign a compatible `169.254.0.0/16`
address automatically for this, so production users need zero network configuration.

**Build versioning added (2026-09-15).** `/etc/openastroguider-version` (build timestamp + git hash,
`-dirty` if uncommitted) is now stamped into every build and exposed via the Alpaca `driverversion`
endpoint — added after a real mixup where `./build.sh` (bare) archives a *new* dated snapshot into
`IMAGE/*_RELEASE_TEST/` on every run without cleaning up old ones, making it easy to flash a stale one by
habit. `./build.sh` itself was confirmed correct (byte-identical output to a manual step-by-step build);
the dated-folder accumulation was the actual trap. Flash from `output/image/sd_update.img` directly to
avoid it.

**No imaging pipeline beyond raw capture, by design.** `alpacad` does raw capture → RAW10 unpack → serve
via `imagearray` — no on-device dark/flat calibration, debayering, or stacking. Matches both this
project's original architecture (raw frames shipped to the PC) and Alpaca's own design (camera driver
hands back raw data, client does processing).

**This board has no onboard eMMC/NAND and can't be flashed over USB** (confirmed via `upgrade_tool`) — use
Rockchip SocToolKit's **SD Card** tab (writes a raw disk image to a physically-inserted card, same as
`dd`), not the Download/Firmware tabs.

**Real Windows/N.I.N.A. testing found and fixed two bugs (2026-09-15):**
1. `StartX`/`StartY`/`NumX`/`NumY` weren't implemented at all — real Alpaca clients (unlike my own `curl`
   testing) PUT `NumX`/`NumY` to the full frame size before *every* exposure, even a full-frame one, and
   abort if that PUT errors. Now accepted/stored/reported (real cropping still not implemented — capture
   always returns the full sensor frame regardless of what's requested).
2. `imagearray` originally streamed with `Connection: close` and no `Content-Length`, and a first attempt
   at fixing that by buffering the whole ~11MB response in one `malloc` **OOM'd and crash-rebooted the
   board** — this is where the 32MB (not 64MB) RAM figure above was actually discovered. Fixed properly
   with a two-pass approach: count the exact byte length first (cheap, no allocation), then stream through
   a small fixed buffer after declaring that exact `Content-Length`. Verified: correct `Content-Length`
   header matching actual bytes sent, memory flat before/after (no growth), valid 1296×2304 JSON output.

Confirmed working end-to-end from Windows: N.I.N.A.'s (or similar) Alpaca autodiscover found the device
correctly as "OpenAstroGuider Camera" at a self-assigned `169.254.x.x:11111#0` address — validates the
link-local networking fix over the real link, not just `adb forward`. Discovery currently takes ~15-20s
from plugging in (not yet investigated — likely IPv4LL's probe/announce timing, RFC 3927 allows up to
~9s of probing alone before even attempting to bind).

**Follow-up, same day: real PHD2 guiding achieved, then two more issues chased.** Got actual guide
exposures out of PHD2. "Images every 5-10s" root cause: JSON `imagearray`'s size/parse cost — PHD2 has no
native Alpaca client, it connects via the ASCOM Platform's Alpaca-to-COM bridge, which (like `alpyca`)
auto-negotiates the faster binary format when offered. **`ImageBytes` is now implemented** (11 little-
endian int32 header fields + raw `uint16_t` data, streamed via a single `mg_write` straight from the
existing capture buffer — no extra allocation, safer than even the JSON path on this ~32MB device). Field
layout taken directly from ASCOM's own `alpyca` client source, not guessed. **Verified 0.68-0.70s fetch
vs 6.4s for JSON (~9x faster)**, confirmed correct via both a manual decode and `alpyca` itself. This also
**definitively resolves the row/column ordering question** — alpyca's own docs confirm row-major, and its
shape report matches `camera_api.c`'s existing `[row][col]` nesting exactly; there was never an ordering
bug. Separately, "exposure duration doesn't apply" was tested and is **not a server-side bug**: 0.01s
exposure gave mean 85, 1s exposure gave mean 677 with the sensor saturating at 1023 (full overexposure,
correct daylight behavior).

**Both confirmed fixed in real end-to-end PHD2 testing on Windows with the latest build**: images arrive
noticeably faster, exposure duration now looks correct. Likely explanation for the duration symptom: with
the old ~6-10s JSON fetch, PHD2 could have been displaying a stale frame while a much shorter new exposure
had already completed — a symptom of the slow-fetch bug, not a separate one. Resolved once ImageBytes
dropped fetch time to ~0.7s.

**Not yet done:** tested against the ASCOM Conformance tool; investigate the 15-20s discovery delay
(likely IPv4LL probe/announce timing, RFC 3927 allows ~9s of probing alone); diagnose PHD2's
duration-not-applied symptom from the client side, now that the server side is proven correct.

PC-side tooling: `grab.py` was the sensor-bringup/testing tool and **is no longer being maintained** —
the project has moved on to the on-device Alpaca driver above; `grab.py` and its diagnostic siblings
(`isp_grab.py`, `noise.py`, `bayer_phase.py`, `planes.py`, `period.py`, `bitorder.py`, `to_fits.py`) can
be left to bit-rot.

Working style for this project: prefer empirical validation over guessing (measure before cutting, use
dark frames as ground truth); push back on unverified fixes offered as definitive — get the actual
measurement or check.

Open next steps: (1) re-verify the real usb0 link now that addressing is link-local, not static, and test
against a real Alpaca client or the ASCOM Conformance tool — in particular confirm `imagearray`'s
row/column element ordering; (2) physically reflash with the current build (`output/image/sd_update.img`,
not a dated `IMAGE/` snapshot) to measure the `quiet` bootarg's real effect and re-run the boot-time
measurement — USB flashing isn't possible on this board (no onboard eMMC/NAND), use SocToolKit's SD Card
tab; (3) push the SC3336
exposure ceiling as far as it goes — the go/no-go signal for long exposures, and note the Alpaca driver's
`ExposureMax` already tracks this automatically once it moves; (4) implement `ImageBytes` binary transfer
in the Alpaca driver once real-client testing is underway (JSON `imagearray` is ~2x the raw payload size);
(5) optional follow-up: disable ISP/RGA/MPP/NPU/audio in kernel Kconfig too (they currently still
compile, just aren't loaded) for a further flash-size cut; (6) separately, a custom SPI NAND board
config for the 64–128MB deployment target — not started, independent of the SD_CARD debloat above; this
is also where Rockchip's thunderboot fast-boot feature becomes applicable; (7) longer term, retarget the
whole stack to RV1106G3 (256MB RAM) once validated on RV1103.
