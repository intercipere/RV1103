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

**Not yet done** (at the time of the entry above — SharpCap was resolved later the same day by the
`ImageElementType` fix, see below): tested against the ASCOM Conformance tool; investigate the 15-20s
discovery delay (likely IPv4LL probe/announce timing, RFC 3927 allows ~9s of probing alone).

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

**Stripes regression found and fixed, plus a pinned-sensor simplification (2026-09-16).** User reported the
previous commit broke the image completely — "only stripes now." Root cause: the cache-blocked transpose in
`send_imagebytes()` (added in that same commit) streamed 32x32 tiles in x0-outer/y0-inner order, so each
column's rows were emitted 32 at a time interleaved with 31 other columns instead of a whole column at a
time. Byte count and pixel values were all correct — only the order was wrong, which is exactly why the
pack-time win (~1078ms → ~80-100ms) looked clean: nothing in that session checked ordering. Fixed by
blocking over a **band of whole columns** rather than square tiles, so concatenating bands is the wire
order by construction. Verified on the host against a naive reference transpose (old loop differs, new one
matches, including partial-band/partial-tile dimensions). Two related changes in the same pass:
- **`REQBUFS count` restored from 1 to 3.** With a single buffer held dequeued across the whole unpack the
  driver has no other DMA target, so on rkcif the in-flight frame can land back in the buffer userspace is
  reading — a plausible second contributor to the stripes, though the transpose is the confirmed one. The
  fd is now `O_NONBLOCK` + `poll()`, which also gives DQBUF a real timeout.
- **[partly superseded — see "Long exposures restored" below: `vertical_blanking` is client-driven again,
  only the gain pin remains]** **`analogue_gain` pinned to 128 and `vertical_blanking` to 64** (project
  decision, to make frame-delivery latency the only moving part), set once at startup before `STREAMON`
  instead of per frame. **This capped exposure at ~1352 rows ≈ 37ms** while it was in force;
  `exposuremax` now reports that real ceiling and `startexposure` clamps to it. Gain PUTs are accepted but
  not applied (erroring makes real clients abort). Separately, every `v4l2_ctrl_get/set` used to reopen the
  subdev and walk the *entire* control enumeration before its one ioctl — seven times per frame in
  `exposure_worker()`; fd and control ids are now cached behind a mutex.
- **Next lever:** unpack and transpose are two separate full passes over ~6MB; fusing them (unpack straight
  into wire order, on the exposure thread) would remove one. Beyond that, `mg_write()` of 5.97MB at the
  ~23MB/s measured over real RNDIS is a hard ~3.8 fps ceiling that only binning/subframing can move.
- **Hardware-verified same day.** The fix is confirmed on the real board: the same frame fetched as
  `ImageBytes` and as JSON is elementwise identical, and the decoded frame renders as a normal coherent
  image; pushing the *same* pixels back through the old tiled loop reproduces the reported vertical stripes
  exactly. (A "spatial coherence"/adjacent-row-correlation check was tried first and **does not
  discriminate** — the scrambled image scored higher. Elementwise comparison against the JSON path, and
  looking at the rendered image, are what settle it.) `exposuremax` now reports 0.0371s, matching the
  pinned vblank exactly, and `gain` reports 0 (raw 128).
- **Measured latency: ~0.89-0.93s per full loop** at a 20ms exposure (down from ~0.95s), device-side
  `discard=67 dqbuf=32 unpack=64 / pack=74 write=615`. Also removed ~60ms/frame of pure waste: the
  `captured min/max/mean` diagnostic walked all 3M pixels *before* marking the frame ready, for one log
  line — now gated behind the `OAG_FRAME_STATS` env var.
- **Real-link measurement settled the same day: `write=265ms`, full loop ~0.51-0.54s.** The 615ms was an
  `adb forward` tunnel artifact inflating it ~2.3x; 5.97MB at ~22.5MB/s matches the earlier live-PHD2
  figure. Treat every older latency number in this file measured via `adb forward` with that in mind.
  Real-link budget: write 265ms (~51%), capture 148ms (~80ms of it the two-frame freshness guarantee,
  67ms unpack), pack 76ms. **Getting onto the real link from Linux:** NetworkManager owns the `enx*`
  gadget interface and sits forever in `connecting (getting IP configuration)` doing DHCP on a
  point-to-point link, flushing any manually added address — which is what made this look unfixable and
  kept every prior session on `adb forward`. Fix with
  `nmcli con mod "Wired connection N" ipv4.method link-local && nmcli con up "Wired connection N"`
  (no sudo needed). Also a product observation: Linux client hosts get no working link out of the box,
  where Windows/macOS self-assign IPv4LL.
- **Unpack+transpose fused (implemented, host-verified, not yet measured on hardware).**
  `unpack_bits_lsb_transposed()` writes `out[x*height+y]` directly in 32x32 blocked tiles, so
  `v4l2_frame_t.pixels` is stored in ImageBytes wire order throughout, `send_imagebytes()` is a single
  `mg_write` with no per-pixel work, and the JSON path reads sequentially too. Removes a full 6MB
  write+read pass; expect ~50-70ms, not the full 76ms (the transpose work still happens, just once).
  Verified on host against the row-major unpack + explicit transpose at five geometries incl. ragged
  tiles and padded stride. **`frame.pixels` is no longer row-major** — index `[x*height+y]`.
- **USB: RV1103/RV1106 is USB 2.0 only.** `rv1106.dtsi` has a DWC3 controller (USB3-capable core) but
  `maximum-speed = "high-speed"` and only a `u2phy` USB 2.0 PHY — no SS PHY node exists. No PCB design can
  add USB3; that needs a different SoC. **But measured RNDIS throughput is 22.5MB/s vs ~40-45MB/s
  realistic for USB 2.0 high-speed bulk — the gadget protocol, not the bus, is the limit.** CDC-NCM
  aggregates frames per USB transfer where RNDIS does not, so ~2x may be available on existing hardware;
  the tradeoff is RNDIS's driver-free Windows 10 support. Untested hypothesis, needs measurement.
- **Long exposures restored (2026-09-16).** Real PHD2/SharpCap/N.I.N.A. testing confirmed the frame rate is
  acceptable, so `vertical_blanking` is client-driven again; **gain stays pinned at 128**. Two things the
  original version lacked: blanking is now *lowered* again when a short exposure follows a long one (left
  high, the frame period stays long and every later short exposure waits out the old slow period), and the
  control write order depends on direction (raise blanking before setting a longer exposure; set the
  shorter exposure before narrowing blanking, then re-assert it, since the driver clamps exposure against
  current blanking).
- **`ImageBytes` `ImageElementType` was inconsistent with the JSON path** — JSON announced `"Type":2`
  (Int32) while ImageBytes announced `ImageElementType=8` (UInt16) for the same image. Per spec
  ImageElementType is what the client materializes (ASCOM `ImageArray` is Int32 = 2) and
  TransmissionElementType is the narrower wire type (UInt16 = 8). Both were 8; now 2 and 8. Lenient
  clients read TransmissionElementType, which is why PHD2/N.I.N.A. were unaffected.
- **SharpCap RESOLVED (2026-09-16):** the `ImageElementType` fix (candidate 1) was it — SharpCap now
  displays correctly alongside PHD2 and N.I.N.A. Original triage kept below for method.
- **Bayer matrix was reported red/blue-swapped — fixed and verified (2026-09-16).** Both SharpCap and
  N.I.N.A. showed brown as blue. **ASCOM's `SensorType` enum has no BGGR/GRBG/GBRG members** (RGGB=2 is the
  only Bayer value), so the arrangement can only be conveyed via `BayerOffsetX/Y`, giving where the
  top-left pixel sits in the reference RGGB 2x2: (0,0)=RGGB, (1,0)=GRBG, (0,1)=GBRG, **(1,1)=BGGR**. The
  SC3336 is BGGR but reported (0,0). Fixed to (1,1); verified on hardware. Confirmed the sensor really is
  BGGR by debayering one frame all four ways: BGGR gives warm lamps/green plants/wood floor, RGGB the exact
  mirror, GRBG/GBRG collapse to R≈B with G suppressed (wrong-phase signature). **Offsets are now derived
  from `desc->bayer` and the `bayer_offset_x/y` struct fields removed** — two sources of truth is what let
  them drift. If subframing is added, an odd StartX/StartY shifts the effective pattern and must be XORed in.
- **[historical] SharpCap still black while PHD2 and N.I.N.A. work (open).** Not an autostretch issue per the user.
  Three earlier SharpCap theories in this file were wrong, so: **no more fixes shipped as explanations
  without a captured log.** Candidates ranked: (1) the ImageElementType mismatch above (fixed, untested);
  (2) 10-bit data in a 16-bit container — real values ~60-180 of 65535, so a client using a fixed shift
  instead of `MaxADU` (correctly reported as 1023) renders exactly zero everywhere, matching "not even
  noisy, just black"; fix would be a selectable full-range scale (<<6), not a format change; (3)
  `sensortype` reports 2 (RGGB) though SC3336 is **BGGR** with `bayer_offset_x/y` both 0 instead of (1,1) —
  a real bug, but it causes wrong colours, not black. Next step is `/tmp/alpacad.log` from a SharpCap
  session, not another patch.
- **JPEG output is not possible in Alpaca.** `ImageArray`/`ImageBytes` are typed numeric arrays (element
  type enum covers Int16/Int32/Double/Single/Byte/Int64/UInt16 only); there is no encoded-image transport,
  so no ASCOM client could request or decode one. The correct mechanism for user-selectable output is
  **`ReadoutModes`/`ReadoutMode`**, a driver-defined named list that SharpCap and others surface in their
  UI — the right home for "Raw 10-bit" vs "Scaled 16-bit" or a binned mode. Not implemented.
- **Hardware-verified (2026-09-16):** fused unpack correct (ImageBytes == JSON, image clean, header
  `ImageElementType=2/Transmission=8`); `unpack` 67->88ms but `pack` 76->0, net ~55ms as predicted, 0.02s
  loop ~0.49s. **`ExposureMax` = 0.899s** — that is the SC3336's real driver-enforced ceiling, answering
  the long-open "push the SC3336 exposure ceiling" question: ~0.9s. Long exposures apply exactly
  (`rows=18214 -> vblank=16990`).
- **[SUPERSEDED — the conclusion below was wrong; see "Stale frames" in the 2026-09-17 section]**
  **`extra_settle` removed — measured obsolete (2026-09-16).** It discarded a second frame on any blanking
  change, added for a stale-frame bug under the **old single-buffer** scheme; the 3-buffer pool + non-
  blocking drain makes one discard sufficient. Verified by making it runtime-switchable
  (`OAG_EXTRA_SETTLE=0/1`) and A/B-ing the *same binary* over 14 exposures alternating 0.005/0.3/0.5/0.8s
  in both directions, **starting with a long exposure right after daemon start** (the exact original
  failure case), using the device's own `OAG_FRAME_STATS` means as ground truth. Correctness identical;
  loop time **-25% at 0.3s, -31% at 0.5s, -36% at 0.8s**. Flag and parameter fully removed.
  **Why this was wrong:** the A/B used `OAG_FRAME_STATS` as ground truth, and that stats walk costs ~85ms
  per capture — enough to hide the race. With stats **on** the failure does not reproduce at all; with
  stats **off** it reproduces ~40% of the time. The instrument changed the thing it was measuring.
- **USB gadget MAC pinned (2026-09-16, hardware-verified).** `S50usbdevice` never set RNDIS
  `host_addr`/`dev_addr`, so `f_rndis` randomized them every boot; `host_addr` is what names the host's
  interface (`enx<host_addr>`), so Linux got a new NM profile stuck in DHCP on every replug and Windows a
  new network profile each plug. Fixed in the overlay's forked `S50usbdevice` (2 functional lines after
  `mkdir .../rndis.gs0`), with the MAC **derived from the SoC chip serial** (`/proc/cpuinfo` `Serial`)
  rather than hardcoded, so boards are stable across reboots but distinct from each other. 0x02/0x06 first
  octet = locally administered, unicast. Verified by real reboot: `02:21:7a:b8:e8:3f`, adb back in ~10s,
  host interface now permanently `enx02217ab8e83f`. Low-risk: vendor `test_write` is
  `test -e $2 && echo $1 > $2`, so a rejected write changes nothing. Host side: one-time
  `nmcli con add type ethernet con-name oag-usb ifname enx02217ab8e83f ipv4.method link-local`.
- **Cooling is not a copied remnant and should not be removed (2026-09-16).** `CanSetCCDTemperature` and
  `CanGetCoolerPower` are mandatory `ICameraV3` members; the driver answers `false`, and
  `cooleron`/`coolerpower`/`ccdtemperature`/`setccdtemperature`/`heatsinktemperature` all return
  `ErrorNumber 1024` (0x400 NotImplemented) — verified live. A client drawing a cooling panel anyway is
  making its own UI choice. Deleting those members would make the driver less conformant.
- **Dew heater added as an Alpaca Switch device (2026-09-16, hardware-verified).** ASCOM's Camera
  interface has no dew-heater member; Switch (ISwitchV2) is the interface meant for auxiliary controls,
  and one Alpaca server can host several devices. Lives at `/api/v1/switch/0/`, enumerated alongside the
  camera, so SharpCap/N.I.N.A. show it in their existing Switch UI. Plain on/off
  (`Min=0 Max=1 Step=1`) by project decision — a percentage would need PWM and `/sys/class/pwm` is not
  exported (needs a `pwm` DTS node + `CONFIG_PWM_SYSFS`). **State persists in
  `/userdata/dewheater.state`** (fsync'd; `/tmp` is a RAM disk) and is restored *and re-applied to the
  GPIO* at startup, so it survives a power cycle with no client — verified across a real reboot. **GPIO
  pin comes from `OAG_DEWHEATER_GPIO`** (documented in `S60alpacad`, no rebuild needed), driven via
  `/sys/class/gpio`; unset it is a working logical control that drives nothing, which is what let this be
  finished before the PCB exists. All GPIO failure modes (bad pin / pin held by another driver /
  malformed) degrade gracefully and are verified. **The success path — a real pin toggling — is NOT
  verified**; only 3 GPIOs are claimed on this board and driving an arbitrary unrouted pin wasn't worth
  the risk. That test belongs with the PCB.
- **Setup page added, so the dew heater is reachable from PHD2 (2026-09-16, hardware-verified).** The
  Switch device is correct ASCOM but only helps in a client that implements ASCOM Switch; **PHD2 does
  not**, so the heater was working and completely unreachable there. PHD2's per-camera **Settings** button
  calls `SetupDialog()`, which for an *Alpaca* device the ASCOM Platform implements by opening the system
  browser at the device's setup URL — it cannot draw a native dialog for a driver on another machine, so
  **a native popup inside PHD2 is not something this driver can provide**; the browser page is the Alpaca
  equivalent. Nothing was listening on those URLs (`/setup` and
  `/setup/v1/<devicetype>/<devicenumber>/setup` are both required by Alpaca — a conformance gap of its
  own). `alpaca/src/setup_api.c` now serves one self-contained page (no CDN — the link-local USB network
  has no internet route; dark/red for night vision) at both, plus the bare root. Camera facts on it are
  fetched from the existing Alpaca API by the page itself rather than re-derived server-side — two sources
  of truth is what swapped red/blue in the Bayer offsets — and the toggle drives the same
  `PUT /api/v1/switch/0/setswitch` a Switch-aware client would. Root is registered as `"/$"`, not `"/"`:
  as a civetweb *pattern* a bare `"/"` prefix-matches every URL, which would make unknown `/api/v1/...`
  paths return HTML instead of a JSON error. Verified over the real RNDIS link in a real browser: all four
  URLs serve the page with an exact `Content-Length`, API routing unaffected, no console errors, and a
  click in the browser flipped the real device (`getswitch` false, `/userdata/dewheater.state` 0, daemon
  logged it) and back.
- `common_dispatch()` now takes a `common_device_t` (per-device name/description/interface version and its
  own `Connected` flag) instead of being hardwired to the camera; `parse_request_params()` moved to
  `http_util.h` so both dispatchers share it.
- **Remaining lever:** binning/subframe — the only thing that cuts the 265ms write, and much bigger:
  2x2 binning gives 1.5MB and a ~66ms write, ~200ms saved. IMX290 (production) is mono so 2x2 binning is
  trivial there; SC3336 is Bayer, so binning a quad mixes colour channels — a real design decision.
- **Starting `alpacad` over adb needs `setsid`** — `adb shell "/etc/init.d/S60alpacad restart"` backgrounds
  it as a child of the adb session, so it dies when the session exits, and the stale boot-time
  `/tmp/alpacad.log` makes it look like it started fine.

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

**[SUPERSEDED — measured and it does not work; see "Boot-to-discoverable: measured and fixed properly"
below] Boot-to-discoverable delay: dhcpcd `usb0` timeout fix added, not yet measured (2026-09-15).**
`dhcpcd` always tries a real DHCP lease first on every interface, including `usb0` — a point-to-point USB
RNDIS link that never has a DHCP server on the other end, so that solicit wastes several seconds before
falling back to the IPv4LL self-assignment that's actually used (see the networking fix above). Added an
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

## Long exposures and the stale-frame bug (2026-09-17)

Prompted by planning the custom PCB (IMX327), the SC3336 was pushed past its 0.899s ceiling to exercise
the long-exposure path before IMX hardware exists. Two ceilings, both software:

- **`SC3336_VTS_MAX` raised 0x7fff -> 0xffff (shipped).** The vendor value was exactly half the range of
  the 16-bit VTS register pair (0x320e/0x320f) that `sc3336_set_ctrl()` already writes unmasked — a
  software cap, not a hardware one. Bit 7 of 0x320e **is** implemented: `ExposureMax` 0.899s -> **1.799s**,
  `vertical_blanking` max 31471 -> 64239, verified with real 1.79s exposures returning correct data.
- **HTS (row time) is the bigger lever, but was only used as a throwaway diagnostic (not shipped).**
  Row time is HTS/pixel_rate, and the driver never writes HTS at all — the sensor runs its power-on
  default. A temporary module param scaling it reached **`ExposureMax` = 10.79s** at 6x, with real
  6.0s and 10.5s exposures verified. Reverted after testing; `git log` has it if ever needed again.
  **Note for anyone redoing this:** the HTS register read back as **1250 (0x04e2)**, NOT `hts_def`/2
  (`hts_def` is 2800, so the ratio is 2.24, not 2). Do not assume `hts_def`'s units — read
  0x320c/0x320d back and scale *that*, which is what made the experiment correct despite the wrong guess.

**`DQBUF_TIMEOUT_MS 5000` was a real ceiling, now measured rather than argued.** At 6.0s the DQBUF wait
was 5367ms and at 10.5s it was 9384ms — both past the old flat timeout, so those exposures would have
failed in the capture layer regardless of sensor support. `v4l2_capture_frame()` now takes
`frame_period_s` and derives the timeout from it (3x period + 5s floor, 120s backstop), computed in
`exposure_worker()` from the *applied* vblank.

**Stale frames: a real correctness bug, found and fixed.** Alternating 0.05s/0.5s exposures, **3 of 8**
returned the *previous* exposure's frame — DQBUF waits of 46/51/47ms for a 0.5s exposure, physically
impossible. The old "drain what's ready, then discard exactly one more" is not sound: with a 3-buffer
pool the number of in-flight stale frames varies, so freshness depended on timing. This is what
`extra_settle` used to mask, and why its removal (2026-09-16) looked safe — see the SUPERSEDED note above.

Fixed in two parts, both necessary and both measured:
1. **Discard by frame timestamp, not by count.** rkcif stamps each buffer from the CSI frame-START
   interrupt (`capture.c`: `vb2_buf.timestamp = readout.fs_timestamp`), so frames that began before the
   control writes are identified directly. `exposure_worker()` samples the reference right after the
   writes via `v4l2_capture_now_ns()`. **That helper uses `CLOCK_BOOTTIME`, deliberately:** on RV1106
   `rkcif_time_get_ns()` is `ktime_get_boottime_ns()` (`cif/dev.h`) even though the queue advertises
   `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC`. They differ only across suspend, which this board never does,
   but matching the driver removes the assumption.
2. **Then exactly one more discard, because this is a rolling shutter.** The timestamp is readout start;
   integration begins ~one frame period earlier, so the first frame with `fs_timestamp >= ref` was
   already integrating when the controls were written. Timestamp filtering *alone* still returned a
   151ms frame for a 0.5s request — measured, not assumed. The extra discard is principled (it is the
   frame straddling the write), not tuned.

Result: 10/10 identical at `wait=506-514ms` for a 0.5s request (was: correct-or-151ms at random), means
rising monotonically with exposure (0.01s ~145, 0.2s ~560, 1.0s 908, 1.79s 972), `ImageBytes` byte-exact
at 5972012 with `ImageElementType=2`/`TransmissionElementType=8`. Cost is one extra frame period per
exposure versus the *fast-but-wrong* path; versus the previously-correct path it is roughly unchanged.

**Method note worth keeping:** `OAG_FRAME_STATS` is not a neutral observer. Its ~85ms walk changes
capture timing enough to hide this class of race entirely (8/8 clean with it on, 3/8 stale with it off).
Frame *means* are also a weak detector — a saturated daylight scene reads 1023 at every exposure. DQBUF
wall-clock time is the reliable signal for staleness.

**This bug is not sensor-specific and will follow us to IMX327.** It lives in the rkcif buffer pool plus
our capture logic, not in `sc3336.c`; any sensor whose control writes take effect a frame later has it,
and `imx327.c` has exactly the same shape (SHS1/VMAX over I2C, effective next frame). It matters *more*
there: a stale frame costs one whole exposure, so at the multi-second exposures the IMX327 is being
chosen for, a 40% stale rate would be crippling. The fix is in the sensor-agnostic V4L2 layer, so it
carries over unchanged.

## Boot-to-discoverable: measured and fixed properly (2026-09-17)

The question "why does it take several seconds before any program can see the ASCOM server" now has a
measured answer. From the board's own `dhcpcd` log, before the fix:

```
t=4s   usb0 carrier acquired
t=6s   soliciting a DHCP lease
t=11s  probing for an IPv4LL address   <- 5s of DHCP on a link with no server
t=16s  using IPv4LL address            <- 5s of RFC 3927 probe/announce
```

**~10 of those 16 seconds are spent negotiating an address on a point-to-point USB link that can never
have a DHCP server on it.** The host PC then does its own IPv4LL self-assignment on top, which is
additive and accounts for the rest of the 15-20s originally reported.

Two things were wrong, both previously assumed fine:

- **The `dhcpcd.conf` override was never actually running.** The overlay had it and the staged rootfs had
  it, but the flashed SD card predated it — exactly the stale-image trap documented under the
  hot-iteration workflow. The board had been running the stock Buildroot sample the whole time.
- **It does not work anyway.** Pushed the correct file, rebooted, measured: the DHCP phase is **5s either
  way**, an identical timeline. `interface usb0 { timeout 1 }` has no effect on when dhcpcd gives up and
  falls back to IPv4LL — that follows its DISCOVER retransmit schedule, not `timeout`. The override has
  been **removed** rather than left in place looking effective.

**Fix: assign the address directly and take dhcpcd off `usb0` entirely.** `S51usb0-linklocal` now derives
a `169.254.x.y` address from the gadget MAC (already pinned to the SoC chip serial by `S50usbdevice`), so
it is stable per board, distinct between boards, and needs no stored state. `etc/dhcpcd.conf` gained
`denyinterfaces usb0` so the two cannot fight — the static-vs-dhcpcd race is exactly how the original
`172.32.0.70` problem manifested. `eth0` still uses dhcpcd normally.

**Measured result: usb0 ready at t=6.39s instead of t=16s — ~9.6s saved.** The script now also writes
`OAG: usb0 link-local ready: <addr>` to `/dev/kmsg`, so `dmesg` carries a kernel-timestamped record of the
moment the camera becomes reachable; that is the one event that answers this recurring question, and
dhcpcd's log no longer covers this interface.

This is **not** a return to the old fixed IP. A `169.254/16` address still needs zero configuration on the
client, because Windows/macOS/Linux all self-assign into the same range. The deliberate tradeoff is that
it skips RFC 3927's probe step: on a two-participant USB link the collision risk is ~1 in 65024, the PC
side still probes, and it would move away from a collision on its own.

## HTS/row-time scaling shipped, default 12x (2026-09-17)

`SC3336_VTS_MAX` is now at the VTS register's 16-bit maximum, so row time is the only remaining lever on
the exposure ceiling (ceiling = VTS_MAX x row_time). `sc3336.ko` gained an `hts_mult` module parameter,
**set in the overlay's `insmod_ko.sh`, not baked into the driver**, so it is changeable with no rebuild.

Measured on real hardware:

| `hts_mult` | row time | `ExposureMax` | verified | shortest capture (10ms request) |
|---|---|---|---|---|
| 1 (stock) | 27.5 us | 1.80s | — | ~0.2s |
| **12 (shipped)** | **329 us** | **21.6s** | 15s, 20s | **~0.9s** |
| 52 (max) | 1427 us | 93.5s | 30s, **85s** | ~4.2s |

52 is the hard maximum: HTS is a 16-bit register and the base value reads back as 1250.

**The cost is symmetrical and is the whole design decision:** row time scales the *minimum* frame period
too, so a long ceiling makes short exposures slow. It hurts focusing and framing, not guiding, where the
exposure itself dominates. 12 was chosen as the balance — 21.6s is already well past anything guiding
needs, while ~0.9s for a short frame stays usable.

**If this is ever redone, do not derive the HTS register value from `hts_def`.** They disagree: the
register reads 1250 while `hts_def` is 2800 (ratio 2.24), and the mode table is not self-consistent about
units either (one entry writes `0x0578 * 2`, the other a bare `0x05dc`). The driver reads the register
back after the mode list is applied and scales *that*, which is what made the original experiment correct
despite a wrong initial guess of 1400. `h_blank` is scaled by the same factor so the row time userspace
derives from it stays consistent.

Note that at 93.5s the DQBUF timeout's 120s backstop (`v4l2_capture.c`) is uncomfortably close to a single
frame period. It works, but living near the 52x ceiling would want that raised.

## Row time was wrong by 12%, and the settle discard was paid unconditionally (2026-09-17)

Triggered by a user report that a 7s exposure "takes almost 5s for the image to arrive". Both halves of
that turned out to be real, and independent of each other.

**1. `hts_def` is wrong in the vendor mode table, so every exposure was ~12% short.** Userspace derives
row time as `(width + h_blank) / pixel_rate`, and `h_blank` comes from `hts_def`. Measured directly with
`v4l2-ctl --stream-count` at three `vertical_blanking` values spanning 5x:

```
vblank=64   -> vts=1360 -> 30.00 fps -> 24.510 us/row
vblank=2000 -> vts=3296 -> 12.38 fps -> 24.507 us/row
vblank=6000 -> vts=7296 ->  5.59 fps -> 24.519 us/row
```

`hts_def` (2800) / pixel_rate (102 MHz) claims **27.451 us/row** — 12% high. **The long-standing
"~27.45 us/row" figure in this file was never independently validated; it was derived from `hts_def` and
then checked only against other numbers derived from `hts_def`.** Consequences: a requested 7s exposure
was really 6.25s, and `ExposureMax` was overstated by 12% at every `hts_mult`.

The mode table's own `max_fps` and `vts_def` fields are self-consistent and correct:
`pixel_rate / (max_fps * vts_def)` gives **2500** for mode 0 and 2499 for mode 1, matching the
measurement exactly. (It also matches the HTS register, which reads 1250 and is evidently in 2-pixel
units.) `sc3336_true_hts()` now computes it that way; the bogus `hts_def` is left alone rather than
editing vendor data other code may compare against. `ExposureMax` at `hts_mult=12` is now an honest
**19.27s** (was a claimed 21.59s), `exposuremin` reports 294.12 us, and a 7s request now programs 23800
rows instead of 21250.

Note this also invalidates the earlier "verification" of the HTS lever at 2x, where 9107 rows took ~448ms
against a predicted 500ms — that 10% shortfall was this same bug, and it was read as agreement rather
than as the discrepancy it was.

**2. The settle discard was paid even when nothing changed.** The stale-frame fix costs a full extra
frame period (timestamp discard + rolling-shutter discard). That is necessary when exposure or blanking
just moved — but if neither changed, every frame already in flight was taken at exactly the requested
settings, so there is nothing to discard. `exposure_worker()` now compares the requested rows and
computed blanking against the values actually programmed, and passes `settle_ref_ns = 0` when they match,
which disables both discards.

This is the common case in a guiding loop, where a client repeats one exposure indefinitely. Measured for
a repeated 7s exposure: **11.9s -> 6.3s**, i.e. down to essentially one frame period, which is the floor.
Correctness checked by comparing a `settle=1` and a `settle=0` capture of the same 0.5s exposure: means
1022.9 and 1022.9, identical. The log line now reports `settle=` alongside `vblank_changed=`.

So the user's "almost 5s" was the unconditional discard, and the 7s that was really 6.25s was the row-time
bug. Both fixed.

## Exposure abort is advertised but not implemented — OPEN, with a real race underneath (2026-09-17)

Reported scenario: in SharpCap, start a 19s exposure, then move the slider to 0.5s while it is running.
The 0.5s exposure takes a very long time to arrive. Reproduced and measured over the API: the 0.5s frame
arrived **16.6s** after it was requested (requested 11:32:06.6, ready 11:32:23.2).

**Nothing here is fixed yet.** Three separate problems, in increasing order of how much they matter:

1. **`AbortExposure`/`StopExposure` do nothing but flip state.** `camera_api.c` sets
   `g_device.state = CAM_IDLE` and returns success; the comment there is honest that no mid-capture
   cancellation exists. But `canabortexposure` and `canstopexposure` both report **true**, so clients
   believe the abort worked. The worker thread stays blocked in `dqbuf_wait()` for up to a whole frame
   period — 19s here.

2. **Two exposure workers can run concurrently, racing on the V4L2 device.** `startexposure` only refuses
   when `state == CAM_EXPOSING`, and the abort just cleared that, so a second `exposure_worker` thread is
   spawned while the first is still inside `v4l2_capture_frame()`. Both then use the same fd and the same
   mmap'd buffer pool. `g_ctrl_lock` in `v4l2_capture.c` guards only the control get/set path — **the
   capture path has no mutex at all**. The log shows it plainly: two `requested rows=` lines 3s apart
   (64600 rows then 1700 rows), and only **one** completion (`wait=19485 discarded=4`) for the two of
   them. Which thread received which frame is undefined. This is a data race, not just latency.

3. **Even a correct software abort would not fix the delay.** The sensor is mid-frame at a 19s frame
   period; writing a shorter exposure and blanking does not shorten the frame already in progress,
   because the period only takes effect at the next frame boundary. The 0.5s frame genuinely cannot start
   until the 19s one finishes.

Planned fix, not yet done:

- **Serialise the capture path** with a mutex so only one worker can be inside `v4l2_capture_frame()`.
  This fixes the race on its own and is worth doing regardless of the rest.
- **Make abort real** by polling in short slices and checking an atomic abort flag between them, so the
  worker unwinds promptly. Preferred over calling `STREAMOFF` from the aborting thread, which would race
  ioctls against a thread sitting in `DQBUF`.
- **Reset the sensor's frame timing on abort** — `STREAMOFF` -> apply controls -> `STREAMON`. This is the
  only part that actually makes the 0.5s exposure arrive in ~0.5s. Measure the restart cost first; it
  should be well under the old ~515ms full teardown since the buffers stay mapped.

**Open design decision:** whether to restart the stream on every shortening change, or only on an explicit
abort. Restarting always makes any long->short transition snappy but adds the restart cost to ordinary
exposure changes; restarting only on abort leaves the normal path untouched but depends on the client
actually calling `AbortExposure` (SharpCap does when the slider moves; not every client will).

Until this is done, `canabortexposure`/`canstopexposure` reporting `true` is a conformance
misrepresentation and is likely to show up in an ASCOM Conformance run.

## Where this stands (end of session, 2026-09-17)

**Working tree is clean** apart from an untracked `PCB/` directory, which is not mine and was left alone.
Row time is now measured-correct (24.51 us/row at `hts_mult=1`, 294.12 us at 12) — treat any older
"27.45 us/row" reference in this file as wrong.
The long-exposure work (raised `SC3336_VTS_MAX`, `hts_mult` row-time scaling at 12x, the scaled DQBUF
timeout, the timestamp+rolling-shutter stale-frame fix, the direct usb0 link-local assignment, the
refreshed overlay binary, and this documentation pass) is committed at the tip of `main`. All of it is
hardware-verified.

The board and the `overlay-luckfox-astroguider` copy of `alpacad` are both running the exact binary
these sources build (md5 confirmed identical on both sides), so a reflash reproduces what was tested.
Note the live SD card may still be newer than the last full `./build.sh ... firmware` output — run a
fresh full build before trusting `output/image/sd_update.img`. The board also needs a fresh
`./build.sh kernel` for the `sc3336.ko` change to reach an image (it was hot-pushed for testing).

Open next steps, roughly in order of value:

1. **Retest against a live PHD2 / N.I.N.A. / SharpCap session.** The stale-frame fix changes the capture
   path for *every* exposure, and no ASCOM client has run since. Worth confirming both that images still
   arrive correctly and that the extra frame period per exposure is acceptable in a real guiding loop.
   Also still unconfirmed from the previous session: that PHD2's camera **Settings** button lands on the
   setup page and its dew-heater toggle.
2. **Implement exposure abort properly** (see the section above). Three parts: a mutex on the capture
   path (fixes a real data race where two workers share the V4L2 fd), a genuine cancellable wait, and a
   stream restart so a shortened exposure does not have to wait out the previous long frame. Measured
   symptom: 16.6s to deliver a 0.5s exposure requested during a 19s one.
3. **Run the ASCOM Conformance tool.** Never done. It probes edge cases hand-testing does not, and the
   driver now has two devices plus the setup URLs it expects. Note it will likely flag
   `CanAbortExposure`/`CanStopExposure`, which currently report `true` without working.
4. **Binning / subframing** — the largest remaining latency lever by a wide margin (2x2 would cut the
   265ms `mg_write` to ~66ms). Blocked on a design decision, not on effort: SC3336 is Bayer so binning a
   quad mixes colour channels; the production IMX290 is mono, where it is trivial. `ReadoutModes` is the
   right ASCOM mechanism to expose it (see `alpaca/README.md`).
5. **Re-run the boot-time measurement** to check the `quiet` bootarg's real effect — build-verified only,
   never timed on hardware. The `dhcpcd`/boot-to-discoverable half of this is now **done**: measured,
   the `timeout 1` override disproved and removed, and ~9.6s cut by assigning usb0's address directly
   (see above). What remains unmeasured is `quiet`, and the *host*-side IPv4LL delay, which is additive
   and outside our control.
6. **Unpin gain.** Gain is still pinned at 128 (1x) and client gain PUTs are accepted but ignored; the
   Int16-safe 0..1000 scale and its 32x practical ceiling are already implemented behind it.
7. Optional: disable ISP/RGA/MPP/NPU/audio in **kernel Kconfig** too (they still compile, just are never
   loaded) for a further flash-size cut.
8. A custom **SPI NAND board config** for the 64-128MB deployment target — not started, independent of
   the SD_CARD debloat. This is also where Rockchip's thunderboot fast-boot feature becomes applicable.
9. Longer term, **retarget the whole stack to RV1106G3** (256MB RAM) once validated on RV1103.

Closed since this list was last rewritten, so nobody re-opens them: the SC3336 exposure ceiling question
(answered — 0.899s stock, **1.799s** after raising `SC3336_VTS_MAX` to the register maximum, and
**21.6s shipped** via `hts_mult=12`, with 93.5s available at the 52x maximum); the boot-to-discoverable
delay (measured; `timeout 1` disproved, ~9.6s cut by direct link-local assignment); the hardcoded `DQBUF_TIMEOUT_MS` ceiling (now derived from the
frame period, and the old 5s limit proven real by measurement); the stale-frame race (fixed by frame
timestamps plus one rolling-shutter discard); the `send_imagebytes()` transpose cost (fixed, then fused
into the unpack); the real-link `mg_write` measurement (265ms, ~22.5MB/s); and the SharpCap black-image
bug (the `ImageElementType` fix).

For the PCB planning that started this session — IMX327 vs IMX290 driver status, slave-mode findings from
the datasheets, and the Luckfox-stripping question — see `alpaca/README.md` and the discussion notes; the
short version is that `imx327.c` is a full Rockchip vendor driver (not a stub) with the same control model
as `sc3336.c`, and slave mode genuinely bypasses VMAX (frame period comes from external XVS), so exposure
length there has no register-width ceiling.
