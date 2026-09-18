# On-device guiding: feasibility

Can the RV run the guide loop itself — find a star, compute the error, and emit guide
pulses — instead of shipping every frame to a PC for PHD2 to do it?

**Yes.** The guide computation is ~0.25 ms per frame on this hardware, measured. The
memory wall that looked fatal is real but solvable, and it largely disappears on the
production SoC anyway. The design questions that remain are about the output path and the
PC-side integration, not about whether the board can do the work.

Everything below is either measured on the real board (marked as such), read out of source
(PHD2's, ours, the ASCOM spec, N.I.N.A.'s), or explicitly flagged as unverified.

---

## The premise needs one correction

> since that is a windows application it might not be possible

PHD2 is not a Windows application. It is cross-platform wxWidgets with first-class Linux
support — `CMakeLists.txt` has a `UNIX AND NOT APPLE` install path, Linux-native backends
(`scope_indi.cpp`, `cam_indi.cpp`), and it is packaged for ARM Linux distributions.
Standalone ARM autoguiders built on it are an established DIY genre, and the commercial
equivalents (Lacerta MGEN3, StarAid Revolution) are the product category this is aiming at.

The real obstacle is not the OS. It is that PHD2 is a **107,699-line GUI application with
no headless mode** — no `--no-gui` flag, no `headless` symbol anywhere in the tree.
Running PHD2 itself on the board would mean wxWidgets + GTK + X or Xvfb, hundreds of MB
against a 43 MB image. That option is dead for size reasons, not portability reasons.

Two supporting facts, checked rather than assumed:

- **C++ is already paid for.** `libstdc++.so.6.0.25` (1.0 MB) is already in the built
  rootfs, and buildroot has `BR2_TOOLCHAIN_EXTERNAL_CXX=y`. PHD2 needs C++14; the
  toolchain is GCC 8.3.0.
- **Licensing is clean.** PHD2 is BSD 3-clause. We can take code into a commercial product
  carrying only the copyright notice. Worth stating explicitly, because the obvious
  alternatives are not: Ekos/KStars' internal guider and lin_guider are both GPL.

---

## Where PHD2's weight actually is

The instinct that "PHD2 is big, therefore its algorithms are big" is wrong. The guiding
mathematics is small; the bulk is GUI, graphs, i18n, docs, and ~40 camera and ~20 mount
backends we need none of.

The entire hysteresis algorithm — PHD2's default, what nearly everyone guides with — is
this, from `src/guide_algorithm_hysteresis.cpp`:

```c
double GuideAlgorithmHysteresis::result(double input)
{
    double dReturn = (1.0 - m_hysteresis) * input + m_hysteresis * m_lastMove;
    dReturn *= m_aggression;
    if (fabs(input) < m_minMove)
        dReturn = 0.0;
    m_lastMove = dReturn;
    return dReturn;
}
```

Six lines. The camera-to-mount transform is a `cos`/`sin` pair against the calibration
angle. The others are the same order: lowpass is a linear fit, resist-switch a median
history plus a direction-change counter, `zfilter` a small IIR.

| Piece | Source | Take it? |
|---|---|---|
| Guide algorithms (hysteresis, lowpass, lowpass2, resistswitch, zfilter) | 1,976 lines, ~a third of which is the wx config dialog | **Yes** — small, and this is the part that is "the standard" |
| Gaussian-process / predictive PEC | `guide_algorithm_gaussian_process.cpp`, 48 KB, needs Eigen | **Not initially** — heaviest by far, optional in PHD2 too |
| Star centroid tracking | `Star::Find`, ~350 lines | **Yes** — the per-frame core |
| Star auto-selection | `Star::AutoFind` + `psf_conv` + `Median3` | **Yes, but restructured** — see memory |
| Calibration + mount geometry | `mount.cpp` (1,956), `scope.cpp` (2,412, minus backend-specific parts) | **Yes** — unavoidable, and the fiddly part |
| Backlash compensation | `backlash_comp.cpp`, 1,293 lines | Later |
| Multi-star guiding | `guider_multistar.cpp`, 56 KB, much of it drawing | Later — single star first |
| ~40 camera + ~20 mount backends, GUI, graphs, i18n, help | the other ~90,000 lines | **No** |

---

## Compute: measured on the board, and it is a non-issue

`bench/psf_bench.c` transcribes PHD2's actual `psf_conv` and a PHD2-shaped centroid at our
real frame geometry (2304×1296) and times the costs that matter. Run on the board
(1 core, 1104 MHz at the time, `neon vfpv4` per `/proc/cpuinfo`):

```
--- per frame (the guiding inner loop) ---
centroid  12 stars x 31x31 :    0.104 ms/frame
unpack    12 windows only  :    0.144 ms/frame   (cf. ~88 ms full frame)

--- live web preview (only while someone is watching) ---
unpack    full frame       :     50.3 ms
preview   4x -> 576x324 8bit :   35.1 ms         (182 kB on the wire)

--- once per session (star selection) ---
median3   full frame       :    754.9 ms
psf_conv  banded 128 rows  :   1437.5 ms         (peak float 2.5 MB, downsample=1)
                              -- 2x downsampled --
downsample 2x -> 1152x648  :     45.7 ms
psf_conv  banded 128 rows  :    340.5 ms         (peak float 1.3 MB)

peak RSS: 12160 kB (downsample=1)  /  16000 kB (with the preview buffers)
```

**The per-frame loop is 0.25 ms.** PHD2 tracks a star inside a 31×31 search region
(`DEFAULT_SEARCH_REGION = 15`), up to 12 stars (`MAX_LIST_SIZE = 12`) — ~11,500 pixels out
of 2.99 million. Against a frame period of 0.4 s at the very best and typically 1–4 s for
guiding, that is a duty cycle of well under 0.1%. The guide computation is free. This is
the central result and it is not marginal — it has three orders of magnitude of headroom.

The host-to-board factor came out at **~12× for the centroid, ~20× for the unpack, ~4× for
median3** — worth recording, since the earlier estimate of "10× to 50×" was doing a lot of
work in the previous version of this document.

There is a second win in the unpack line. `alpacad` currently spends ~88 ms per frame
unpacking the whole RAW10 frame because the client wants the whole image. **Guiding needs
only the search regions**, which is 0.145 ms. So on-device guiding drops both the ~88 ms
unpack and the ~265 ms RNDIS write of 5.97 MB — together about 70% of the measured ~0.49 s
loop. A local guide loop is not just "fast enough", it is structurally cheaper than the
current one.

Star selection lands at **~2.2 s full-resolution** (median 755 + psf_conv 1437) or
**~1.1 s at 2× downsample**, once per session. Both fine. An easy further win if it ever
matters: downsample *before* the median rather than after, which is what the benchmark
does in the wrong order — that would cut the 755 ms to roughly 190 ms and put AutoFind
under a second.

---

## Memory: the wall is real, confirmed by OOM, and solvable

Running the benchmark's original full-frame `AutoFind` shape on the board did not fail
gracefully — **the kernel OOM-killed it**:

```
Out of memory: Killed process 393 (psf_bench) total-vm:35776kB, anon-rss:17596kB
```

That `total-vm:35776kB` is the predicted 35.8 MB almost exactly: `usImage` original +
median copy (5.97 MB each) plus two `FloatImg` (11.94 MB each), which is what PHD2's
`Star::AutoFind` allocates. It is the same wall the `imagearray` buffering attempt hit.

One detail worth carrying forward: **`malloc` succeeded.** Linux overcommit meant the
failure arrived as a `SIGKILL` when the pages were touched, not as a null return, so the
benchmark's careful "ALLOC FAILED" path never ran. Any on-device allocation of this size
has to be budgeted in advance — it cannot be discovered by checking a return value.

PHD2's own escape hatch does not fire for us either: `AutoFind` downsamples only below
`DOWNSAMPLE_SCALE_THRESH = 0.6` arcsec/px, and we are at ~2 "/px (2.0 µm pixels at 200 mm
focal length). The heuristic downsamples when pixels are small *on the sky*, not when
memory is short.

**Banding fixes it completely.** Running `psf_conv` over 128-row horizontal bands with a
4-row overlap (the stencil reach) needs 2.5 MB of float instead of 23.9 MB, and total
**peak RSS drops from 35.8 MB (killed) to 12.2 MB (comfortable)** — measured above. The
codebase already has this pattern; `unpack_bits_lsb_transposed()` works in blocked tiles
for the same kind of reason. The cost is nil: the banded full-res pass is the 1437 ms
figure, and there is no correctness difference.

So this is a restructuring of `AutoFind`, not a blocker. The per-frame steady state was
never in question — one packed frame (3.73 MB, already mmap'd by V4L2) plus a few 31×31
windows. Guiding never needs the 5.97 MB unpacked buffer at all.

### Why PHD2 needs so much, and how much of that we inherit

Worth separating two things that got conflated above: the 35.8 MB figure is what PHD2's
`Star::AutoFind` *allocates*, not what PHD2 the application uses. Its real footprint is
larger, and the reason is not bloat — **PHD2's memory is a fixed multiple of frame size,
and the multiplier comes from features, not from the guiding algorithm.** It holds several
whole frames live at once:

| Buffer | Where | Type | At 2304x1296 |
|---|---|---|---|
| `m_pCurrentImage` | `guider.h:159` | `usImage`, u16 | 5.97 MB |
| `m_displayedImage` | `guider.h:139` | `wxImage`, RGB8 | 8.96 MB |
| scaled display bitmap | `wxBitmap`, display-depth | ~4 B/px | ~11.9 MB |
| `CurrentDarkFrame` | `camera.h:175` | `usImage`, u16 | 5.97 MB |
| `Darks` | `camera.h:176` | **map: exposure -> `usImage`** | **up to 131 MB** |
| `AutoFind` transient | `star.cpp` | 2x `usImage` + 2x `FloatImg` | 35.8 MB peak |

That `Darks` row is the striking one. It is one full frame *per exposure duration in the
dark library*, and `myframe.cpp:963` lists 22 durations (10 ms … 30 s). Build a complete
dark library at our sensor size and PHD2 is holding 131 MB of dark frames alone. On a DSLR-
class guide camera at 1280x960 that is 54 MB, which is why nobody notices on a desktop.

**So "can PHD2 be stripped down?" has a better answer than trimming it: almost none of
this is inherited in the first place.** We are porting ~2,300 lines of algorithm, not
running the application, and every large allocation above is a feature we either do not
have or implement differently:

- **The two display buffers (~21 MB) do not exist.** There is no local display. The browser
  renders a 182 kB 8-bit preview (measured above); the RGB expansion happens on the phone.
- **The dark library collapses.** A guide camera runs at one or two exposure durations, not
  22. One dark is 5.97 MB, and it can be dropped to zero by subtracting during the unpack
  that already happens, instead of keeping a frame resident.
- **`AutoFind` is 2.5 MB banded**, not 35.8 MB — measured.
- **The current-image buffer mostly is not needed either.** Guiding reads 31x31 search
  regions out of the packed V4L2 buffer, which is already mmap'd and lives in CMA, not in
  `MemTotal`. Only the preview path wants a whole unpacked frame.

Measured baseline for what this actually has to fit into: `alpacad` right now is
**VmRSS 12.1 MB, VmHWM 18.1 MB**, with 19 MB available. The high-water mark comes from
serving a full `imagearray`, which the guide loop never does. Adding guiding should move
the steady state by single-digit MB, not tens.

The honest conclusion is that the RAM question was pointed at the wrong thing. PHD2's
footprint is a property of PHD2-the-application; the guiding *algorithms* have essentially
no memory cost at all — the working set is one search region per star.

### RAM vs flash, since you asked

**32 MB is RAM, not storage.** `MemTotal: 32588 kB` on the board right now, with
`MemAvailable: 18864 kB`.

### Where the other 8 MB went

64 MB physical minus 24 MB of CMA should leave 40 MB, but `MemTotal` is 31.8 MB. The
kernel prints the full accounting (from this board's `dmesg`):

```
Normal ... present:65536kB managed:32588kB
8237 pages reserved
6144 pages cma reserved
```

```
present (64 MB physical)                        65536 kB
- CMA reservation        6144 pages            -24576 kB   RK_BOOTARGS_CMA_SIZE=24M
- kernel + early reserve 2093 pages             -8372 kB   text/data/bss, page tables, memmap
                                               ---------
= managed  (== MemTotal)                        32588 kB
```

So the missing ~8.2 MB is **the kernel itself** — its code, data and bss, plus the
`struct page` array and page tables the kernel allocates before userspace exists. That is
normal and not recoverable in any meaningful way; it is the floor for running Linux at
all. The 24 MB CMA carveout is the part that is oversized, and it is recoverable (below).

Note the CMA reservation is counted as *reserved*, not as managed memory, on this
Rockchip kernel — `CmaFree: 0 kB` with `CmaTotal: 24576 kB`. So it is not memory the
system can fall back on under pressure; it is genuinely carved out.

Three things follow, and they matter for the PCB decision:

- **A 256 MB or 512 MB NAND does not help this at all.** NAND is flash — program and image
  storage. The current image is 43 MB of rootfs against a 128 MB card, so storage is not
  remotely a constraint today. Buying more NAND buys nothing for the guiding problem.
- **RAM is not expandable on these parts.** RV1103 and RV1106 have the DRAM *inside the
  package* (RV1103 built-in 16-bit DDR2, RV1106 built-in 16-bit DDR3L). There is no
  external memory bus to populate. RAM is chosen by picking a part number, not by adding a
  chip: **RV1106G2 = 128 MB DDR3L / 0.5 TOPS, RV1106G3 = 256 MB DDR3L / 1 TOPS**.
- **So the production target already solves this.** The RV1106G3 on the planned PCB has
  256 MB — eight times this board. Even the naive 35.8 MB `AutoFind` would fit there with
  room to spare. The memory work above is worth doing anyway (it is cheap, and it makes
  the prototype board a usable development target), but it should not drive the BOM.

### A free ~10 MB on this board, if you want it

`RK_BOOTARGS_CMA_SIZE="24M"` reserves 24 MB of the RV1103's 64 MB for the contiguous
allocator — sized for the stock ISP/RGA/MPP/rockit pipeline this project stripped out.
Measured on the board right now:

```
CmaTotal:   24576 kB
CmaAllocated: 11664 kB     <- our 3 V4L2 buffers, 3 x 3.73 MB
CmaFree:         0 kB
```

We use 11.4 MB of the 24 MB. Dropping the reservation to ~14 M should return ~10 MB to
`MemTotal`, a ~30% increase in usable RAM for free.

Note this **changes the earlier conclusion in `CLAUDE.md`**, and correctly so. Shrinking
CMA was dismissed there because it would not help *framerate* — that reasoning was right,
and it still is: CMA backs DMA capture buffers, not `alpacad`'s heap. But the guiding
question is a *memory* question, not a framerate one, and for that it is the single
cheapest lever available. The catch is that it is a bootarg, so it needs a rebuild and a
physical SD card reflash (this board has no USB flashing — confirmed previously via
`upgrade_tool`, `Flash Size: 0MB`).

---

## Getting the pulses out

Your two use cases pull in the same direction, which is convenient:

> A standalone operation would allow users with only a tracker to guide it via ST4 without
> any computer. For our OpenAstroTech mounts, it could send the guidepulses directly to the
> mount, without having to use an extra USB cable.

### There is no Alpaca "guider" device — but the Camera interface already pulse-guides

The ASCOM/Alpaca device types are a fixed set of ten (Camera, CoverCalibrator, Dome,
FilterWheel, Focuser, ObservingConditions, Rotator, SafetyMonitor, Switch, Telescope).
There is no Guider, so "put the error out over Alpaca" has no standard home.

But it does not need one. The ASCOM **Camera** interface itself has `CanPulseGuide`,
`PulseGuide(Direction, Duration)` and `IsPulseGuiding`, specified for "a camera that can
directly send auto-guider pulses to the telescope mount via an electrical connection" —
i.e. a camera with an ST-4 port. Directions are `guideNorth`/`guideSouth`/`guideEast`/
`guideWest`. Confirmed in the ASCOM spec and in PHD2's own client: `cam_alpaca.cpp`
implements `ST4PulseGuideScope()` by calling `canpulseguide` at connect, then `pulseguide`
and polling `ispulseguiding`. PHD2 surfaces it as the **"On camera"** mount option.

So: **add three members to the Camera device we already ship, backed by four GPIOs wired
as ST-4.** That gets you:

- Standards-correct output with no protocol invention and no second device.
- Working PC-side guiding *before* any on-device guide loop exists — PHD2 or N.I.N.A.
  picks "On camera" as the mount and guides through our ST-4 port, no separate mount cable.
  That is your "no extra USB cable" win, available early and much cheaper than the full
  guide loop.
- The same GPIO path the standalone loop uses later. Not throwaway scaffolding.

The mechanism is already proven here: `switch_api.c` drives `/sys/class/gpio` for the dew
heater with env-configured pins (`OAG_DEWHEATER_GPIO`) and graceful degradation when unset.
ST-4 should follow that exactly. Caveat carried over: only 3 GPIOs are claimed on the
Luckfox Pico, so a real four-line ST-4 port is PCB work — the logic can be written and
checked against a logic analyser first, but the success path will not be hardware-verified
until the PCB exists.

### For OpenAstroTech mounts specifically, serial may beat ST-4

Worth considering rather than defaulting to ST-4. OAT/OAM firmware speaks an LX200-style
serial protocol including a guide-pulse command, and the SoC has **six UARTs**
(`uart0`–`uart5` in `rv1106.dtsi`; `uart2` is the debug console, so five are free). A
direct UART link to the mount board would be two pins instead of four plus optocouplers,
and it carries more than pulses — it could report or query mount state.

ST-4 is still the right *default*, because it is what the tracker-only users have and it is
universal. But if the PCB is going to sit next to an OAT anyway, a UART header alongside the
ST-4 jack is nearly free and strictly more capable. That is a PCB decision worth making
deliberately rather than by omission.

---

## Hijacking PHD2's event server for N.I.N.A.: yes, this works, and it is the right call

> we could hijack phd2's JSON-RPC to get things like guide error etc to sequencers like NINA

This is the strongest idea in your message and it checks out concretely.

PHD2's event server is **line-delimited JSON over TCP on port 4400**
(`event_server.cpp:2618`: `port = 4400 + instanceId - 1`). It is the de-facto standard
interface for "a thing that guides" — every sequencer already speaks it, because every
sequencer already integrates PHD2. Implementing that protocol makes N.I.N.A. see a PHD2
and dither, settle, and plot guide error with **zero changes on the PC side**.

The surface is bounded and known. N.I.N.A.'s client (`NINA.Equipment/Equipment/MyGuider/
PHD2/`) defines exactly 22 event types it consumes:

```
Alert, AppState, CalibrationComplete, CalibrationDataFlipped, CalibrationFailed,
GuideStep, GuidingDithered, GuidingStopped, LockPositionLost, LockPositionSet,
LoopingExposures, LoopingExposuresStopped, Paused, Resumed, SettleDone, Settling,
StarLost, StarSelected, StartCalibration, StartGuiding, Version
```

and a similarly bounded method list in `PHD2Methods.cs` — `guide`, `dither`,
`stop_capture`, `loop`, `find_star`, `get_app_state`, `get_pixel_scale`, `get_calibrated`,
`get_connected`, `get_star_image`, `capture_single_frame`, and a tail of getters. PHD2's
own table has ~50 methods; N.I.N.A. uses a subset, and many of those are trivial constant
getters.

Why this is the right architecture and not a hack:

- **It is the only standard that carries guide error.** Alpaca cannot express it. This can,
  and every sequencer already parses it.
- **Dithering works.** `dither` plus the `Settling`/`SettleDone` handshake is what makes a
  sequencer able to dither between subframes — the actual thing you lose by not running
  PHD2, and the reason "just emit the error somewhere" would not have been enough.
- **It composes with the Alpaca camera.** N.I.N.A. talks Alpaca to our camera for imaging
  and port 4400 for guiding, from the same box. Nothing conflicts; they are separate
  listeners in the same daemon.
- **It degrades honestly.** Unimplemented methods return a JSON-RPC error, which is a
  defined response, not a protocol violation.

The one thing to be careful about: **advertise only what actually works.** The
`canabortexposure` lesson from `CLAUDE.md` applies directly — reporting a capability that
silently does nothing is worse than reporting `false`, and sequencers make real scheduling
decisions on `get_app_state` and the settle events.

---

## A web UI for the guider: feasible, and mostly already built

> would it be feasible to do a simple interface for the guiding on a webpage? So a user
> could connect his phone and configure the guider, or watch the live images

The page itself is nearly a solved problem here. `setup_api.c` already serves a
self-contained, CDN-free, dark/red-for-night-vision page over the link-local link, already
drives real device state from a browser click, and is already registered at multiple
routes without disturbing API routing. A guider page is more of the same thing.

The two parts worth thinking about are the live image and the phone.

### Live images need no encoder at all

There is no `zlib`, `libjpeg` or `libpng` in the rootfs — checked. And the hardware JPEG
encoder is unreachable: it lives behind the MPP/ISP stack this project deliberately
stripped out.

Don't add any of that back. For a guide preview, **send a downsampled 8-bit buffer and
render it with canvas `putImageData`.** No encoder, no new dependency, no format
negotiation. Measured on the board:

```
unpack    full frame          :     50.3 ms
preview   4x -> 576x324 8bit  :     35.1 ms   (182 kB on the wire)
```

~85 ms of CPU and 182 kB, which is ~8 ms over the link at the measured 22.5 MB/s. At a
1–4 s guide cadence that is a few percent duty cycle, and it is only paid while someone is
actually looking.

Two notes. The full-frame unpack is 50 ms here versus the ~88 ms `alpacad` pays per
exposure, because the preview path reads sequentially while `alpacad`'s also transposes
into ImageBytes wire order. And the preview costs ~340× the guide loop itself (85 ms vs
0.25 ms), so on a single core it should be throttled and on-demand — a free-running
preview is the one thing on this page that could plausibly disturb guiding.

### The phone part is a BOM decision, not a software one

**This board cannot do it at all.** There is no WiFi hardware on this Luckfox Pico
variant: `/sys/bus/sdio/devices/` is empty, `/lib/firmware/` does not exist, and the only
interfaces are `usb0` and loopback. The USB gadget link goes to a PC, and a phone cannot
reliably consume an RNDIS gadget — that needs USB host mode plus a driver, which is
hit-and-miss on Android and effectively unavailable on iOS.

The custom board will have WiFi, so this is a "not on the prototype" problem rather than a
design problem. The options split cleanly:

- **Today, over USB:** a laptop or tablet browser works right now, with no new hardware.
  That is worth building first regardless, because it is also the development and
  diagnostic UI.
- **For a phone:** the PCB needs a WiFi module (RV1106 has SDIO for it), and the image
  needs `hostapd` + `dnsmasq` for AP mode — neither is in the rootfs today, and both cost
  a little flash and RAM.

There is a mockup of the page at `mockup/guider.html` — open it in a browser. **It is a
drawing, not a client: it talks to nothing and has never run on the RV.** It runs on
simulated guiding data (periodic error in RA, a random walk in Dec) so the layout can be
judged in motion rather than as a static sketch, and it is built to the same constraints
the real one has: no external stylesheet, script or font, one dark theme, and a single
560 px column that stacks at phone width.

**AP mode is the right choice** for the standalone use case: a field user with a tracker
and no computer has no network for the device to join. The device creates its own, the
phone connects, the browser opens. This is exactly what StarAid does, so it is a proven
interaction model rather than a guess.

One consequence worth stating early: in the standalone case **the web page is the only
UI there is.** Star selection, calibration, guide-error display and any failure reporting
all have to work there, for a user with no terminal and no PC. That raises the page from a
convenience to a load-bearing component, and it is an argument for designing it before the
calibration code starts assuming what its caller looks like.

## What is already true today, for free

So it is not re-derived later: **PHD2 on the PC can already drive this camera natively.**
`cam_alpaca.cpp` is a full Alpaca camera client, fetches frames via `ImageBytes` (which we
implement, ~6× faster than JSON here), and `DefaultUseSubframes = false` in `camera.cpp`,
so PHD2 requests full frames and our unimplemented `StartX`/`NumX` cropping does not bite.

That is the baseline on-device guiding has to beat, and the honest case for beating it is:

1. **Standalone operation** — no PC in the field. The MGEN3/StarAid category, and the only
   one of these that is a capability rather than an optimisation.
2. **No mount cable** — ST-4 or UART straight off the camera board.
3. **Latency** — the loop stops paying ~88 ms unpack + ~265 ms transfer per frame.

---

## Serving now: the guider runs inside `alpacad` at `/guide`

The loop is wired into the daemon and serves a live page. On this board that is
**http://169.254.232.63:11111/guide** (the address derives from the SoC chip serial, so it
is stable here and different on any other board; `dmesg | grep 'OAG: usb0'` prints it). The
setup page links to it, which is how a user arriving via a client's Settings button finds it.

It measures guiding; it does not yet perform it. There is no calibration and no ST-4 output,
so corrections are computed and plotted but never sent to a mount — and without calibration
the "RA/Dec" labels are really camera x/y.

The **frame-source toggle is on the page**: Camera runs the real sensor, Simulated draws
stars at known positions and needs no sky, no mount and no light. Both verified on hardware.

Two things this forced, both worth having independently:

- **`v4l2_exposure_lock()`**, because the atomic unit is "write controls → sample the settle
  reference → capture", and `g_ctrl_lock` only guarded individual ioctls. This closes the
  concurrent-`exposure_worker` race already on the open-issues list. Verified: four Alpaca
  `ImageBytes` captures served correctly *while the guide loop ran on the camera source*.
- **Releasing the frame after each iteration.** Holding it plus an Alpaca client's retained
  frame OOM-killed the daemon at `total-vm:49020kB`. Fixed; now peaks at VmHWM 25.8 MB with
  both running.

## Port started: the algorithm core is in `alpaca/src/guide/`

Steps 3 and part of 2 below are done and hardware-verified. `guide_star_find()` (PHD2's
`Star::Find`), `guide_star_autofind()` (`Star::AutoFind`, banded and single-pass) and four
guide algorithms are ported, with a harness that runs them against a **real frame off this
sensor** with injected stars at known sub-pixel positions for ground truth. Full detail in
`alpaca/src/guide/README.md`; measured on the board:

```
guide_star_find      0.182 ms per frame
centroid error       mean 0.0092 px, worst 0.0141 px over 20 frames, 0 lost
autofind ds=4         321 ms, 0.69 MB scratch   (ds=2: 872 ms / 1.38 MB, ds=1: 2912 ms / 2.76 MB)
```

Board and host agree to every printed digit. The banded AutoFind confirms the memory
analysis above in practice: the same work PHD2 does in 35.8 MB (OOM-killed here) runs in
0.69 MB.

There is a frame-source toggle: `--source camera` pulls live frames from the running
`alpacad` over Alpaca HTTP, `--source sim` draws stars at known sub-pixel positions over a
real frame from this sensor. Both are verified working.

**That toggle settled an architecture question the hard way.** Measured: `alpacad` retains a
whole 5.97 MB frame after every exposure (VmRSS **12.5 MB at rest, 18.4 MB after one
exposure**) and holds two transiently while capturing the next. A separate guide process
that also holds a frame needs room for three between them, and the RV1103's 32 MB does not
have it — pulling frames from a separate process **OOM-killed `alpacad` three times**, the
kernel choosing the daemon over the test that caused it. So: **the guide loop must live
inside `alpacad`, sharing the frame it already captured**, not in a separate process. That
is better anyway — no HTTP round trip, no second 6 MB copy. The tool now refuses to open a
camera source when memory is short rather than taking the daemon down.

Still to do there: calibration, ST-4 output, and the in-daemon loop that joins them.

## Suggested order of work

1. **ST-4 output as Camera `PulseGuide`** (env-configured GPIOs, following `switch_api.c`).
   Standards-correct, immediately useful with PC-side PHD2, unblocks everything after it,
   and is the smallest real deliverable here.
2. **Restructure `AutoFind`**: banded `psf_conv`, downsample before median. Measured to fit
   in 12.2 MB and ~1 s. Optionally reclaim ~10 MB by shrinking `RK_BOOTARGS_CMA_SIZE`.
3. **Port the tracking core**: `Star::Find` centroid + hysteresis, on search regions only,
   no full-frame unpack.
4. **Calibration** — `mount.cpp`'s transform plus the pulse-and-measure sequence. The
   fiddliest remaining piece and the one most worth taking from PHD2 verbatim rather than
   reinventing: the sign conventions and orthogonality-error handling are where PHD2's own
   comments admit the original code was wrong for years.
5. **The web UI**, extending `setup_api.c` — star selection, calibration, live preview
   (raw 8-bit + canvas, no encoder). Works over USB on a laptop today. It is the only UI
   in the standalone case, so sketch its shape *before* step 4 — calibration code should
   not be written assuming a caller that turns out not to exist.
6. **PHD2 event server on port 4400** — the 22 events and the N.I.N.A. method subset. Do
   this once there is real guide state to report; it is the piece that makes the whole
   thing usable from a sequencer.
7. Later: backlash compensation, multi-star, dec guide modes, the GP algorithm. WiFi + AP
   mode if the phone UI is wanted — a PCB and image decision, not a software one.

## Files here

- `ROADMAP.md` — consolidated status, what each missing piece costs to build, and the mount
  interface options (ST-4 GPIO, LX200 serial for OpenAstroTech, Alpaca Telescope client).

- `bench/psf_bench.c` — the benchmark. Takes `[downsample] [band_rows]`. Contains a
  transcription of PHD2's `psf_conv` (BSD 3-clause, attributed in the source header).
- `bench/Makefile` — cross-compiles with the project toolchain.
  Build it for the board with the toolchain (see `bench/Makefile`); the binary is not
  checked in.
- `mockup/guider.html` — a standalone mockup of the guider page (simulated data). Same
  design language as `setup_api.c`'s page, self-contained with no external stylesheet,
  script or font, and deliberately single-theme for dark adaptation. Open it in a browser;
  to make it real, replace the simulation block at the bottom with a poll of guider state
  and keep the drawing code.
