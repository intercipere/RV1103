# On-device hinted plate solving: feasibility

Can the RV take one guide frame, plus an approximate pointing from the mount, and return
the exact RA/Dec, rotation and image scale of that frame on the device itself?

**Yes. Measured on the board (2026-10-15): a solve takes ~60 ms and 0.64 MB.** A *hinted* solve
only touches the few square degrees of catalogue around the hint, and it reuses the star
finder the guider already has. Two things can block it, and neither is technical:

1. **The catalogue licence.** Every modern deep catalogue is Gaia, and Gaia data are
   CC BY-NC 3.0 IGO. Commercial use needs a written licence from ESA. See
   "The catalogue" below.
2. **Limiting magnitude, still unmeasured.** The solve needs roughly V 11–12 in a guide
   exposure to get 15–40 stars even at the galactic poles. No real sky frame from this
   camera exists yet.

Status (2026-10-15): **the ASTAP solver core is ported to C (`src/`), host-tested against
a synthetic sky, and benchmarked on the RV1103.** See "Port and measurements" below. Not
yet done: real sky frames, a real catalogue, integration into `alpacad`. Figures below are
marked as measured on the board, read from source, counted by us, or estimates.

---

## Why bother: what a solve gives the guider

Most of this is the mount telemetry that `Guiding/ROADMAP.md` says ST-4 cannot provide:

- **Declination for Dec compensation**, with no UART link to the mount. Guide rate in RA
  scales with cos(Dec). Today that needs LX200 `:GD#`, and an ST-4-only mount cannot
  report it at all. One solve provides it.
- **Camera angle.** Guiding still needs PHD2's calibration to measure the guide rates.
  But a solve gives the sky orientation directly. A 180° jump between two solves means
  a meridian flip, and a small one means the camera was rotated. That covers part of the
  "position-dependent calibration" to-do.
- **Polar alignment**, in the style of SharpCap: solve, rotate RA, solve again, and the
  centre of rotation is the mount axis. This is the strongest feature for a standalone
  guider.
- **Goto refinement for OpenAstroTech**: solve, then LX200 sync. Also a "where am I
  pointing" readout on the web page.

**The hint comes from the mount.** For OpenAstroTech it is `:GR#`/`:GD#` over LX200. A
plain ST-4 mount gives no hint at all, which means a *blind* solve. That is a different
and much harder problem: it would need the whole catalogue indexed, not just a few
tiles. Blind solving is out of scope here. If it is ever needed, ASTAP's spiral search
outward from a guess is the cheap halfway step.

---

## Budget, measured on the board (2026-10-15)

```
CPU          1 x Cortex-A7 @ 1104 MHz, neon vfpv4 (scaling_cur_freq, /proc/cpuinfo)
MemTotal     32588 kB
MemAvailable 18584 kB   with alpacad idle (VmRSS 12760 kB, VmHWM 18164 kB)
CMA          24576 kB reserved, ~11.4 MB of it used by our 3 V4L2 buffers
Flash free   60.6 MB on / (87.4 MB ext4), 13.8 MB on /userdata
```

**Field of view** at 182 mm:

| Sensor | Field | Area | Diagonal |
|---|---|---|---|
| SC3336, 2304x1296 @ 2.78"/px | 1.78° x 1.00° | 1.78 deg² | 2.04° |
| IMX327/290, 1920x1080 @ 3.29"/px | 1.76° x 0.99° | 1.73 deg² | 2.01° |

Both are the same field for solving purposes, so a catalogue built for one serves both.

**RAM matters for production, not just this board.** `Guiding/README.md` says the
production SoC "already solves" the memory problem with 256 MB. That was true for the
RV1106G3. But the PCB baseline agreed on 2026-10-14 is the **RV1103G1 (64 MB)**, so
production has this board's RAM. The cheap lever is still available: `rk_dma_heap_cma=24M`
on the kernel command line (`RK_BOOTARGS_CMA_SIZE`) is about 10 MB larger than our
buffers need. Shrinking it to ~14M should raise MemAvailable to ~28 MB. It needs a
rebuild and a reflash. Worth doing for the guider anyway.

**What a solve needs to hold**, estimated:

- The frame. The guide loop already gets one, and the star list comes from the existing
  banded `guide_star_autofind()`, which peaks around 0.7–1.3 MB of scratch.
- At most a few thousand catalogue stars from up to 4 tiles near the hint.
- The quads and hashes built from them.

That is hundreds of kB, not MB. The catalogue itself stays on flash and is read per tile.

---

## Candidates

All of these were read from source in a scratch clone; none were built.

| Solver | Licence | Language | Catalogue for ~1.8° | Uses a hint | Verdict |
|---|---|---|---|---|---|
| **ASTAP** (Han Kleijn) | **MPL-2.0** | Free Pascal | Own tiled format. D05 = 102 MB, but its layout is truncatable | **Native**: synthetic field around RA/Dec, spirals out to a search radius | **Port this** |
| arcsec | MIT | Rust (+ C lib) | Reads ASTAP's databases | Yes, ASTAP-style | Second reference. ARMv7+uClibc is not a supported Rust target |
| astrometry.net | **GPL-3+ as a whole** | C | 4107–4111 (Tycho-2) = 165/95/50/25/10 MB, each a single all-sky file | Yes, but saves no flash | No: GPL (index reader `qfits-an/` and GSL are GPL and unremovable) |
| StellarSolver | GPL | C++/Qt | astrometry.net's | Same | No: GPL, Qt |
| tetra3 / cedar-solve | Apache-2.0 | Python | Built for 10–30°. At 1.8° it needs ~1.4 M pattern stars, ~33 MB in RAM (estimate) | **No**: lost-in-space only | No |
| Richmond `match` | GPL-2+ | C | — | — | No |

Sources: astrometry.net `LICENSE` and `solver/engine.c`; ASTAP `LICENSE`,
`unit_astrometric_solving.pas`, `unit_star_align.pas`, `unit_star_database.pas`,
`documentation/astap_astrometric_solving.htm`; tetra3 `tetra3.py`
(`solve_from_centroids`, the database builder); index sizes from data.astrometry.net and
SourceForge listings.

### How ASTAP solves

It is small, which is why it is the right port. The relevant Pascal is ~5,900 lines across
three units, much of it GUI glue and stacking code.

1. Build four-star quads, each from a star and its nearest neighbours.
2. Hash each quad by its five distance ratios d2/d1 … d6/d1. That makes the hash
   independent of scale and rotation.
3. Match image quads to catalogue quads within `quad_tolerance`. The default is
   ~0.007 (`unit_star_align.pas`, `find_fit`).
4. Reject outliers by median scale ratio. Require at least 3 matching quads
   (`minimum_count`, "3 for plate solving").
5. Fit a 6-constant linear plate solution by least squares.

The catalogue window is 1–2x the field depending on star count
(`unit_astrometric_solving.pas` ~line 966). Without a good hint, it spirals outward over
neighbouring windows.

**Port, don't reimplement.** The same rule as PHD2 (`Guiding/ROADMAP.md`): port the
source with its constants, not the description. The algorithm is simple enough to invite
a from-scratch rewrite, and that is exactly how the guider ended up with a resist-switch
that was a different algorithm. Two practical points:

- **Port from upstream ASTAP on SourceForge** (`hg.code.sf.net/p/astap-program/code`).
  An earlier version of this note claimed the GitHub mirror (`CanardConfit/ASTAP`) was edited.
  That was wrong: its "2026 perf" comments are upstream's own. The port was still made
  from upstream, changeset `4ea2c2f5`.
- **MPL-2.0 is file-level copyleft.** Ported files stay MPL-2.0 and their source must be
  offered. The rest of `alpacad` is unaffected. That fits a commercial product, unlike
  GPL, but it is a real obligation. Keep the port in its own files with the notice.

---

## Star density: how deep the catalogue must go

Counted by us from Gaia DR3 (`gaiadr3.gaia_source` via the ESA TAP service). V is derived
from G and BP−RP (Riello et al. 2021).

| V < | All-sky /deg² | Galactic pole /deg² | Stars per field at the pole | All-sky count |
|---|---|---|---|---|
| 9 | 3.1 | 1.7 | ~3 | 129 k |
| 10 | 8.6 | 3.8–4.1 | ~7 | 356 k |
| 11 | 22.6 | 8.4–9.4 | ~15–17 | 931 k |
| 12 | 56.3 | 18.3–21.1 | ~33–38 | 2.32 M |

Quad matching wants roughly 10–30 stars after losses. That rules out **V 10 as too thin
at the poles**, makes **V 11 the minimum**, and makes **V 12 comfortable**. The galactic
plane has ~6x the pole density, so a per-tile brightness cap matters there more than the
magnitude cut.

**Flash cost** in ASTAP's layout: 1476 tiles of ~5°, 5-byte records sorted bright to
faint, so any tile can be truncated.

- V<11: **~4.7 MB**.
- V<12: **~11.6 MB**.

Both fit in the 60 MB free on the SD card today and in a 128 MB NAND. Larger NAND
(256 MB) is not needed for this.

**Unverified assumption that matters most:** that the guide scope reaches V 11–12 in a
0.1–5 s exposure at gain 1x (gain is still pinned). Pixel scale and aperture make it
plausible, but nobody has measured it. The first real sky frames answer it: solve one on
the PC with ASTAP and read the faintest matched magnitude.

---

## The catalogue

This is the open problem, and it is a decision for you, not for code.

- **Gaia** (DR2/DR3) is **CC BY-NC 3.0 IGO**. ESA's terms require a written licence
  request (data.licences@esa.int) for commercial use
  (cosmos.esa.int/web/gaia-users/license). That covers ASTAP's D-series and
  astrometry.net's 5200 series.
- **Tycho-2** (2.54 M stars, 99% complete to V≈11.0, 90% to V≈11.5, CDS `I/259`) has
  enough depth for V 11. It is also ESA data, and its terms were **not** verified. It is
  probably the same situation, possibly looser.
- **Hipparcos** alone (118 k stars) is far too sparse at 1.8°.

Options, in order of preference:

1. Ask ESA for a commercial licence for a derived V<12 subset. This is the cleanest path,
   and the request is a single email.
2. Use Tycho-2 if its terms turn out to allow it, accepting V≈11 and thin fields near
   the poles.
3. Leave the catalogue off the device image and have the user download it. This is what
   ASTAP and astrometry.net effectively do, but it costs the standalone story.

Whichever is picked, generate the catalogue with **our own host-side tool**, not by
redistributing ASTAP's files. That keeps the provenance and licence of the data separate
from the solver code.

---

## The NPU: not for this

The RV1103's NPU is rated **0.5 TOPS, INT4/INT8/INT16** (`PCB/RV1103 datasheet V1.5.pdf`).
The runtime is small and already built, though the debloat keeps it out of the image:
`librknnmrt.so` is 190 kB and `rknpu.ko` is 41 kB in `media/out/lib/` and
`sysdrv/out/kernel_drv_ko/`.

Plate solving has four stages, and only one has the shape an NPU accelerates:

| Stage | Shape | NPU? |
|---|---|---|
| Star extraction (`median3` + `psf_conv`) | Convolution over the frame | **Yes, in principle** |
| Quads (nearest neighbours of 50–500 points) | Tiny | No |
| Hash matching | Branchy compares | No |
| Least-squares fit | 6 unknowns | No |

Even that stage is not worth moving:

- **It is already cheap enough.** On the CPU, `psf_conv` at 2x downsample was measured at
  340 ms (`Guiding/README.md`). `median3` was measured at 755 ms at full resolution, and
  downsampling it first should bring it to ~190 ms. That is under a second, for an
  operation that runs once per solve. Even polar alignment, re-solving every frame,
  tolerates that.
- **It costs exactly what is scarce.** NPU tensors are DMA buffers from CMA, the
  reservation we want to *shrink* to free RAM. A full-frame INT16 input is another ~6 MB.
- **INT8 would clip the data.** The frames are 10-bit with faint stars a few ADU above
  the background, so INT16 is needed. That is the NPU's slowest mode.
- **The tooling is heavy.** Even a one-layer convolution goes through rknn-toolkit2
  model conversion on the host.

The NPU only becomes interesting if a learned model is ever wanted, for example to reject
clouds, satellites or hot pixels. That is speculative and outside this plan. Nothing about
the NPU affects the PCB.

---

## Getting the hint: where the current pointing comes from

ASCOM's Camera interface has no member through which a client could tell the camera
where it points. The hint has to come from somewhere else. Options, cheapest first:

1. **The previous solve.** A tracking mount holds RA/Dec, and a guiding one holds it to
   arcseconds. After one successful solve, every later hint is free until a slew.
   Detect a slew from a sudden jump in star positions and drop the hint.
2. **Known pointings that need no mount.** Polar alignment starts at the home position,
   which points at the celestial pole. "Somewhere within 5° of the pole" is a valid hint,
   and RA does not matter there.
3. **The user, on the web page.** Type a target name, or RA/Dec, before the first solve.
   A list of a few thousand named objects (Messier, bright NGC, named stars) is tiny.
   This is the fallback for ST-4-only mounts.
4. **LX200 over the header UART** (Meade-protocol mounts; for OpenAstroTech see the
   caveat below: it does not listen on a spare UART). `:GR#`/`:GD#` return
   RA/Dec; OAT's handling of them is already confirmed from its source (`CLAUDE.md`,
   "Mount interface"). The same link carries guide pulses (`:Mg`) and sync-after-solve.
   LX200 coordinates are **JNow**, so we precess to J2000 for the catalogue, which needs
   the date (see "Time" below).
5. **NexStar protocol** for Celestron and Sky-Watcher SynScan hand controllers. It is a
   different, equally small protocol (`e`/`E` return RA/Dec). Those ports are **RS-232
   levels**, which means an external adapter (see PCB notes).
6. **An Alpaca Telescope client inside `alpacad`, over the USB link.** The PC side already
   knows the pointing whenever N.I.N.A./SharpCap/PHD2 drive the mount. If the mount driver
   is exposed as Alpaca (native, or via ASCOM Remote on Windows), `alpacad` can read
   `RightAscension`/`Declination` from the PC's link-local address, found through the same
   UDP discovery. **This covers every ASCOM mount, including USB-only ones** (EQMOD cables,
   iOptron, Celestron USB). It needs no hardware, only a PC, so it does not fit the
   standalone case. It is `Guiding/ROADMAP.md`'s "Backend 3", and it would serve both.

**A solve needs a sky position, not a site position.** A common intuition is that the
first solve needs the observer's location, or else it is blind. It does not. The hint is
RA/Dec, and every option above provides that without location. Location only enters when
the only thing known about the pointing is in *horizon* coordinates, for example "the
camera is tilted 40° up". Converting that to RA/Dec needs location and time, plus a
tilt sensor this board does not have. So location is *useful* (polar-alignment wording,
refraction, sync), but not a precondition for solving.

### OpenAstroTech, read from source (2026-10-15)

Checked against `OpenAstroTech/OpenAstroTracker-Firmware` at `42d2982` (2026-10-13):

- **Location is stored and readable.** `:Gt#`/`:Gg#` return site latitude/longitude
  (`core/meade/MeadeParserGet.cpp`). `:St`/`:Sg` set them, and `Mount::setLatitude`/
  `setLongitude` write them to EEPROM (`Mount.cpp`, `EPROMStore.cpp`). So the plan is as
  proposed: read the mount's stored location, show it on the page, and let the user edit
  it. An edit is written back with `:St`/`:Sg`, which also fixes the mount's own copy.
- **Time is not stored.** OAT keeps time as "the value last set, plus `millis()` since"
  (`Mount::getLocalTime`), and boots at **2021-01-01** (`Mount.cpp`, constructor). Its
  optional GPS module is the exception. Until something sets it, OAT's local sidereal time,
  and therefore its reported RA, is wrong; Dec is unaffected. **In the standalone case the
  camera should be OAT's time source:** take the phone's time from the page and push it with
  `:SL`/`:SC`/`:SG`. That makes our RA hint valid, and it fixes OAT's own gotos as a
  side effect.
- **OAT does not accept Meade commands on a spare UART.** `f_serial.hpp` processes commands
  from `Serial` only, which is the USB port on Mega and RP2040 boards, plus Wi-Fi on ESP32
  builds (`WifiControl.cpp`: TCP server and UDP discovery). Serial1–3 are assigned to GPS,
  TMC drivers, focuser or debug, depending on board and build (`boards/*/pins_*.hpp`).
  **So the LX200 UART on our header cannot talk to a stock OAT.** Options:
  1. A small change in OAT's firmware to also process commands on a free hardware UART.
     OAT is open source, so this could go upstream. Which UART is free varies per board
     config.
  2. **Wi-Fi.** The production camera will have Wi-Fi, and ESP32-based OAT builds speak
     Meade over TCP. Mega-based OATs have no Wi-Fi.
  3. USB host from the camera to OAT's USB port. This needs USB role switching and a
     second power input on the camera, since it is bus-powered from that same port. It is a
     PCB-level change.

  The UART header is still worth fitting for other LX200 mounts. But "OAT over UART" is
  not true today, and earlier notes in this repo implied it was.

Mounts that only offer a USB port cannot be wired to the camera directly. The RV1103 has a
single USB 2.0 port, and it is already the gadget to the PC. Option 6 is how those get a
hint.

### Time

**The RV1103 has no RTC.** Its datasheet lists only the 24 MHz oscillator.
`rv1106.dtsi` has an RTC node, but that is an RV1106 block. After every power-up, the
clock is wrong until something sets it. How much this matters depends on the task:

- **A hinted solve does not care.** J2000 to JNow over 26 years is ~0.36°, well inside
  any hint radius.
- **Sync to the mount and polar alignment do care, but only to about a month.**
  Precession is ~50"/year, so a month is ~4". A clock stuck at the epoch is 56 years off,
  ~47'.

Sources, any one of which is enough: the LX200 date and time query (standard Meade
`:GC#`/`:GL#`; OAT's support is **not verified**), the browser's clock sent by the web page,
the PC over the USB link, or as a last resort the build timestamp in
`/etc/openastroguider-version`, which is within a month or two of the truth. **No RTC chip is
needed.**

### The standalone case: phone or tablet, no PC (use case agreed 2026-10-15)

The expected standalone user images with a DSLR and has no PC. They open the camera's web
page on a phone or tablet and run polar alignment from there. So the page supplies both
time and location.

**Time from the browser: yes, trivially.** `Date.now()` needs no permission and is
NTP-accurate on a phone. The page sends it on load. Keep it as an offset inside
`alpacad` (or set the system clock if it is still unset). Use UTC epoch, so time zones
never come into it.

**Location from the phone's GPS: not over plain HTTP.** The browser Geolocation API only
works in a *secure context*: HTTPS, or `localhost`. A page served from
`http://169.254.x.y` or an AP address like `http://192.168.4.1` is neither, so browsers
refuse the location request. This is the W3C secure-contexts rule, not a device quirk.
Options:

- **From the mount, editable on the page.** OAT keeps the last site in EEPROM (see
  "OpenAstroTech, read from source"), and users rarely move far between sessions. Show it
  on the page, let the user correct it, and write corrections back to the mount. If no
  mount is connected, fall back to manual entry stored in `/userdata`. 1° precision is
  enough for everything below. **This is the plan** (user decision, 2026-10-15).
- HTTPS with a self-signed certificate. The user must click through a certificate warning,
  and whether Geolocation is then allowed on the overridden origin is **not verified**.
  Poor UX either way.
- A native companion app. It has GPS access, but it is a whole extra product.

**What location is actually needed for.** Less than it seems:

- **Finding the polar alignment error needs no location.** Two solves with an RA rotation
  between them give the mount axis. The JNow celestial pole needs only the date. Showing
  the user where to move the axis in the image, SharpCap-style, works without location.
- **Location adds two refinements:**
  - "Raise altitude 12', move azimuth 5' east" instead of an arrow in image coordinates.
    That needs latitude, plus longitude for local sidereal time, to ~1°.
  - Refraction correction of the pole's apparent position. It is arcminutes near the
    horizon, so it matters at low latitudes. That needs latitude.

**The bigger standalone question is how the phone connects at all.** The current link is
an RNDIS USB gadget. Earlier research (`Guiding/README.md`, "The phone part") found phones
consume that unreliably: Android hit-and-miss, iOS effectively not at all. The standalone
case therefore needs Wi-Fi in AP mode. The production version will have a Wi-Fi module
(user, 2026-10-15).

---

## Port and measurements (2026-10-15)

`src/astap_solve.c` and `src/astap_db.c` are a line-by-line port of ASTAP's solver core
and its `.1476` catalogue reader, from upstream SourceForge changeset `4ea2c2f5`. Both
files are MPL-2.0 with ASTAP's notice. Ported:

- `find_quads` and `find_many_quads` (5/6/7-star groups for sparse fields);
- `find_fit`, plus the hashed variant above 120 quads;
- the Givens `lsq_fit` and the XY-ratio sanity check;
- `read_stars` with ASTAP's four-tile area split;
- `solve_image`'s spiral search and its second, recentred "maximum accuracy" pass.

Constants are ASTAP's, and Pascal `Round` is ported as `rint`. **The deviations are listed in
the file headers.** Not ported: star detection, binning, autoFOV, SIP. The one change
of substance is the second pass's "keep only stars visible in the image" filter. ASTAP
does it with its own pixel conventions (`rotate()`). Here it inverts the first pass's fit,
which is exact for any convention.

`src/catalog.c` writes the `.1476` format. It is our code and the start of the host tool
that will build the real catalogue.

### Host test: synthetic sky (`make test`)

`solve_test` builds a sky with Gaia's counts per magnitude (from "Star density" above),
scaled by a density factor, and writes it with `catalog.c`. It then synthesises detections
at a known pointing, rotation, scale and mirror, with these failure modes:

- a soft limiting magnitude;
- 5% lost stars;
- 0.15 mag brightness scatter;
- 0.25 px centroid noise;
- 5% spurious detections;
- a hint up to 5° off;
- ±3% scale error.

Accuracy is checked with `astap_pixel_to_radec()` against an independent projection at the
centre and corners, so no axis convention can hide an error. 100 trials per row:

| Sky | Catalogue | Image limit | Solved | Wrong | Centre error p50 / p95 | Host time |
|---|---|---|---|---|---|---|
| average | V<12 | V<11 | 100/100 | 0 | 0.2" / 0.4" | 2.5 ms |
| galactic pole (0.35x) | V<12 | V<11 | 93/100 | 0 | 0.3" / 0.6" | 6 ms |
| galactic plane (2.3x) | V<12 | V<11 | 100/100 | 0 | 0.1" / 0.3" | 2 ms |
| galactic pole | V<12 | V<11.5 | 98/100 | 0 | 0.2" / 0.7" | 5 ms |
| average | V<11 | V<10.5 | 100/100 | 0 | 0.2" / 0.6" | 4 ms |
| galactic pole | V<11 | V<10.5 | 65/100 | 0 | 0.4" / 1.0" | 14 ms |

- **No wrong solutions anywhere.** Across all runs, including 1200 extra trials, no
  solve has ever put the centre on the wrong field.
- **Failures are fields with too few stars.** 41 of the 44 failures in one full run had
  fewer than 10 detected stars, and none had more than 15. This confirms the depth
  argument above: at the galactic poles the camera needs about **V 11.5**. V 10.5 is not
  enough there, whichever catalogue is used.
- **About 1% of solves are imprecise**, with corner errors of 30–60". The centre is still
  within ~1–20". The cause is coincidental quad matches surviving into the least-squares
  fit, with residuals of 185–806". ASTAP has no residual-based rejection, so this is the
  method, not the port. It does not matter for polar alignment, goto or Dec compensation.
  `resid_max` in the solution flags these. A residual-rejection refit would be a small
  addition *beyond* ASTAP, and is left for when a use case needs it.
- **The test can fail.** It catches each of three deliberate mutations:
  - a sign error in the projection: 20/20 wrong;
  - swapped Dec bytes in the reader: 0 solved;
  - no scale-outlier filter: the 10+ star solve rate drops to 85/90.

### On the board

`solve_bench.arm` ran on the RV1103 at 1104 MHz, with `alpacad` running. It solved 40
cases per sky, generated on the host (the 56 MB synthetic sky does not fit on the board):

```
average sky, V<12 catalogue, image V<11:   40/40 solved   59 ms avg   142 ms max   VmHWM 640 kB
pole sky,    V<12 catalogue, image V<11.5: 39/40 solved  254 ms avg  6949 ms max   VmHWM 668 kB
```

The results are identical to the host's. The one slow case is the failure: a 14-star field
that searched the whole default 20° radius before giving up. Excluding it, pole solves
average ~80 ms. **With a mount hint good to a few degrees, the search radius should be
5–10°**, which caps failure time at roughly a quarter of that.

**Memory is a non-issue.** The solver peaks at 0.64 MB, against 18 MB available. The
catalogue is read one ~10 kB tile at a time.

**Wider search radii cost time, not RAM.** The spiral visits one catalogue window at a time
and discards it, so memory is the same at any radius. Time grows with the number of
windows, roughly (radius / field height)². The measured failure covers ~1250 windows of
1° in 6.6 s, so about **5 ms per window on the board**. Extrapolated, not measured:

| Search radius | Windows | Worst case (no match) |
|---|---|---|
| 5° | ~80 | ~0.4 s |
| 10° | ~310 | ~1.7 s |
| 20° (ASTAP default) | ~1250 | 6.6 s, measured |
| 45° | ~6400 | ~35 s |
| whole hemisphere | ~20000 | ~2 min |

A successful solve stops as soon as it matches, so typical times are far lower. Two
caveats before relying on wide radii:

- ASTAP's spiral covers at most ±90° of RA around the seed. **A true blind solve would need
  two seeds.** That is a cheap loop around `astap_solve()`, but it is untested.
- **Every extra window is another chance of a coincidental match.** There was not one
  wrong solve in ~1800 trials, but those used hints within 5°. A wide-radius false-match
  rate must be measured before offering a blind solve. If it holds up, it would cover the
  ST-4-only mount that has no hint at all, in minutes rather than never.

**Flash:** the synthetic V<12 sky, which has the real star count, is **14.0 MB in 1476
files**, and occupied 14.4 MB on the board's ext4. A real V<12 catalogue will be about
the same size.

## Plan

1. ~~Host prototype, synthetic fields.~~ Done, see above.
2. ~~On-board bench.~~ Done, see above.
3. **Real sky frames.** Settles the limiting-magnitude question. Cross-check every solve
   against desktop ASTAP on the same frame. This step can also run the port against
   ASTAP's own D05/D20 databases, which the reader already understands.
4. **A real catalogue**: a host tool that turns a Gaia/Tycho extract into our `.1476` files
   with `catalog.c`. It can wait for the licence decision.
5. **Integration**: a `solve` action in the guide page and API, and the LX200 hint. One thing
   to settle first, by reading ASTAP's `find_stars`: `guide_star_autofind()` drops
   saturated and near-edge stars, which is right for guiding. But the solver matches the
   *brightest* image stars against the brightest catalogue stars, so a solve wants those
   stars kept. That points to a star-list mode of the existing finder, not a second
   finder.

The catalogue licence only blocks shipping, not prototyping. (2026-10-15: the user will
raise it with ESA contacts later.)

---

## What this means for the PCB

Plate solving adds **almost nothing** to the board. Storage (1 Gbit SPI NAND, ~12 MB
needed), RAM (64 MB, after the CMA trim) and the CPU are all enough as planned. The one real
item is the **LX200 UART on header J203**, because it is the hint source and the sync path.
In schematic v1 (`PCB/HARDWARE-DESIGN-NOTES.md` §13) it runs straight from the header to
SoC pins 13/14 (UART5_M0, VCCIO4, 3.3 V) with no protection. That cable goes to a
different device with its own supply and ground:

- **RX must survive 5 V.** OpenAstroTech's usual controller boards are ATmega2560-based,
  so 5 V logic (this is from general knowledge, **not verified** against OAT's hardware
  repository). The SoC's 3.3 V pins tolerate only VDDO+0.3 V (datasheet `Vih` max). A
  5 V-tolerant buffer on RX, powered from 3.3 V (for example a 74LVC1G17), with a series
  resistor in front of it, makes RX survive 5 V. That series resistor also limits current
  if someone plugs in an RS-232 port by mistake.
- **TX** with a series resistor is enough. 3.3 V high meets an ATmega's 0.6xVCC = 3.0 V
  threshold, though only marginally.
- **ESD** on both lines, like the USB pair.
- **RS-232 mounts** (Celestron/SynScan hand controllers) belong on an external adapter
  cable with a MAX3232, not on the board.
- **No RTC and no USB-host port are needed** (see "Time" and "Getting the hint").
- **The LX200 header does not reach a stock OpenAstroTech mount.** OAT only takes
  commands on USB and on Wi-Fi (see "OpenAstroTech, read from source"). Keep the header for
  other LX200 mounts, but OAT needs a firmware change, Wi-Fi, or USB host.
- **Wi-Fi: the production version will have a module** (user, 2026-10-15). It also serves
  the phone, and it is a possible OAT link to ESP32-based mounts. The PCB notes still say
  "not in v1", with SDIO_M1 (pins 60–65) left free. Two checks for the PCB session:
  - Since the 2026-10-14 correction, VCCIO6 runs at **1.8 V**. The chosen module's SDIO IO
    must support that.
  - Whether the module needs a 32.768 kHz low-power clock, which the RV1103 cannot supply
    from an RTC.

---

## Files here

- `HANDOFF.md`: current state, decisions, how to run, open items. **Read first.**
- `README.md`: this feasibility note.
- `src/`: the solver port (`astap_solve.c`, `astap_db.c`, MPL-2.0), the catalogue writer
  (`catalog.c`), the host test (`solve_test.c`, `make test`) and the board bench
  (`solve_bench.c`, `make board`; cases and catalogue come from `solve_test -k DIR -w FILE`).
- `STANDALONE.md`: the phone-only use case. The camera acts as the mount's "second PC" (time
  and site to the mount, goto, solve, sync, center). Also covers the two-masters problem when a
  PC is present, and the small OAT firmware changes this implies.
