# Guiding core

Star detection, centroid tracking, the guide algorithms and calibration, ported from
[PHD2](https://github.com/OpenPHDGuiding/phd2) (BSD 3-clause, Copyright (c) 2013-2019 Open
PHD Guiding development team). Feasibility analysis and the reasoning behind this subset is
in `Guiding/README.md` at the repo root.

**Wired into `alpacad` and serving.** The loop runs inside the daemon, shares the streaming
V4L2 device with the Alpaca exposure worker, and serves a page at `/guide`. With a mount
connected it calibrates (PHD2's sequence) and then guides, sending pulses. **The only mount
backend is a simulated one**, which comes with the simulated frame source — so the whole
select → calibrate → guide cycle runs and is tested on the bench, but on the real camera
there is still nothing to send pulses to. ST-4 and LX200 backends are the missing piece.

| File | What |
|---|---|
| `guide_star.h/.c` | `guide_star_find()` (PHD2 `Star::Find`) and `guide_star_autofind()` (PHD2 `Star::AutoFind`, restructured) |
| `guide_algo.h/.c` | hysteresis, lowpass, resist-switch, identity |
| `guide_cal.h/.c` | calibration state machine, camera/mount transforms, guide step (PHD2 `scope.cpp`/`mount.cpp`) |
| `guide_mount.h/.c` | the mount as guiding sees it: `guide_mount_pulse(dir, ms)`, backends behind it |
| `guide_source.h/.c` | frame sources: live camera over Alpaca, or a simulated star field |
| `guide_source_v4l2.c` | in-daemon source: captures via V4L2 under `v4l2_exposure_lock()` |
| `guide_loop.c/.h` | the loop thread: acquire, select, track, run the algorithms, publish |
| `guide_api.c/.h` | `/guide` page plus `/guide/state`, `/guide/preview`, `/guide/control` |
| `guide_sim.c` | test harness: runs the whole pipeline against either source |
| `guide_cal_test.c` | `make test`: calibration and closed-loop guiding against the simulated mount, pass/fail |
| `guide_settings.h/.c` | settings under PHD2's config keys, persisted; `/guide/settings` |
| `guide_serve.c` | `make guide_serve [--web ../../web]`: the real page and loop on a host, no board needed |

The browser side lives in `alpaca/web/guide/` (not here): plain HTML/CSS/JS, embedded into the
binary at build time. See `Guiding/UI_PORTING_PLAN.md` for how it is organised and why.
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
GET  /guide               the page (served from web/, see web.c)
GET  /ui/<path>           page assets, pre-gzipped, ETag
GET  /guide/state?since=N JSON: state, star, calibration, image scale, and the guide steps
                          after seq N (PHD2's S_HISTORY fields; 400 kept)
GET  /guide/preview       downsampled 8-bit greyscale; overlay geometry in X-* headers
GET  /guide/star          41x41 raw uint16 around the star, for the star profile
GET  /guide/settings      implemented settings by PHD2 key; PUT key=&value= changes one
PUT  /guide/control       loop|start|stop|autoselect|reselect|calibrate|clearcal|decmode|
                          source|exposure|minmove|algo
```

RMS is no longer computed on the device: PHD2 reports the population sigma over the graph's
chosen x-length, which is a display choice, so the page computes it from the history. The
device used to report root-mean-square about zero over a fixed 100 frames, which a steady
offset inflates and PHD2's figure does not.

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

## Overlay, and why frames take 0.4 s

The preview draws PHD2's two markers, now with PHD2's own colours and styles (see
`web/guide/js/panel-image.js`): a **box around the guide star** the size of the search region,
and a full-width **crosshair at the lock position** -- dotted yellow while calibrating, solid
green while guiding. Both are drawn **in the browser, on a separate canvas layer at screen
resolution**; the device only sends their coordinates. They used to be drawn into the
576x324 preview pixels, which the browser then scaled up about 3x -- that, not any compression
(there is no encoder on the device), is what made them look smeared. The real search region is 31 px, under 8 preview pixels,
so both are held to a readable minimum on screen. Candidates are not marked. The positions
travel in `/guide/preview`'s own headers (`X-Star`, `X-Lock`, `X-Frame`, `X-Search`,
`X-Preview-Scale`), captured in the same locked swap as the pixels, so the box can never be
drawn on a different frame from the one it describes. The preview is now double-buffered for
the same reason; it used to be built in place while an HTTP thread could be copying it.

The page polls `/guide/state` every 100 ms (was 1 s) and fetches the 182 kB preview only
when the frame number has changed, one request in flight at a time. The old 1 s poll alone
added 0-1 s of display lag.

### Frame rate: from 2.5 to 7.6 frames/s (2026-10-07, measured on the board)

"Frame acquire" read ~400 ms because that was the **sensor's minimum frame period** at
`hts_mult=12`: (1296 + 64) rows x 294 us = 400 ms. Not download time -- the loop runs inside
`alpacad`. Since guiding needs at most ~5 s exposures, `hts_mult` is now **4** (ceiling
6.42 s, shortest frame 133 ms; see the table in the overlay's `insmod_ko.sh`). Verified on the
board: `ExposureMax` 6.424 s, `ExposureMin` 98.04 us = one row.

That alone gave **5.5 frames/s, not 7.5**, at 91% CPU: the loop had become CPU-bound. The
timing added to the Detail card ("Preview build") showed why -- `build_preview()` took
**~100 ms**, more than the 72 ms unpack, because it walked the column-major frame row by row
(a 2.6 kB stride between reads). Walking it in storage order brought it to **~40 ms**, with
output byte-identical to the old loop (compared on three simulator frames), and the loop to
**7.6 frames/s at 0.1 s exposures -- the sensor's own limit**. CPU stays ~90% while looping,
now spent on more frames; idle when the loop is stopped.

What is left per frame: 72 ms unpack (RAW10 -> uint16, whole frame) and ~40 ms preview.
Further gains would mean unpacking only a subframe around the star while guiding -- the
real-sensor version of the binning/subframe lever.

Verified on the board through the real page: the box and lock crosshair draw on a live
camera frame while guiding. The red lost-star box has still not been seen.

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

Short version: the inner measurement loop, calibration, and the guide step -- driving a
simulated mount only, since no real mount backend exists yet. PHD2's `GUIDER_STATE` has
eight states; this has eight too, with `calibrating` standing in for PHD2's
`CALIBRATING_PRIMARY` (there is no secondary/AO mount) and `CALIBRATED` transient here as
it is in PHD2.

| PHD2 | Here |
|---|---|
| Loop Exposures | **yes** |
| Auto-select Star | **yes** — "Next star", cycles the candidate list |
| Guide | **yes** with a mount; measure-only (camera pixels) without one |
| Stop | **yes** |
| Hysteresis / Lowpass / ResistSwitch / Identity | **yes**, 4 of PHD2's 7 |
| Lowpass2, ZFilter, Gaussian Process (PPEC) | no |
| Calibration — the west/east/backlash/north/south/nudge sequence, transforms, clear | **yes**, tested against the simulated mount |
| Calibration — parity, Dec compensation, meridian flip, sanity check vs previous | no — needs a mount that reports position |
| Calibration — assistant, review, restore, manual entry, persistence across restarts | no |
| Mount output — simulated | **yes** |
| Mount output — ST-4, LX200, Camera `PulseGuide`, Manual Guide | no |
| Dithering, settle handshake | no |
| Dark library / bad-pixel map | no |
| Backlash compensation | no |
| Dec guide mode (auto / north / south / off) | **yes** |
| Multi-star guiding | detects several, tracks one |
| Star mass-change rejection (PHD2's `STAR_MASSCHANGE`) | no |
| Lock position, lock shift, comet tracking | no |
| Subframes, binning | no |
| Guiding Assistant, Drift Align, Polar Drift Align, Star-Cross | no |
| Event server on :4400 (what N.I.N.A. and SGP talk to) | no |
| Guide log | no |

What blocks guiding a real telescope is now only **mount output**: an ST-4 or LX200 backend
behind `guide_mount_pulse()`. Calibration exists, but has only ever calibrated the simulator.

### Looping vs guiding

PHD2 separates these and the first version here did not, which collapsed a genuinely
distinct mode. `Loop` acquires frames and updates the preview with no star lock and no
corrections -- it is how you frame and focus the guide scope before choosing anything. With
a star selected it keeps measuring it (HFD is the focus signal) but still computes nothing.
`Guide` auto-selects if no star is chosen, so the one-click path still works, then tracks.

States: `stopped` -> `looping` -> `selecting` -> `selected` -> `calibrating` -> `guiding`,
plus `lost` and `error`. `calibrating` is PHD2's `CALIBRATING_PRIMARY`; there is no secondary
(adaptive optics) mount, and `CALIBRATED` is transient in PHD2 too. With no mount connected
`calibrating` is skipped and guiding measures in camera pixels, as it always did.

## Calibration

`guide_cal.c` is a port of `Scope::BeginCalibration` / `UpdateCalibrationState` and the
`Mount` transforms, read line by line from PHD2's source (cloned 2026-10-07, commit
`0f9f905`). The sequence: pulse **west** in 750 ms steps until the star has moved 25 px
(that gives the RA angle and rate), pulse **east** back in larger fast-recenter pulses,
**clear Dec backlash** by pulsing north until three consecutive moves of at least 60% of
the expected step go the same way, pulse **north** for the Dec angle and rate, **south**
back, then up to three **nudges** south to land within 2 px of the start. Every constant —
60 max steps, the 0.6 backlash factor, 3 accepted moves, 60 s of clearing pulses, 3 nudges,
2 px nudge tolerance, the 30° east-retrace check, the 12.5° orthogonality advisory — is
PHD2's.

Three places look odd and are deliberately PHD2's: the post-increment in
`steps++ > MAX_CALIBRATION_STEPS`; `dX/dY/dist` zeroed in one state and read in the next
after a fall-through; and with Dec guiding off, calibration completes one frame *after*
east finishes (PHD2 `break`s there and installs the result from the `COMPLETE` case on the
next call). PHD2's "little south movement" check is computed and not shown, as in PHD2,
which commented that alert out because Dec backlash triggers it on healthy mounts.

PHD2's `IsCalibrated()` rule is kept too: a calibration made with Dec guiding off covers RA
only, so turning Dec back on makes the next Guide recalibrate rather than guiding Dec with
no rate.

**Not ported, because no mount here can report position:** guide parity (needs RA/Dec
before and after a leg), declination compensation of the RA rate, meridian-flip handling,
the guide-rate-dependent backlash step, and the "different from the previous calibration"
advisory. ST-4 can never supply these; an LX200 backend (`:GR#`/`:GD#`) can, and those are
the next pieces to port when it exists. Calibration is also not yet persisted, so it is
lost when `alpacad` restarts.

### The simulated mount

The simulated source holds a mount state that pulses move: RA from periodic error plus
pulses, Dec from polar-alignment drift plus pulses through a backlash deadband, both then
rotated (and optionally mirrored) into the camera frame. The guider is told none of it —
calibration has to discover the rotation, the mirror and both rates, and the test checks
what it found against what was configured. Defaults: 30° rotation, 0.0025 px/ms (so a
750 ms step moves 1.9 px), 300 ms Dec backlash with the gear starting on the far side, 1.6 px
RA periodic error over 35 frames, 0.02 px/frame Dec drift, 0.1 px seeing.

Pulses take effect on the next frame and do not sleep. The star patches are now saved
wherever each star is drawn, every frame, and restored in reverse order — the old fixed
patch at the start position only coped with ~2 px of motion, and calibration moves 25+.

### What the tests show (`make test`, host)

| Scenario | Calibration found | Guided / unguided true RMS |
|---|---|---|
| ideal (0°, nothing moving) | angles within 0.03°, rates within 0.1% | 0.000 / 0.000 px |
| default (30°, PE, backlash, drift, seeing) | RA 1.0° off, Dec 1.0° off, rates within 3% | **0.36 / 3.03 px** |
| 135°, Dec mirrored | 0.8° / 2.0°, within 4% | 0.38 / 3.07 px |
| −100°, 1500 ms backlash, Dec rate 0.72× RA | 0.2° / **6.7°**, within 3% | 0.38 / 2.61 px |
| Dec guiding off | RA 0.4°, Dec left uncalibrated | 0.35 / 1.35 px |
| fast guide rate (3 steps) | correct, and the "few guide steps" advisory fires | — |
| RA does not move | fails: "RA Calibration Failed: star did not move enough" | — |
| Dec does not move | fails: "Backlash Clearing Failed: star did not move enough" | — |

Plus a transform round trip at 48 calibrations (every 15°, both Dec senses), exact to
3e-15 px.

The 6.7° in the −100° case is **periodic error, not the port**: with PE turned off and
nothing else changed it is 0.09°, and turning off seeing, drift or backlash instead changes
nothing. RA periodic error keeps running while the 19-step Dec leg is measured and pushes
the star sideways across it. PHD2 has exactly the same exposure. The angle tolerance is
therefore derived from the configured PE (a full peak-to-peak swing across one 25 px leg,
plus 1°), not tuned until the test passed. Guiding still held 0.38 px with that angle.

**The suite was mutation-tested**: flipping the RA pulse direction in the guide step fails
every closed-loop check, and swapping PHD2's argument order in the Dec angle puts it 180°
off and fails both the angle and guiding checks. A pass means something.

The same calibration runs through the real `guide_loop.c` under `guide_serve`: driven from
the page, the default simulator calibrates to the identical −148.96° / 119.03°, then guides
at 0.32 px RA / 0.13 px Dec RMS. Recalibrate, Clear, Dec mode, Stop→Start (calibration
kept, source kept) and a source switch (calibration cleared) were each exercised.

**What none of this can show** is how a real mount behaves: real backlash is not a clean
deadband, real guide rates drift, real stiction makes small pulses do nothing. That still
needs a mount and a clear night. What the bench does establish is that the port is
self-consistent and the signs close the loop at every geometry, so a night on the sky is
spent on the mount rather than on the maths.

### Fixed along the way

After **Stop → Start**, the loop always reopened the camera, even with Simulated selected —
the source choice was only applied when it changed. It now reopens the last chosen source,
and calibration survives a Stop/Start on the same source (PHD2 keeps it across sessions).

### An observation, not chased

`guide_sim`'s centroid error rose from 0.016 to 0.023 px mean (200 frames, synthetic
pedestal) with the new simulator. It is the Dec drift: `guide_sim` holds its search centre
fixed at the first position, and by frame 200 the star has wandered 4 px from it. With the
drift removed the new simulator scores 0.013 px, better than before. The real loop re-centres
every frame, so this does not affect it; why `guide_star_find()` loses ~0.01 px accuracy
when the star is 4 px off the search centre is open.

## Not done yet

- **A real mount backend.** ST-4 over GPIO and LX200 (`:Mg<dir><ms>#`) over a UART, both
  behind `guide_mount_pulse()`; then Camera `PulseGuide` on the Alpaca side calling the same
  function. Until one exists the camera source guides nothing.
- **Calibration persistence** (to `/userdata`, like the dew heater state), and the
  position-dependent parts listed under Calibration once LX200 can report RA/Dec.
- **Dead reckoning on a lost frame.** PHD2 asks the algorithms to `deduceResult()`; here a
  lost frame simply sends no pulse.
- Multi-star guiding, backlash compensation, the Gaussian-process algorithm.
- `guide_star_find()` has no dark-frame or defect-map input yet. On this sensor the real
  frame already shows hot pixels; `gather_band()` medians them out for *detection*, but
  tracking currently relies on the smoothed peak search to not latch onto one.
