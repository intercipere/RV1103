# On-device guiding: status, effort, and the mount question

Consolidates what has been built and measured, how much is missing, and what each missing
piece actually costs. Companion documents: `Guiding/README.md` (the feasibility work and the
numbers behind it) and `alpaca/src/guide/README.md` (how the implementation is put together).

Everything marked *measured* was measured on the real board. Everything else is an estimate
and is labelled as one.

---

## Where it stands

Working and hardware-verified:

- **Star detection and centroid tracking**, ported from PHD2 (BSD 3-clause). `guide_star_find`
  runs in **0.18 ms/frame**; centroid error against injected ground truth is **0.009 px mean,
  0.014 px worst** over 20 frames, 0 lost. Board and host agree to every printed digit.
- **Star auto-selection**, restructured to run in bands: **321 ms and 0.69 MB** at 4x
  downsample, where PHD2's own arrangement needs 35.8 MB and is OOM-killed here.
- **Four guide algorithms** — hysteresis, lowpass, resist-switch, identity — with PHD2's own
  default parameters.
- **The loop, inside `alpacad`**, sharing the V4L2 device with the Alpaca exposure worker
  under a new `v4l2_exposure_lock()`. Verified: four full Alpaca `ImageBytes` captures served
  correctly while the guide loop ran concurrently.
- **A page at `/guide`** with live preview, error graph, star cycling, and a camera/simulated
  frame-source toggle.

Not working, because it does not exist: **calibration** and **mount output**. Both are
required before this guides anything. Today it measures guiding; it does not perform it.

---

## The mount question

> For mount motion I suppose there is an Alpaca function with ST4?

Nearly — but the direction matters, and it changes what needs building.

**ASCOM's Camera interface does have pulse guiding**: `CanPulseGuide`, `PulseGuide(Direction,
Duration)` and `IsPulseGuiding`, specified for "a camera that can directly send auto-guider
pulses to the telescope mount via an electrical connection". Directions are
`guideNorth`/`guideSouth`/`guideEast`/`guideWest`. PHD2's own Alpaca client implements it and
surfaces it as the **"On camera"** mount option.

But that is the **inbound** direction: a PC client telling our camera to emit a pulse. Our own
guide loop does not need Alpaca at all — it is already inside the box and would call the GPIO
driver directly. **Alpaca is the remote-control wrapper, not the mechanism.**

So the shape is: write the pulse driver once, expose it twice.

```
                 guide_mount_pulse(dir, ms)
                            |
        +-------------------+-------------------+
        |                   |                   |
   ST-4 GPIO          LX200 serial       Alpaca Telescope
   (4 opto lines)     (:MGn0500#)        client (PC-hosted mount)
```

...with two callers: our own guide loop, and `PulseGuide` on the Camera device for a PC-side
PHD2 using "On camera". Same driver, and building it serves both.

### Backend 1 — ST-4 over GPIO

Four optocoupler outputs, RA+/RA-/Dec+/Dec-. Universal: every tracker with an ST-4 jack takes
it, which is the "guide a tracker with no computer" case.

The GPIO mechanism is already proven in this codebase — `switch_api.c` drives
`/sys/class/gpio` for the dew heater with env-configured pins and graceful degradation.

Two real caveats. **Only 3 GPIOs are free on the Luckfox prototype**, so a true four-line port
is PCB work; the logic can be written and checked against a logic analyser first. And **pulse
timing needs measuring** — a sysfs write plus `usleep` should hold millisecond accuracy, but
Linux scheduling jitter on a single-core box under a capture load is exactly the kind of thing
that is fine until it is not. A 200 ms pulse that lands as 215 ms is a 7% rate error.

### Backend 2 — LX200 serial, for OpenAstroTech mounts

> For connection with OAE or other OpenAstroTech mounts there might have to be a custom
> interface.

Good news: **no custom protocol needed.** OAT firmware already speaks Meade/LX200, and the
guide-pulse command is in `src/core/meade/MeadeParserMovement.cpp`:

```
:MG<dir><DDDD>#     dir = n|s|e|w,  DDDD = 4-digit duration in ms
:Mg<dir><DDDD># is accepted identically
```

Nine bytes, no response on success. This is the standard LX200 `:Mg`, so the same backend also
drives any LX200-compatible mount, not just OAT — a wider win than it first looks.

Note the 4-digit field caps a single pulse at **9999 ms**, which is far beyond anything
guiding uses.

The SoC has **six UARTs** (`uart0`–`uart5` in `rv1106.dtsi`; `uart2` is the debug console, so
five are free). Two pins instead of four plus optocouplers, and it carries mount state rather
than just pulses.

**Recommendation for the PCB: fit both.** An ST-4 jack for universal compatibility and a UART
header for OAT/LX200. The marginal BOM cost is a connector and four optocouplers; the
capability difference is large, and this decision gets expensive to revisit once boards exist.

### Backend 3 — Alpaca Telescope client

The RV becomes a client of a mount exposed over Alpaca. PHD2's `scope_alpaca.cpp` is a
complete working reference including the `CanPulseGuide` / slew-check / `IsPulseGuiding`
handshake. It works, but it needs an ASCOM Remote server on a PC — which reintroduces exactly
the dependency that makes on-device guiding worth doing. Worth having eventually; not first.

---

## What is missing, and what each costs

Estimates, not measurements. "Bench-testable" means it can be verified without a mount and
without sky — which on this project has repeatedly been the difference between a day and a
month.

| Piece | Effort | Bench-testable? | Notes |
|---|---|---|---|
| **ST-4 GPIO driver + Camera `PulseGuide`** | ~150 lines, **easy** | Partly | Pattern exists in `switch_api.c`. Needs a logic analyser, then the PCB. Measure pulse timing jitter. |
| **LX200 serial backend** | ~120 lines, **easy** | Partly | Syntax confirmed above. Testable against a serial loopback or a real OAT. |
| **Calibration** | ~600-line port, **medium to write, hard to validate** | **No** | Reuse PHD2's directly — see below. |
| **Dec guide mode** (auto/N/S/off) | ~20 lines, trivial | Yes | Gate the correction by sign. |
| **Dithering + settle handshake** | ~150 lines, easy-medium | Yes | Offset the lock position; wait for error below threshold for N frames. Needed for any sequencer. |
| **Lowpass2, ZFilter algorithms** | ~150 lines each, easy | Yes | Direct ports, same shape as the four already done. **Port them, do not write them** -- see below. |
| **Gaussian Process / PPEC** | 48 KB + Eigen, **hard** | Yes | Heaviest thing in PHD2. Optional there too. Probably never worth it on this CPU. |
| **Star mass-change rejection** | ~40 lines, easy | Yes | Stateful check PHD2 does in the guider, not in `Find`. Cheap insurance against latching onto a passing satellite or cloud edge. |
| **Multi-star guiding** | ~200 lines, medium | Yes | Detection already returns a ranked list; needs averaging plus outlier rejection. |
| **Bad-pixel map** | ~100 lines, easy | Yes | Sparse list of coordinates. **Prefer this over a dark library here** — see memory note. |
| **Dark library** | ~200 lines, medium | Yes | A dark frame is 5.97 MB resident. On 32 MB that is a real cost; on the 256 MB part it is not. |
| **Backlash compensation** | ~300 lines, medium | **No** | Needs a real mount with real backlash. |
| **PHD2 event server on :4400** | ~600-800 lines, medium | Yes | 22 event types + ~30 methods for N.I.N.A. No new dependencies. Only useful once guiding works. |
| **Guide log** | ~150 lines, easy | Yes | PHD2's log format is well documented and third-party analysers read it. |
| **Subframes / binning** | medium-hard | Yes | V4L2 cropping is not implemented at all — a driver-level change, not an app one. Also the single largest latency lever. |
| **Guiding Assistant, Drift/Polar Align, Star-Cross** | each a project | No | Analysis tools. Low priority for a guider appliance. |

### Calibration: reuse PHD2's, and the difficulty is not where I first put it

An earlier version of this document rated calibration "hard" at ~400-600 lines. That
conflated two different things, and the writing half was wrong. Measured against the actual
source:

`Scope::UpdateCalibrationState()` in `scope.cpp` is **586 lines**, of which:

- **391 lines are pure logic** — geometry, state transitions, arithmetic.
- **73 lines** touch `pFrame->Alert`, `Debug.Write`, `GuideLog`, `EvtServer`, `wxString`,
  `pConfig` — logging, notification and config shells, exactly the layer stripped out when
  `star.cpp` was ported.
- It calls out to the mount **four times in total**: `CanPulseGuide()` twice, `SideOfPier()`
  once, `GetGuideRates()` once.

That is remarkably self-contained for a state machine of that size, and it is BSD-licensed.
**So yes — port it rather than reinvent it.** The structure is a per-frame state machine
driven by the current star position:

```
CLEARED -> GO_WEST -> GO_EAST -> CLEAR_BACKLASH -> GO_NORTH -> GO_SOUTH -> NUDGE_SOUTH -> COMPLETE
```

Each step pulses, measures how far the star moved, and either continues, advances, or fails
with a specific reason ("RA Calibration Failed: star did not move enough" and friends). That
is a direct port of the same shape and difficulty as the star-detection port already done.

**What stays genuinely hard is validation, and no amount of code reuse changes it.** It needs
a mount that actually moves, a real star, and a clear night. The failure paths — star lost
mid-calibration, insufficient movement, backlash mis-measured, a target too near the pole —
only exercise on sky. Every line written so far could be checked indoors in daylight, and
that is precisely why it went quickly. This is where that stops.

So the revised rating is **medium to write, hard to validate**, and the plan should budget
nights rather than hours.

### Port, do not reimplement -- a lesson already paid for

Of the four guide algorithms first written here, two were taken from PHD2's source and two
were written from a description of what the name implies. **One of those two was wrong.**
The resist-switch implementation was a reversal counter; PHD2's is a windowed sign-consensus
with a "not getting worse" test, a fast-switch path and an overshoot veto. It has since been
ported properly. Algorithm defaults were also applied globally rather than per algorithm,
which under-corrected the Dec axis by 30%.

Neither would have produced an obvious failure. Both would have shown up as mediocre guiding
on sky, which is the most expensive place to debug. The rule for everything remaining:
**read the source first, port it, and keep PHD2's constants.**

### What the mount can tell us back changes what calibration can do

PHD2's calibration adapts to the mount's capabilities, and this is where the two backends
genuinely diverge:

- **ST-4 is write-only.** Four wires, no telemetry at all. No declination, no side of pier,
  no guide rates. Declination compensation (RA rate scales with `cos(dec)`) cannot be
  automatic, and a meridian flip cannot be detected. PHD2 supports such mounts, so the
  degraded path exists and works — but it is the degraded path, and the user has to supply
  what the mount cannot.
- **LX200 serial reports back.** Confirmed in OAT's `MeadeParserGet.cpp`: `:GR#` current RA,
  `:GD#` current Dec, `:GX#` mount status, plus site latitude and longitude. Declination
  alone enables proper rate compensation; status and coordinates make flip awareness
  possible.

This is a stronger argument for fitting the UART header than the earlier "two pins instead of
four" one. ST-4 makes the mount move; serial makes calibration *better*.

---

## Suggested order

1. **ST-4 GPIO driver + Camera `PulseGuide`.** Smallest real deliverable, unblocks everything,
   and immediately useful to a PC-side PHD2 via "On camera" before any on-device guiding
   exists. Measure the pulse timing jitter while implementing.
2. **LX200 serial backend**, behind the same interface. Cheap once step 1 defines the shape.
3. **Star mass-change rejection and Dec guide mode.** Hours of work, meaningful robustness.
4. **Calibration**, ported from PHD2's `Scope::UpdateCalibrationState()`. Still the long
   pole, but because of where it must be tested, not because of how much must be written.
   Everything before this is desk work; this is not.
5. **Dithering + settle**, then the **event server**, which together make the thing usable
   from N.I.N.A. or SGP.
6. Refinements: multi-star, bad-pixel map, remaining algorithms, guide log.
7. Binning/subframing when latency matters — it is also the largest remaining latency lever.

## Hardware decisions this implies

- **ST-4 jack *and* a UART header on the PCB.** Both are cheap; only one is universal, and
  only the other lets the mount report declination — which is what separates compensated
  calibration from the degraded path.
- **Four routed GPIOs** for ST-4. The prototype has three free, which is why the success path
  cannot be verified before the board exists.
- **RV1106G3 (256 MB)** removes every memory constraint documented here. The dark library, the
  full-resolution `AutoFind`, and a separate guide process all become non-issues. None of them
  should drive the BOM on their own, but none of them argue for the smaller part either.
