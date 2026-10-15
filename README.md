# OpenAstroGuider

An astronomy guide camera that runs its own software. It serves frames to PHD2,
N.I.N.A. and SharpCap over ASCOM Alpaca, and it is growing into a standalone guider and
plate solver that a phone can drive without a PC.

The end product is a custom board: a Rockchip RV1103 with a Sony IMX327 or IMX290
sensor, SPI NAND and USB-C. Until that board exists, development runs on a
**Luckfox Pico (RV1103) with a Waveshare SC3336 camera module**. Both are prototyping
stand-ins, not the target. This repository is a fork of the
[Luckfox Pico SDK](https://github.com/LuckfoxTECH/luckfox-pico), itself based on
Rockchip's RV1103/RV1106 SDK. It is trimmed down to what a raw-capture camera needs.

## What works today

- **A camera that speaks ASCOM Alpaca, on the device.** `alpacad` is a small C server
  running on the RV1103. It covers discovery, the management API, a full `ICameraV3`
  camera, and an `ISwitchV2` dew heater. It is tested end to end from Windows with PHD2
  (real guiding), N.I.N.A. and SharpCap.
- **Raw capture, no ISP.** Frames come straight off the sensor's raw node as 10-bit data.
  All image processing is left to the client. Exposures run up to ~6.4 s on the SC3336 in
  the shipped configuration.
- **Zero-configuration USB networking.** The camera is a USB RNDIS gadget, which Windows
  10/11 drives without an installed driver. It gives itself a stable IPv4 link-local
  address derived from the chip serial, so Windows and macOS need no setup.
- **A small image.** The stock 199 MB rootfs is cut to ~43 MB, with only the kernel modules
  the raw path needs loaded at boot. The whole SD image is ~69 MB.
- **On-device guiding, partly.** Star detection, the guide algorithms and calibration are
  ported from PHD2 and run inside `alpacad`. A PHD2-style web page is served at `/guide`.
  Calibration is verified against a simulated mount. **Guide output to a real mount (ST-4 or
  LX200) is not written yet.**
- **Hinted plate solving, as a prototype.** ASTAP's solver core is ported to C and
  benchmarked on the RV1103 at ~60 ms and 0.64 MB per solve, against a synthetic sky. Not yet
  integrated or tried on real sky.

## Where things are

| Path | What |
|---|---|
| [`alpaca/`](alpaca/) | `alpacad`, the on-device Alpaca server, with the guide loop in `alpaca/src/guide/` and the web pages in `alpaca/web/`. Start at [`alpaca/README.md`](alpaca/README.md). |
| [`Guiding/`](Guiding/) | On-device guiding: feasibility and measurements, roadmap, UI porting plan. |
| [`PlateSolving/`](PlateSolving/) | Hinted plate solving: feasibility, the solver port, tests, and the standalone/mount-control design. Start at [`PlateSolving/HANDOFF.md`](PlateSolving/HANDOFF.md). |
| `project/cfg/BoardConfig_IPC/` | Board configuration. The active one is `BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico-IPC.mk`; this project's files are `overlay/overlay-luckfox-astroguider/`, `luckfox-astroguider-oem-pre.sh` and the tracked `luckfox_pico_defconfig`. |
| `sysdrv/`, `media/`, `tools/` | The vendor SDK: U-Boot, kernel, Buildroot, Rockchip media libraries, toolchain. |
| [`CLAUDE.md`](CLAUDE.md) | The detailed engineering log: every decision, measurement and correction, dated. |
| [`README_LUCKFOX.md`](README_LUCKFOX.md) | The original Luckfox SDK README, kept for its `build.sh` reference. |

## Building

On Ubuntu 22.04, with the dependencies from [`README_LUCKFOX.md`](README_LUCKFOX.md):

```shell
# once per shell
cd tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/ && source env_install_toolchain.sh && cd -

./build.sh lunch      # RV1103_Luckfox_Pico -> SD_CARD -> Buildroot (already selected in this repo)
./build.sh            # full build
```

The image to flash is **`output/image/sd_update.img`**. A bare `./build.sh` also archives a
dated copy under `IMAGE/`; flash from `output/image/` so you never pick up a stale one.

**Flashing:** the Luckfox Pico used here has no onboard flash, so it cannot be flashed over
USB. Write `sd_update.img` to a microSD card with `dd`, balenaEtcher, or the SD Card tab of
Rockchip's SocToolKit. (The custom board has SPI NAND and will restore USB flashing.)

**Faster iteration on `alpacad` alone:** cross-compile in `alpaca/`, then `adb push` the
binary to `/usr/bin/alpacad` and restart it. Run `sync` on the device afterwards: the board
is bus-powered, so unplugging it is an unclean power cut, and an unsynced push can come back
as an empty file. The full workflow and its pitfalls are in `CLAUDE.md`.

Host-side tests, no board needed:

```shell
make -C alpaca/src/guide test       # calibration against a simulated mount
make -C PlateSolving/src test       # plate solver against a synthetic sky
```

## Connecting

Plug the camera into a PC over USB. About 15 s after power-up it is reachable at a
`169.254.x.y` address, and ASCOM Alpaca discovery finds it as **"OpenAstroGuider Camera"**
on port 11111.

- **PHD2, N.I.N.A., SharpCap:** choose the Alpaca camera; on Windows, through the ASCOM
  Platform.
- **Setup page and dew heater:** `http://<address>:11111/setup`. This is also where a
  client's camera "Settings" button lands.
- **Guider page:** `http://<address>:11111/guide`.
- **Shell:** `adb shell`. SSH and telnet are disabled on purpose.
- **Linux hosts:** NetworkManager tries DHCP on the link forever. Switch the connection to
  link-local once:
  `nmcli con mod "<connection>" ipv4.method link-local && nmcli con up "<connection>"`.

## Status and next steps

The live to-do list is "Open to-dos" in [`CLAUDE.md`](CLAUDE.md). In short:

1. Mount output for the guider (ST-4 over GPIO, LX200), then a night on a real mount.
2. Plate solving on real sky frames, then integration into `alpacad`.
3. Standalone operation from a phone over Wi-Fi: goto, solve, sync, polar alignment. See
   [`PlateSolving/STANDALONE.md`](PlateSolving/STANDALONE.md).
4. The custom board: RV1103G1 with an IMX327/IMX290 and SPI NAND, in design.

## Credits and licences

This repository mixes licences.

- **Luckfox Pico SDK and Rockchip SDK:** the base of this fork, under their respective
  licences, including GPL for the Linux kernel, U-Boot and Buildroot.
- **[PHD2](https://github.com/OpenPHDGuiding/phd2)** (BSD 3-clause): the star detection,
  guide algorithms, calibration and interface ported in `alpaca/src/guide/` and
  `alpaca/web/`. Notices are kept in the source.
- **[ASTAP](https://sourceforge.net/p/astap-program/)** by Han Kleijn (MPL-2.0): the
  plate-solving core ported in `PlateSolving/src/astap_solve.c` and `astap_db.c`. Those
  files remain MPL-2.0.
- **[CivetWeb](https://github.com/civetweb/civetweb)** (MIT): the HTTP server, vendored in
  `alpaca/third_party/`.

## Notice

Do not copy or extract this source tree on Windows before building on Linux. It can strip
executable bits and break the symlinks the build depends on.
