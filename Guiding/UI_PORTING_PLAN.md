# Porting PHD2's interface: plan

Status (2026-10-07): **steps 1, 3 and most of 4 done; step 2 (the settings registry) next.**
Companion to `ROADMAP.md`. Decisions taken with the user: one page that adapts to screen
width; movable, closable panels with the layout saved per browser (like PHD2's dockable
panes); a single equipment profile for now, keyed so profiles can be added later; colour
schemes Dark (default), OLED, Light and Night (red); guide scope focal length 182 mm.

What exists now:
- **Asset pipeline**: the page is ordinary files under `alpaca/web/`, embedded pre-gzipped by
  `alpaca/tools/embed_web.py` and served by `src/web.c` (ETag/304). `guide_serve --web
  ../../web` serves them live from disk for editing without rebuilding.
- **Panels** (`web/guide/js/layout.js`): drag by the title bar between a full-width top area,
  1-3 resizable columns and a bottom area; collapse; close; re-open from View; reset. The
  graph, image, target and star profile also resize by a grip on their bottom edge
  (double-click resets); a tall star profile switches to PHD2's focusing mode. Phone and
  desktop arrangements, sizes included, stored separately in localStorage.
- **Views ported from PHD2 source**: history graph (`graph.cpp`), target (`target.cpp`), star
  profile (`star_profile.cpp`), image overlay, toolbar (exposure list, gamma), status bar.
- **Settings store** on the device under PHD2 keys (`guide_settings.c`, `/guide/settings`),
  persisted to `/userdata/guide.cfg`. One setting so far: `/frame/focalLength`, default 182.
- **Not yet**: the registry itself (step 2), clicking the image to select a star, the inline
  algorithm controls under the graph (they come with the registry), Stats window details
  (peak/limit counts), dithering labels on the graph (no dithering yet).

## The problem

PHD2 cannot run on this device. 136 of its 141 source files include `phd.h`, which pulls in
`<wx/wx.h>`; it is a wxWidgets desktop program needing GTK, a display server and far more
than the ~32 MB of RAM here. Its guiding *logic* has been ported line by line, and that works.
Its *interface* has not been: the current `/guide` page was designed from scratch, which is
the UI equivalent of the hand-written resist-switch algorithm that turned out wrong.

Doing it setting by setting by hand does not scale. PHD2 reads ~300 distinct configuration
keys (224 literal, ~80 built from a prefix), its Advanced Settings manual page alone is 6,500
words, and many settings serve setups and workflows nobody on this project uses -- exactly
the ones most likely to be skipped or misunderstood if each is hand-built.

## The approach: port the interface as data, from PHD2's own sources of truth

PHD2 already contains a complete specification of its interface, in three places, all
BSD-licensed in the same repository:

| What | Where in PHD2 | Gives us |
|---|---|---|
| every setting's key, type and default | `pConfig->Profile.Get*("/key", default)` calls | storage, defaults |
| label, tooltip, range, step, grouping | the dialog code (`AddTableEntry`, `MakeSpinCtrlDouble(..., min, max, val, inc)`, the Advanced dialog tab classes) | the form |
| what each setting means and when to touch it | `help/*.htm`, the user manual | help text |

So instead of building settings one at a time:

1. **Extract** a settings registry (`guide_settings.json`) from a pinned PHD2 commit with a
   script kept in the repo: key, type, default, range, label, tooltip, tab and group, manual
   excerpt, source `file:line`. Defaults that are named constants get resolved by the script;
   whatever it cannot resolve is reviewed once by hand and recorded. Re-running against a new
   PHD2 release shows what changed.
2. **Store** settings on the device under **PHD2's own keys** (`/scope/CalibrationDuration`,
   ...) in one file on `/userdata`, with `GET`/`PUT /guide/settings`. The C code reads values
   by key, so no per-setting plumbing.
3. **Render** the settings UI from the registry. Every PHD2 setting appears automatically, in
   PHD2's tabs and groups, with PHD2's own tooltip wording. Each carries a status:
   - *active* -- ported and wired;
   - *not yet ported* -- shown, disabled, labelled so. The UI doubles as an honest gap list;
   - *not applicable* -- e.g. ASCOM chooser, language, Windows software update. Hidden, but
     listed in the registry with the reason, so the omission is a decision rather than an
     accident.

The C side still has to *implement* each feature behind a setting; nothing changes that. What
changes is that nobody hand-writes forms, defaults, ranges or help text again, and nothing
can be silently left out.

## Two layouts, one page

What real users do, from PHD2's own *Best Practices* (Waddington & Galasso, 2019): set up a
profile once (pixel size, guide focal length), build a dark library, loop, **auto-select**,
calibrate **once and re-use it**, guide at **2-4 s exposures**, run the **Guiding Assistant**
for min-move and backlash, and otherwise "always start with default settings" and "be
conservative with adjustments". The advanced dialog is mostly left alone.

- **Phone** (narrow screens): image with the overlay, compact graph, RMS, the four toolbar
  actions (Loop, Auto-select, Guide, Stop) and exposure. Target and star profile as swipeable
  panels; all settings in a full-screen sheet rendered from the registry.
- **Desktop** (wide screens): PHD2's main window -- history graph across the top with its
  own controls, image in the centre, Target and Star Profile docked on the right, toolbar at
  the bottom, status bar, Advanced Settings as a tabbed dialog. Close to a direct copy.

Same page and API, chosen by screen width with a manual override. Existing precedent for the
phone side: Mobindi, a responsive PHD2 front end (github.com/pludov/mobindi).

## Views to port, each from its PHD2 source

| View | PHD2 source | Notes |
|---|---|---|
| Overlay | `guider.cpp`, `guider_multistar.cpp` | **done**: box and lock-crosshair colours/styles per state |
| History graph | `graph.cpp` | **done**: x 50-400, y +/-1-16, px/arcsec, RA-Dec/dx-dy, Trendlines, Corrections (+ to scale), RMS as population sigma, RA Osc. Missing: inline algorithm controls (registry), line colour pickers, dither labels |
| Target | `target.cpp` | **done**: rings, zoom 0.25-4, length, Clear, reference circle |
| Star profile | `star_profile.cpp` | **done**: mid/avg row/avg col, FWHM, Peak, HFD (large in focusing mode), zoom with raw/interpolated -- interpolated is wxWidgets' `ResampleBicubic` (cubic B-spline) ported, since the browser's bilinear scaling looked visibly sharper than PHD2. One deliberate difference: zoom from full-resolution pixels, see the file header. Device sends a 41x41 raw crop (3.4 kB) per frame |
| Other overlays | `guider.cpp` | not yet: bullseye, grids, RA/Dec axes |
| Stats | `statswindow.cpp` | partly: a Stats panel with the basics |

Arc-second units need the guide scope focal length (user setting, PHD2's profile wizard asks
for it) and the pixel size (a property of the sensor, so it belongs in `sensor_desc_t` for
both SC3336 and the IMX).

## Prerequisite: get the page out of a C string

The page is ~300 lines inside a C string literal in `guide_api.c`. A PHD2-scale interface
would be thousands. Move it to ordinary `.html`/`.css`/`.js` files in the repo, embedded into
`alpacad` at build time as a generated C array and served **pre-gzipped** (the browser
decompresses; no zlib on the device). `guide_serve` serves the same files on a host.

## Order

1. Asset pipeline (page out of the C string). Everything else depends on it.
2. Registry extraction + settings store + generated settings UI. Wire the settings already
   implemented (calibration step/distance, max RA/Dec pulse, Dec mode, algorithms and their
   parameters, min-move, search region, exposure). **Persist calibration** -- the Best
   Practices are explicit that it should be re-used, and today it dies with the daemon.
3. Graph controls, Target, Star Profile (+ focal length for arc-seconds).
4. Desktop layout.
5. Features behind "not yet ported" settings, in `ROADMAP.md` order -- mount backends first.

## Open decisions

- **One page with a breakpoint, or two URLs?** Recommend one page.
- **Equipment profiles.** PHD2 keeps one per gear combination. Recommend a single profile
  first, keyed exactly as PHD2's so profiles can be added without migrating anything.
- **Night colours.** PHD2's palette is bright (blue, green, yellow). A red night-mode toggle is
  common in field software; worth adding later as a display option, defaulting to PHD2's.
