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

**SharpCap black image + N.I.N.A. total failure, same root cause, found and fixed.** N.I.N.A. gave a
precise error: `The JSON value could not be converted to System.Int16 ... BytePositionInLine: 14`, which
lands exactly on our `gainmax` value (99614, the raw V4L2 `analogue_gain` max) in a `{"Value":99614,...}`
response. **ASCOM types `Gain`/`GainMin`/`GainMax` as `Int16`** (max 32767) — 99614 blows past that.
Likely explains SharpCap too: if it can't parse a valid gain range either, it may default to an unusable
gain (e.g. 0, below our real minimum of 128 = 1x) and produce black frames regardless of exposure. Fixed
by translating `gain`/`gainmin`/`gainmax` to/from a fixed `Int16`-safe ASCOM-facing scale (`0..1000`) at
the API boundary, keeping the raw value internal.

**That alone didn't fix SharpCap.** N.I.N.A. connected afterward but still black; added request/exposure
logging to `alpacad` to see real client traffic (had to fix a stdio-buffering bug first — `stderr` is
block-buffered once redirected to a file, so log lines never reached disk until `setvbuf(..., _IONBF, 0)`
was added to `main()`). The captured SharpCap log showed the server working *correctly* the whole time —
every exposure's requested/applied hardware values matched exactly, and captured data was genuinely valid.
The real bug: SharpCap's normal gain-calibration sweep reached ASCOM gain ≈898/1000, which our then-linear
mapping across the sensor's full native range (128..99614 raw, ~1x..~778x) translated to raw gain ~89466
(~700x) — saturating badly, so SharpCap compensated with a ~2ms exposure that reads out as pure noise floor
at that gain (indistinguishable from black). Fixed by capping the ASCOM-exposed gain range to a practical
32x ceiling instead of the sensor's full ~778x range — a guide camera has no real use for gain that high,
it just amplifies noise. Re-verified the exact scenario from the log now maps to ~28.8x instead of ~700x.
**Correction: the gain theory was wrong.** Asked directly whether the very first exposure (mean=472.5,
well-exposed, gain untouched) also displayed black — yes, **every image was identically flat black, "not
even noisy, just black."** That rules out an exposure/gain data problem entirely (real underexposed data
still shows read noise); it points to something structurally broken in how the client decodes the
response, not what's in it. The gain-scale cap stays in (independently correct), but it wasn't the fix.
Also retracted the "SharpCap auto-calibrates toward a brightness target" claim — the gain-sweep sequence
starts *while a previous exposure is still capturing*, not after examining its result, so that was
overconfident inference, not a verified mechanism. Forced JSON-only and retested: SharpCap gave a precise
error instead — `image array element type Unknown is not supported (0x8004040b)`. **Real root cause
found**: read an independent, open-source Alpaca server implementation
(`github.com/mikefsq/goalpaca/server/imagearray.go`) and found two concrete bugs. (1) Our JSON response
was missing the required `"Type"`/`"Rank"` fields entirely (`{"Type":<n>,"Rank":<n>,"Value":[...],...}` —
SharpCap reads the missing `Type` as its zero-default `Unknown`). (2) **The array dimension order was
backwards in both JSON and `ImageBytes`** — the correct wire convention is `[Width][Height]` with X as the
*outer* index (`Value[x][y] = Pixels[y*Width+x]`), not row-major as previously claimed. That earlier claim
was based on alpyca's client-side reshape code, which just mirrors whatever dimensions a server sends
without validating against spec — and pixel statistics (min/max/mean) are transposition-invariant, so
nothing caught this until an independent reference implementation was checked directly. Likely also
explains the *original* `ImageBytes` black-image symptom: wrong dimension order silently failing
SharpCap's own documented strict frame-validation. Both fixed in `camera_api.c`; loop logic verified
structurally correct via a small hand-traceable host-side test (board unreachable from this machine at fix
time). **Not yet verified on real hardware or a real client session** — meaningfully higher confidence
than the two earlier wrong theories (grounded in an independent authoritative implementation, not log
inference), but still needs a real retest.

**Not yet done:** retest SharpCap/N.I.N.A. with this fix (the real test); tested against the ASCOM Conformance tool;
investigate the 15-20s discovery delay (likely IPv4LL probe/announce timing, RFC 3927 allows ~9s of
probing alone).

**Real per-frame latency measured precisely, two fixed root causes found (2026-09-15).** User reported
0.1s PHD2 exposures still coming in slow. Added `CLOCK_MONOTONIC` timing to every V4L2 ioctl stage and
split the `imagebytes` send into pack-vs-write time rather than guessing. Found two large, *fixed* costs,
almost totally independent of the requested exposure duration — full numbers and analysis in
`alpaca/README.md`, "Real per-frame latency measured precisely":
1. `v4l2_capture_frame()` opens/closes `/dev/video0` on every single exposure, and the `STREAMOFF`+
   `munmap`+`close` teardown alone costs ~515-523ms every time — confirmed via direct measurement (not the
   earlier guessed "hardware pipeline warm-up" theory, which only actually applies to the very first
   capture after daemon start). A 1.1ms sensor exposure still took 643ms total.
2. `send_imagebytes()`'s ~1.3s send time is **CPU-bound, not network-bound**: the transposed pixel-pack
   loop (needed for the `[Width][Height]` wire order fixed earlier this session) takes ~1078ms of it, while
   the actual `mg_write()` network transfer is only ~250ms (~23MB/s, a normal RNDIS rate). Classic
   stride-based cache-miss pattern, not a bandwidth problem.

Combined fixed floor was ~2 seconds per frame regardless of exposure duration. Two independent fixes
identified; the first is now implemented (below), the second — a cache-blocked transpose (or native
wire-order internal storage) instead of the naive stride-w copy in `send_imagebytes()` — is still open.

**Persistent V4L2 device: implemented and hardware-verified, two real settling bugs found and fixed
(2026-09-15).** `v4l2_capture_frame()` no longer opens/closes `/dev/video0` per exposure. Split into
`v4l2_capture_init()` (open, format, allocate one mmap'd buffer, `STREAMON` — called once at daemon
startup in `main()`, right after sensor detection; daemon exits if it fails) and `v4l2_capture_frame()`
(now takes no sensor argument, reuses the buffer set up by `_init`). Deliberately kept to a **single**
buffer, not the old 4-buffer pool — with the driver only ever holding one frame in flight, the correctness
argument for continuous streaming stays simple: dequeue-and-requeue whatever's already ready (discarded),
then dequeue again for the frame that actually reflects the caller's just-applied controls.

On real hardware this needed **two** settling fixes, both confirmed via reboot + controlled testing
(alternating dim/bright exposure requests, reading the existing `requested ... -> applied ...` /
`captured min/max/mean` log lines — not just trusting the log's control readback, which was exactly what
was misleading before this was checked): (1) the very first exposure after daemon startup could return a
stale/wrong frame — fixed with a throwaway discard-and-capture cycle inside `v4l2_capture_init()` itself,
before the HTTP server starts; (2) the first request in a session that raises `vertical_blanking`
(exposure_worker() does this when the requested exposure exceeds the sensor's frame-length ceiling — see
"Validated sensor/driver facts" above) also came back stale, since changing the frame period evidently
needs more settling than a plain gain/exposure change — fixed generally by discarding **two** frames
(not one) before every real capture, rather than trying to detect which requests are "first of their
kind". Verified end-to-end: `imagearray` (both JSON and `ImageBytes`) now returns correctly-shaped,
correctly-valued data on every request, with no lag from a previous request's settings, and `ImageBytes` is
still ~6x faster than JSON in practice (1.4s vs. 8.9s for a full frame).

Cross-compiles cleanly (`-Wall -Wextra`, no warnings); binary stripped and kept in sync in the
`overlay-luckfox-astroguider` overlay. Full detail in `alpaca/README.md`, "Persistent V4L2 device".

**Latency follow-up, same day: PHD2 0.5s-exposure loop still took longer than 0.5s — two more fixes, both
hardware-verified.** (1) The `vertical_blanking`-settle fix above originally paid its extra discard round
on *every* capture, not just the one that actually raises `vertical_blanking` — `v4l2_capture_frame()` now
takes an `extra_settle` flag that `exposure_worker()` only sets on that specific request. (2)
`send_imagebytes()`'s transpose loop was cache-blocked (32x32 tiles, row-major reads) instead of one
`pixels[y*w+x]` at a time — measured **pack time dropped from ~1078ms to ~80-100ms (~12x)**. A full
0.5s-exposure loop (via `curl`/`adb forward`) went from ~2.3-2.5s to ~0.9-1.3s. Remaining dominant cost is
`mg_write()` itself, measured ~620-655ms here — but that's through the `adb forward` TCP tunnel (this host
has no route onto the board's real `usb0` RNDIS interface), not the real link a client uses, so treat it as
a testing artifact pending a real-client remeasurement, not a confirmed regression. Full detail in
`alpaca/README.md`, "Latency: conditional discard + cache-blocked transpose".

**Boot-to-discoverable delay: dhcpcd `usb0` timeout fix added, not yet measured (2026-09-15).** `dhcpcd`
always tries a real DHCP lease first on every interface, including `usb0` — a point-to-point USB RNDIS
link that never has a DHCP server on the other end, so that solicit wastes several seconds before falling
back to the IPv4LL self-assignment that's actually used (see the networking fix above). Added an
`etc/dhcpcd.conf` overlay (`interface usb0 { timeout 1 }`) via the same `RK_POST_OVERLAY` mechanism.
Verified it lands in the built rootfs; not yet rigorously timed before/after on hardware.

**RAM/CMA reservation: confirmed not yet touched, and reasoned through why it likely wouldn't help
framerate anyway (2026-09-15).** User asked whether shrinking the 24MB CMA reservation (see RAM
explanation above) would help the framerate complaint. It has not been implemented. Reasoning: CMA backs
V4L2's DMA capture buffers specifically, general `MemTotal`/`malloc()` backs `alpacad`'s own heap — they're
separate pools, and the real per-frame latency (see above) is capture/serialize-bound, not
memory-availability-bound. This reasoning has not itself been empirically stress-tested, but the timing
instrumentation above already fully explains the observed slowness without invoking RAM at all.

**Hot-iteration workflow found, with a bus-power gotcha (2026-09-15).** For `alpacad`-only changes,
`adb push`ing the cross-compiled binary straight to `/usr/bin/alpacad` and restarting via
`/etc/init.d/S60alpacad restart` is much faster than a full SD-card reflash. **Gotcha:** this board is
USB-bus-powered, so unplugging it (to move it between machines) is an unclean power cut, not a graceful
shutdown. `adb push` doesn't `fsync`, and ext4 delayed allocation can leave the pushed data dirty in page
cache — cutting power before it flushes zeroes the file on the next boot's journal replay. This happened
for real this session (`alpacad` silently became a 0-byte file after a reconnect, which looked exactly
like a network/discovery bug at first). **Always run `sync; sync` on-device right after any `adb push` of
a binary meant to survive a power cycle.** Also: the live SD card now has newer `alpacad` code (via this
hot-push workflow) than the last full `./build.sh ... firmware` output — run a fresh full build before
trusting `output/image/sd_update.img` for a reflash.

PC-side tooling: `grab.py` was the sensor-bringup/testing tool and **is no longer being maintained** —
the project has moved on to the on-device Alpaca driver above; `grab.py` and its diagnostic siblings
(`isp_grab.py`, `noise.py`, `bayer_phase.py`, `planes.py`, `period.py`, `bitorder.py`, `to_fits.py`) can
be left to bit-rot.

Working style for this project: prefer empirical validation over guessing (measure before cutting, use
dark frames as ground truth); push back on unverified fixes offered as definitive — get the actual
measurement or check.

Open next steps: (1) commit the current working-tree changes (dimension-order/`Type`/`Rank` `imagearray`
fix, Int16-safe gain scale, persistent V4L2 device incl. the two settling fixes, latency instrumentation,
dhcpcd `usb0` timeout) — all now hardware-verified this session (direct HTTP/log-based testing), nothing
since the "Confirm ImageBytes and exposure fixes" commit has been committed yet; (2) retest against a real
SharpCap/N.I.N.A./PHD2 session — this session verified correctness via direct HTTP calls and log
inspection, not a live ASCOM client, though the underlying data is now confirmed correct so this is
expected to just work; (3) re-run the boot-time measurement to check the `quiet` bootarg's real effect,
now that a board is connected; (4) the cache-blocked transpose fix for `send_imagebytes()`'s pixel-pack
loop (~4x the cost of the actual network write) — the other half of the per-frame latency work, still not
started, and now the largest *unfixed* latency item; (5) push the SC3336 exposure ceiling as far as it
goes — the go/no-go signal for long exposures, and note the Alpaca driver's `ExposureMax` already tracks
this automatically once it moves; (6) optional follow-up: disable ISP/RGA/MPP/NPU/audio in kernel Kconfig
too (they currently still compile, just aren't loaded) for a further flash-size cut; (7) separately, a
custom SPI NAND board config for the 64–128MB deployment target — not started, independent of the SD_CARD
debloat above; this is also where Rockchip's thunderboot fast-boot feature becomes applicable; (8) longer
term, retarget the whole stack to RV1106G3 (256MB RAM) once validated on RV1103.
