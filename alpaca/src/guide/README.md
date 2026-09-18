# Guiding core

Star detection, centroid tracking and the guide algorithms, ported from
[PHD2](https://github.com/OpenPHDGuiding/phd2) (BSD 3-clause, Copyright (c) 2013-2019 Open
PHD Guiding development team). Feasibility analysis and the reasoning behind this subset is
in `Guiding/README.md` at the repo root.

**Wired into `alpacad` and serving.** The loop runs inside the daemon, shares the streaming
V4L2 device with the Alpaca exposure worker, and serves a page at `/guide`. What is still
missing is the other half: there is no calibration and no ST-4 output, so corrections are
computed and displayed but never sent to a mount.

| File | What |
|---|---|
| `guide_star.h/.c` | `guide_star_find()` (PHD2 `Star::Find`) and `guide_star_autofind()` (PHD2 `Star::AutoFind`, restructured) |
| `guide_algo.h/.c` | hysteresis, lowpass, resist-switch, identity |
| `guide_source.h/.c` | frame sources: live camera over Alpaca, or a simulated star field |
| `guide_source_v4l2.c` | in-daemon source: captures via V4L2 under `v4l2_exposure_lock()` |
| `guide_loop.c/.h` | the loop thread: acquire, select, track, run the algorithms, publish |
| `guide_api.c/.h` | `/guide` page plus `/guide/state`, `/guide/preview`, `/guide/control` |
| `guide_sim.c` | test harness: runs the whole pipeline against either source |
| `grab_frame.py` | pulls a frame off the camera over Alpaca ImageBytes and writes raw uint16 |

## Two deliberate departures from PHD2

**AutoFind is banded and single-pass.** PHD2 median-filters the frame, converts to float,
downsamples and convolves, holding two whole float images. At 2304x1296 that peaks at
35.8 MB and **the kernel OOM-kills it on this board** — measured, `total-vm:35776kB`. Here
the work is identical but done in horizontal bands gathered and downsampled straight out of
the source image, so no full-size intermediate ever exists: peak scratch is two float bands,
0.69–2.76 MB depending on downsample.

It is also one pass rather than two. PHD2 computes the convolution's global mean and stdev
over the finished image, then rescans scoring `h = (val - local_mean) / global_stdev`.
Since `global_stdev` is a single scalar applied uniformly it cannot change the *ranking*, so
the top-N list is collected by `val - local_mean` as bands are produced and the threshold
applied once at the end. Same result.

**Images are addressed through explicit strides.** `alpacad` stores frames in ImageBytes
wire order (`[x * height + y]`, X outer), not row-major. Transposing 6 MB per frame to
satisfy a convention would cost far more than the guiding itself, so `guide_image_t` carries
`x_stride`/`y_stride` — `{1, width}` for row-major, `{height, 1}` for alpacad's layout.

PHD2's numeric constants are left alone: annulus radii 7 and 12, the 3-sigma threshold, nine
clipping iterations, 0.5 e-/ADU nominal gain, the 0.1 detection threshold, `min_move`,
hysteresis 0.10, aggression 0.70. Those are what a decade of real guiding tuned, and this
port has no evidence to overrule them with.

## Inside alpacad

`guide_loop_init()` starts the thread at daemon startup, but it idles until something asks
it to run — an installation that only ever uses the Alpaca camera pays nothing for it.

Endpoints, deliberately outside `/api/v1` because this is not an ASCOM interface and ASCOM
has no guider device type to conform to:

```
GET  /guide            the page
GET  /guide/state      JSON: state, star, error history, RMS, timings
GET  /guide/preview    downsampled 8-bit greyscale, geometry in X-Preview-* headers
PUT  /guide/control    start|stop|reselect|source|exposure|minmove|algo
```

### The page never sees a full frame

`/guide/preview` sends a **downsampled 8-bit** buffer: 576x324, 182 kB. The sensor's actual
frame is 2304x1296 at 10 bits -- 5.97 MB -- and it never leaves the device for this page.
That is a 33x reduction, and it is the difference between a preview that costs ~8 ms on the
link and one that costs 265 ms.

(The Alpaca `imagearray` endpoint *does* serve the full frame, at full depth. That is for
imaging clients. The guider page is not one.)

There is also **no encoder**. No zlib, libjpeg or libpng in the rootfs, and the hardware
JPEG encoder is behind the stripped-out MPP stack -- so the page gets raw bytes and renders
them with canvas `putImageData`.

The stretch is display-only and had to be got right twice. A min/max stretch puts the white
point on the brightest hot pixel and renders stars near black. A median black point clips
half the frame to zero *by construction* -- fine for a star field, which is nearly all sky,
but it turns any scene with shadow detail into half solid black. It is now the 5th
percentile to the 99.99th (stars are far rarer than the 99.9th percentile implies), with a
minimum span so a flat frame stays dark instead of being amplified into full-contrast
static. Nothing downstream sees these values; the centroid never touches them.

**`v4l2_exposure_lock()` is new and load-bearing.** The atomic unit is the whole sequence
"write controls -> sample the settle reference -> capture", not just the capture:
`g_ctrl_lock` only guarded individual ioctls. With two callers now — the Alpaca exposure
worker and the guide loop — sharing one fd and one buffer pool, which thread received which
frame was otherwise undefined. This also closes the concurrent-`exposure_worker` race in the
project's open issues: a second `startexposure` arriving mid-capture now waits.

Verified on the board: four Alpaca `ImageBytes` captures (5,972,012 bytes each, correct
headers) served **while the guide loop was running on the camera source**, no corruption and
no crash. The guide loop's own frame time rises to ~2.1 s under that contention, which is
the serialisation working rather than a fault.

### Memory, again

The first version retained its frame between iterations. Running it alongside an Alpaca
client OOM-killed the daemon (`total-vm:49020kB`) — two 6 MB frames plus a third being
captured, against ~8 MB of real headroom. `guide_source_release()` now frees the frame as
soon as the centroid and preview have been extracted, and `/guide/preview` allocates exactly
the preview size instead of a speculative megabyte. After that, guide loop plus concurrent
Alpaca captures peaks at **VmHWM 25.8 MB** and stays up.

## Frame sources

`--source camera` and `--source sim` are interchangeable — the loop above them cannot tell
which it is on. What differs is what can be checked.

**`camera`** fetches live frames from the running `alpacad` over HTTP on localhost. Going
through the Alpaca API rather than opening `/dev/video0` is what makes it work at all while
the daemon is serving PHD2: the V4L2 device has one owner and `alpacad` already is it. There
is no ground truth here — a real frame tells you where a star *appears*, never where it
actually was — so the centroid can only be checked for self-consistency.

**`sim`** draws stars at exactly known sub-pixel positions, drifting the way a mount does,
over a **real** frame from this sensor. It is the only configuration that can score the
centroid against truth, and the only one that works in daylight, indoors, or with no mount.

Neither replaces the other: use `sim` to know the maths is right, `camera` to know it
survives this sensor.

## The camera source does not fit on this board, and that settles an architecture question

Measured: `alpacad` retains a whole 5.97 MB frame after every exposure — **VmRSS 12.5 MB at
rest, 18.4 MB after one exposure** — and holds two transiently while capturing the next. A
separate process that also holds a frame needs room for roughly three frames between them.
On the RV1103's 32 MB there is not.

This is not theoretical. Pulling frames from a separate process **OOM-killed `alpacad` three
times** during development, and the kernel picks the daemon, not the test that caused it
(`civetweb-worker invoked oom-killer` ... `Killed process 342 (alpacad)`). `guide_source.c`
now refuses to open a camera source when `MemAvailable` is too low, rather than taking the
daemon down.

The conclusion is useful rather than annoying: **the real guide loop belongs inside
`alpacad`, sharing the frame it already captured**, not in a separate process pulling frames
over HTTP. That is also strictly better — no HTTP round trip, no second copy, no duplicated
6 MB. The camera source keeps its value as the reference for what the in-daemon source must
do, and runs fine from a host or on the 256 MB production part.

## Testing it

There are no stars in a daylight frame, so the harness injects Gaussian stars at known
sub-pixel positions into a **real** frame off the sensor. What the real frame contributes is
this sensor's actual read noise, bias pedestal, hot pixels and fixed-pattern structure —
exactly what the background estimator and detection threshold have to survive, and exactly
what a fully synthetic frame would flatter. The injected positions give exact ground truth
for the centroid. Point it at a real star field with `--no-inject` once there is one.

```
# capture a base frame from the running daemon (adb forward tcp:11111 tcp:11111 first)
python3 grab_frame.py 0.0003 frame_dark.u16

# host, simulated stars over that real frame
make && ./guide_sim --source sim --base frame_dark.u16 --frames 20

# host, live frames straight off the camera
./guide_sim --source camera --exposure 0.1 --frames 5

# board (simulated only -- see the memory section above)
make CC=arm-rockchip830-linux-uclibcgnueabihf-gcc \
     CFLAGS='-O2 -Wall -Wextra -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard'
adb push guide_sim /tmp/ && adb push frame_dark.u16 /userdata/
adb shell '/tmp/guide_sim --source sim --base /userdata/frame_dark.u16 --frames 20'
```

Put the frame on `/userdata`, not `/tmp` — `/tmp` is a RAM disk, and a 6 MB frame there
comes straight out of the memory the test is trying to measure.

## Measured on the board (RV1103, 1 core @ 1104 MHz)

```
guide_star_find      0.159-0.182 ms per frame
centroid error       mean 0.0057-0.0092 px, worst 0.0109-0.0141 px, 0 lost

autofind, downsample=1   2912 ms   2.76 MB scratch
autofind, downsample=2    872 ms   1.38 MB scratch
autofind, downsample=4    321 ms   0.69 MB scratch
```

All three downsample settings find the same stars with the same centroid accuracy —
detection is downsampled, but the centroid is always computed at full resolution. **4 is a
good default**: 321 ms once per session, and it is the cheapest in both time and memory.

Board and host agree to every printed digit, which is a useful correctness signal for a
port that changed the loop structure this much.

Against a ~0.4–4 s frame period, 0.182 ms of guiding per frame is a duty cycle under 0.05%.

## Audit: what is a real port, and what was written by hand

Prompted by the obvious question -- if PHD2 already has this, why write it? The honest
inventory, after checking each against the source:

**Faithful ports** (PHD2 logic, PHD2 constants, wx shell stripped):
`Star::Find`, `hfr()`, `psf_conv`, and the hysteresis, lowpass and resist-switch algorithms.

**Written here, because PHD2 has no equivalent:** the frame sources, the HTTP layer and the
page (PHD2 is an Alpaca *client*, not a server), and the loop that drives them.

**Written here where PHD2 does have something** -- and this is where one real defect was:

- **Resist switch was not PHD2's algorithm at all.** The first version was a hand-written
  reversal counter that shared only the name. PHD2's keeps a 10-sample window, sums the signs
  of entries above `min_move`, and will only reverse when that sum reaches +/-3 *and* the
  newest three samples are worse than the oldest three -- plus a fast-switch path for
  excursions over `3 * min_move`, and a veto when the current side disagrees with the input
  (overshoot). None of that was present. Now ported properly and checked against hand-traced
  cases: below-min-move vetoed, a lone reversal vetoed, a reversal that is already improving
  vetoed, a large excursion switching immediately.
- **Algorithm defaults were global, not per-algorithm.** PHD2 sets `aggression` to 0.7 for
  hysteresis (which is already damping via the previous move) and 1.0 for resist-switch.
  Using one value for both quietly under-corrected the Dec axis by 30%. Fixed.
- `gather_band` replaces PHD2's `Median3` + `Downsample` + float conversion. The downsample
  is equivalent to PHD2's box average; the hot-pixel filter is a median-of-three along x
  rather than PHD2's full 3x3 `median9` network, which is weaker. Deliberate, and cheap to
  upgrade if hot pixels ever cause a false detection.
- The preview stretch is percentile-based; PHD2 takes min/max of a median-filtered frame and
  applies a user gamma (default 1.0, i.e. linear -- so the default behaviour matches). A
  gamma control is a missing *feature*, not a bug.
- `peak_insert` and the candidate merge are a hand-rolled top-N where PHD2 uses
  `std::set<Peak>`. Equivalent behaviour; C has no std::set.

**Lesson for the rest of the port:** read the source before writing the thing, even when the
algorithm "obviously" does what its name says. Two of the three algorithms written from a
description were wrong in ways that would have shown up only as poor guiding on sky.

## Two things the page got wrong at first

Both found by using it rather than by reading it, and worth recording because neither
looked like the bug it was.

**"The camera output is black."** The preview endpoint 503s until the loop has produced a
frame, and the page's fetch handler silently did nothing on failure -- so on a freshly
booted daemon the canvas was simply never drawn. It was not a black *image*, it was an
undrawn canvas, and nothing said so. There is now an explicit empty state over the frame.

**"Re-select star does nothing."** It worked, and that was the problem: re-running detection
is deterministic, so it returned the same brightest star every time. The button is now
**Next star** and advances through the candidate list, and the header reports `star 3 of 6`
so the selection is visible.

## What of PHD2 is actually here

Short version: the inner measurement loop, and almost nothing else. PHD2's `GUIDER_STATE`
has eight states; this has seven, and the three calibration ones have no counterpart
because there is no calibration.

| PHD2 | Here |
|---|---|
| Loop Exposures | **yes** |
| Auto-select Star | **yes** — "Next star", cycles the candidate list |
| Guide | **partial** — tracks and computes corrections; does not calibrate, does not move a mount |
| Stop | **yes** |
| Hysteresis / Lowpass / ResistSwitch / Identity | **yes**, 4 of PHD2's 7 |
| Lowpass2, ZFilter, Gaussian Process (PPEC) | no |
| Calibration (all of it: assistant, review, restore, manual entry, flip, clear) | no |
| Mount output — ST-4, PulseGuide, Manual Guide | no |
| Dithering, settle handshake | no |
| Dark library / bad-pixel map | no |
| Backlash compensation | no |
| Dec guide mode (auto / north / south / off) | no |
| Multi-star guiding | detects several, tracks one |
| Star mass-change rejection (PHD2's `STAR_MASSCHANGE`) | no |
| Lock position, lock shift, comet tracking | no |
| Subframes, binning | no |
| Guiding Assistant, Drift Align, Polar Drift Align, Star-Cross | no |
| Event server on :4400 (what N.I.N.A. and SGP talk to) | no |
| Guide log | no |

The two that block everything else are **calibration** and **mount output**. Without
calibration the axes are camera x/y, not RA/Dec, and the numbers the page prints are in
pixels for that reason. Without mount output the loop measures guiding rather than
performing it.

### Looping vs guiding

PHD2 separates these and the first version here did not, which collapsed a genuinely
distinct mode. `Loop` acquires frames and updates the preview with no star lock and no
corrections -- it is how you frame and focus the guide scope before choosing anything. With
a star selected it keeps measuring it (HFD is the focus signal) but still computes nothing.
`Guide` auto-selects if no star is chosen, so the one-click path still works, then tracks.

States: `stopped` -> `looping` -> `selecting` -> `selected` -> `guiding`, plus `lost` and
`error`. PHD2's `CALIBRATING_PRIMARY`, `CALIBRATING_SECONDARY` and `CALIBRATED` sit between
`selected` and `guiding` and are the gap.

## Not done yet

- **Calibration.** Nothing maps pixels to mount axes, so the RA/Dec labels on the page are
  really "x" and "y" in camera coordinates. PHD2's `mount.cpp` transform plus the
  pulse-and-measure sequence is the piece to take, and the one most worth taking verbatim.
- **ST-4 output.** Corrections are computed and shown but go nowhere. Until this and
  calibration exist, the loop measures guiding rather than performing it.
- Multi-star guiding, backlash compensation, dec guide modes, the Gaussian-process algorithm.
- `guide_star_find()` has no dark-frame or defect-map input yet. On this sensor the real
  frame already shows hot pixels; `gather_band()` medians them out for *detection*, but
  tracking currently relies on the smoothed peak search to not latch onto one.
