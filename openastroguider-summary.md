# OpenAstroGuider — Project Summary & Goals

*High-level orientation. The detailed, hard-won technical record lives in `CLAUDE.md` (whole-project) and
`alpaca/README.md` (the on-device driver). Where they disagree with this file, they are right — this one
is the map, not the territory. Last reconciled against reality **2026-09-16**.*

## The project
Building a custom astronomy guide camera: a mono IMX290 sensor on a custom PCB around a Rockchip RV1106G3
SoC, replacing an off-the-shelf colour IMX290 USB module capped at 0.5s exposures. Goal: long exposures
without relying on PHD2 live stacking.

Prototyping platform: Luckfox Pico (RV1103) + Waveshare SC3336 camera module, standing in for the IMX290
until the PCB exists. **Both are temporary stand-ins, not the production target.** The SC3336's exposure
ceiling is an accepted prototyping limitation, not a bug to chase. Final target: RV1106G3 with 256MB RAM.

**RAM correction:** the RV1103 has 64MB physical, but 24MB is reserved for the CMA (sized for the stock
ISP/RGA/MPP/rockit pipeline this project has stripped out), leaving `MemTotal`/`malloc()` with **~32MB**.
Discovered the hard way, via a real OOM crash-reboot while buffering an 11MB HTTP response.

## Repo
Firmware forked to `https://github.com/intercipere/RV1103` (Luckfox Pico Buildroot SDK fork, based on
`LuckfoxTECH/luckfox-pico`). Ubuntu box set up as the dedicated build environment, with Claude Code
working directly on the local clone.

## Architectural decisions
- **Raw-only capture, no ISP.** Straight off the rkcif raw node (`/dev/video0`), sensor controls via the
  subdev (`/dev/v4l-subdev2`). rkaiq/rockit bypassed entirely.
- **No image processing on-device** — no dark/flat calibration, debayering or stacking. Raw frames go to
  the PC. This matches Alpaca's own design, where the driver hands back raw data.
- **Transport: HTTP/Alpaca over a USB gadget, RNDIS** (not ECM, not UVC) — RNDIS needs no driver install
  on Windows 10/11.
- **Exposure and blanking are measured in rows, not seconds** (~27.45µs/row here). Exposure is capped by
  frame length (1296 rows + vertical blanking), so `vertical_blanking` must be raised before a longer
  exposure will apply.
- FITS was the original output-format decision; in practice the driver serves ASCOM `ImageArray` /
  `ImageBytes` and the client writes FITS.

## Sensor/driver facts (validated empirically, not from docs)
- RV1103's compact RAW10 packing (`bits_lsb`) is **little-endian bit order within each byte AND
  little-endian bit assembly within each pixel's 10 bits**. Big-endian attempts produce pixel-scrambled
  noise that looks identical across all four Bayer phases — that signature means this bug, not a phase
  offset.
- The SC3336 is **BGGR**, confirmed by debayering one frame all four ways.
- **SC3336 exposure ceiling: ~0.899s** — answered, and the driver's `ExposureMax` computes it live from
  the V4L2 control ranges rather than hardcoding it.
- `/tmp` on the board is a RAM disk on a ~32MB device — never buffer multi-frame captures on-device.

## What is built and working
- **Debloated image**: 199MB → 43.3MB rootfs, no ISP/RGA/MPP/NPU/audio/rkipc running, only the four
  kernel modules the raw path needs. Partition table resized to fit a 128MB card (~69MB image).
- **`alpacad`**, a C Alpaca server running *on the camera*, baked into the image and surviving reboots:
  discovery, management API, a full `ICameraV3` camera, and an `ISwitchV2` dew heater. Confirmed working
  end-to-end from Windows with **PHD2 (real guiding), N.I.N.A. and SharpCap**.
- **Zero-configuration networking**: usb0 self-assigns an IPv4 link-local address and the gadget MAC is
  derived from the SoC serial, so the host interface name is stable. Windows/macOS need no setup; Linux
  needs one `nmcli` line (see `CLAUDE.md`).
- **A browser setup page** at `/`, `/setup` and every `/setup/v1/<type>/<n>/setup` — what a client's
  "Settings" button actually opens for an Alpaca device, and the only way to reach the dew heater from
  PHD2, which has no ASCOM Switch UI.
- Per-frame latency worked down to **~0.49s at a 0.02s exposure** over the real link, with the remaining
  budget dominated by the 265ms network write of a 5.97MB frame.

## PC-side tooling
`grab.py` was the sensor-bringup tool and is **no longer maintained** — the project moved to the
on-device driver. It and its diagnostic siblings (`isp_grab.py`, `noise.py`, `bayer_phase.py`,
`planes.py`, `period.py`, `bitorder.py`, `to_fits.py`) can be left to bit-rot.

## Working style / preferences
- Prefer empirical validation over guessing — measure before cutting, use dark frames as ground truth.
- Push back on unverified fixes offered as if definitive; want the actual measurement or check, not a
  plausible-sounding correction. This project has a documented history of confident wrong theories
  (three in a row on one SharpCap bug) that were only settled by reading an independent reference
  implementation and capturing a real log.

## Open next steps
See **"Where this stands"** at the end of `CLAUDE.md` for the current, reconciled list. In brief: retest
against live ASCOM clients, run the ASCOM Conformance tool, then binning/subframing (the largest
remaining latency lever), and eventually the SPI NAND board config and the RV1106G3 retarget.
