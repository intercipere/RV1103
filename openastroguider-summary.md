# OpenAstroGuider — Project Summary & Goals

## The project
Building a custom astronomy guide camera: a mono IMX290 sensor on a custom PCB around a Rockchip RV1106G3 SoC, replacing an off-the-shelf colour IMX290 USB module currently capped at 0.5s exposures. Goal: long exposures without relying on PHD2 live stacking.

Prototyping platform: Luckfox Pico (RV1103, 64MB RAM) + Waveshare SC3336 camera module, standing in for the IMX290 until the PCB exists. Final board target: RV1106G3 with 256MB RAM — the RV1103's 64MB proved too tight for buffering multiple full-resolution frames.

## Repo
Firmware forked to `https://github.com/intercipere/RV1103` (Luckfox Pico Buildroot SDK fork, based on `LuckfoxTECH/luckfox-pico`). Ubuntu box set up as dedicated build environment. Claude Code installed there and working directly on the local clone — no GitHub access needed for Claude Code itself, git push uses your own `gh auth login` / SSH credentials.

## Architectural decisions made so far
- **Raw-only capture path, no ISP.** Bypass rkaiq/rockit entirely — go straight off the rkcif raw node (`/dev/video0`), controls via the sensor subdev (`/dev/v4l-subdev2`).
- **No live processing on-device** — raw 10-bit Bayer frames only, processing happens on the PC.
- **Transport: HTTP/Alpaca protocol over a USB gadget**, not UVC. Landed on **RNDIS** (not ECM) for zero-driver compatibility with Windows 10/11 — ECM would need a driver install on Windows, RNDIS is built in.
- **Output format: FITS**, for compatibility with astronomy capture/stacking software.
- **Exposure/blanking are measured in rows, not seconds** (~27.45µs/row on this sensor/clock config). Exposure is capped by frame length (1296 rows + vertical blanking), so `vertical_blanking` has to be raised before a long exposure will apply.

## Sensor/driver facts (validated empirically, not from docs)
- RV1103's compact RAW10 packing (`bits_lsb`) is **little-endian bit order within each byte, and little-endian bit assembly within each pixel's 10 bits** — i.e. `np.unpackbits(..., bitorder="little")` with ascending powers of two. Every big-endian attempt produced pixel-scrambled noise that looked identical across all four Bayer phases, which is what made this hard to diagnose.
- `/tmp` on the board is a RAM disk on a 64MB device — multi-frame captures must be pulled in batches, not buffered fully on-device.
- SC3336 is a surveillance sensor with the same architecture as IMX290: exposure is capped by frame length, and the driver likely enforces a `vblank` ceiling below what the silicon actually allows. Pushing that ceiling on the SC3336 is the validation step for whether the platform will tolerate the long exposures the real IMX290 build needs.
- Colour rendering solved via OpenCV `cv2.cvtColor(..., cv2.COLOR_BayerRG2RGB_EA)` + 16-bit PNG output (this is now in `grab.py`).

## Debloating (SPI NAND deployment target, 64–128MB)
Stock Luckfox image is ~116MB (86.7MB rootfs + 29.4MB `/oem`). Biggest cut targets identified:
- **Python 3.11 + `libpython3.11.so`** — 37.6MB
- **Entire `/oem` ISP/media stack** (librkaiq, librockit, MPP, RGA, NPU runtime) — 29.4MB, all unneeded since the raw-only path bypasses the ISP
- **OpenSSH + GnuTLS** — 7.5MB

Board config direction: use `[11] custom` in the lunch menu. Two candidate starting points to compare line-by-line before editing:
```
project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico-IPC.mk
project/cfg/BoardConfig_IPC/BoardConfig-SPI_NAND-Busybox-RV1106_Luckfox_Pico_Pro_Max-IPC_FASTBOOT.mk
```
The second is a Rockchip `IPC_FASTBOOT` minimal/Busybox config (currently only exists for RV1106 Ultra/Pro Max, not plain RV1103) — adapting it to RV1103 with SPI NAND is likely less work than stripping the first down from scratch, since the Busybox/fastboot decisions are already made there. Partition layout (where the 64MB target gets decided) lives in these same files.

## USB gadget / networking (in progress, not yet implemented)
- Check board DTS for the OTG USB node: needs `dr_mode = "peripheral"` (or `"otg"` negotiating peripheral). If currently `"host"`, that explains why comms are adb-only right now.
- Kernel config needs `CONFIG_USB_CONFIGFS` + `CONFIG_USB_CONFIGFS_RNDIS` (add `CONFIG_USB_CONFIGFS_ECM` too if Linux/Mac support is wanted later).
- **Extend the existing USB composite gadget, don't replace it** — the board already exposes something over USB (mass storage for flashing / adb function) via a configfs script, likely under `/etc/init.d/`. Add the RNDIS function to that same gadget rather than standing up a competing one.
- Give `usb0` a static IP once it exists (e.g. `192.168.55.1/24`) in that same init script.
- Next layer up: small C or Python HTTP server on-device exposing capture as a couple of routes, returning raw frame + JSON metadata. On Windows side, nothing built yet — just confirming `curl`/browser can trigger a capture is the first milestone, since that alone replaces the adb-only workflow.

## PC-side tooling already written
`grab.py` (the working capture/unpack/preview script — sets sensor controls, captures N frames, pulls raw over adb/ssh, unpacks RAW10, writes PNG), plus diagnostic/exploration scripts from earlier in the process: `isp_grab.py`, `noise.py`, `bayer_phase.py`, `planes.py`, `period.py`, `bitorder.py`, `to_fits.py`.

## Working style / preferences to carry into Claude Code sessions
- Prefer empirical validation over guessing — measure before cutting, use dark frames as ground truth.
- Push back on unverified fixes offered as if they were definitive; want the actual measurement or check, not a plausible-sounding correction.
- A `CLAUDE.md` was created in the repo capturing the hard-won technical facts above (packing format, transport decisions, debloat targets) for future Claude Code sessions to read on start — worth confirming it's current before the next SDK session.

## Open next steps
1. Confirm/push the SC3336 exposure ceiling as far as it goes (the concrete go/no-go signal for long exposures on this platform).
2. Compare the two BoardConfig `.mk` files and decide the SPI NAND / RV1103 debloat starting point.
3. USB gadget: locate the existing configfs script, add RNDIS function, verify `usb0` comes up with a static IP.
4. Minimal on-device HTTP server exposing capture — first milestone is `curl` working from a PC, killing the adb-only workflow.
5. Longer term: retarget the whole stack to RV1106G3 (256MB RAM) once validated on RV1103.
