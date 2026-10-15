# Standalone operation: the camera as the mount's "second PC"

Planning notes, 2026-10-15. Nothing here is built. The user maintains OpenAstroTech (OAT), so
mount-side firmware changes are an option the camera design can rely on, not a workaround.

**Use case:** a DSLR imager with no PC. A phone or tablet opens the camera's web page over Wi-Fi.
From there the user polar-aligns, slews to a target, solves, syncs and guides. With no PC
present, the camera is the only thing that can move the mount. So the page needs at least a
simple goto, and the camera needs most of an LX200 client.

---

## There is no chicken-and-egg, only an order

The phone's clock is available the moment the page loads, before anything else happens. So the
chain is linear:

```
page loads ── Date.now() ──► camera ── :SC/:SL/:SG ──► mount clock valid
mount EEPROM ── :Gt/:Gg ──► camera ──► page shows site, user may edit ── :St/:Sg ──► mount
                       (mount now has time + site, so its own RA/Dec model is valid)
polar alignment:   hint = the pole (needs only hemisphere; the mount's model isn't even used)
goto:              page target ── :Sr/:Sd/:MS ──► mount slews on its own model (a few ° off)
plate solve:       hint = :GR/:GD ──► solve ──► true RA/Dec ── :Sr/:Sd/:CM ──► mount synced
center:            re-slew + solve + sync until within tolerance
guide:             existing guide loop; pulses as :Mg<dir><ms># on the same link
```

A clock that was never set can be detected without a firmware change: OAT boots at
**2021-01-01** (`Mount.cpp`), so `:GC#` returning that date means "unset". An explicit flag
would be cleaner (see the firmware list below).

---

## What this adds to the camera

Rough sizes, **estimated**, and small next to what already exists (HTTP server, settings store,
panel-based page, guide loop):

| Piece | What | Estimate |
|---|---|---|
| Mount link | Byte transport over UART or TCP (Wi-Fi), with timeouts and reconnect | small |
| LX200 client | time/site get+set, position, slew/abort, sync, jog (`:Mn/s/e/w`, `:Q`), home/park, guide pulse, plus OAT `:X` extensions where useful | a few hundred lines of C |
| Astro math | JNow↔J2000 precession, LST, alt/az for horizon checks | small. OAT's own `src/core/{CalendarMath,SiderealClock,CoordinateMath}.cpp` are 85/65/45 lines, MIT, and already match the mount's conventions |
| Object list | Messier + bright NGC/IC + named stars for the target search | a few hundred kB of flash |
| Goto panel | search or RA/Dec entry, slew, abort, jog buttons, status | a new panel in the existing page framework |
| Solve → sync → center | the loop above | small, once the solver exists |
| Polar alignment | two solves around the RA axis, axis vs. pole, live adjust | medium. Port an established routine; do not invent one |

**Most of the mount link is already on the guiding to-do list.** `Guiding/ROADMAP.md` has
"LX200 backend behind `guide_mount_pulse()`" as item 1. The same client carries goto and sync, so
these are a few more commands, not a second subsystem. Build it once, with the transport
separated from the protocol.

---

## Two masters: a connected PC is always primary (user decision, 2026-10-15)

When a PC is present, the user is running PHD2/N.I.N.A. there, so **the PC is primary**. The
camera's mount features exist for the no-PC case and step back when a PC appears. The camera does
*not* proxy the mount to the PC: an Alpaca Telescope pass-through was considered and rejected for
this reason.

**Arbitrate in the mount, not in the camera.** Only OAT sees both channels: its USB, where the PC
is, and the spare UART or Wi-Fi link, where the camera is. A rule enforced there cannot be bypassed
by a camera bug or a race. A sketch, for the maintainer to shape:

- **Primary vs. secondary channel.** USB (or whichever channel the ASCOM driver / OATControl uses)
  is primary. The camera's channel is secondary.
- **The PC claims the mount.** The ASCOM driver and OATControl send a claim on connect and release
  it on disconnect. A timeout covers a PC that vanishes without disconnecting (cable pulled,
  laptop asleep).
- **While claimed, the secondary channel is read-only.** Reads (`:GR/:GD`, status, site, time)
  still work, so the camera can display position and use it as a solve hint. Writes (slew, sync,
  jog, park, time/site set) get a distinct error the camera can show: "mount is controlled by a
  PC".
- **The camera can query the claim state** and grey out its goto/sync/PA controls instead of
  waiting for an error.
- Each channel needs its own command parse buffer. Two channels must never interleave bytes
  within one command.

**Guide pulses always pass on the secondary channel** (user decision, 2026-10-15). If the camera
is guiding, the PC cannot guide on its own, so blocking the camera's `:Mg` would just stop guiding.
Double guiding is avoided **on the camera side** instead:

- **The camera is connected to the PC as a camera** (an Alpaca Camera client is connected; PHD2
  uses it as its guide camera). Then the PC guides: PHD2 sends pulses through its own mount
  driver, and the on-device guide loop stays off. `alpacad` already knows whether a camera client
  is connected, so this needs no mount support.
- **Otherwise the camera guides and its pulses go straight to the mount.** That covers no PC at
  all, and a PC that only talks to the mount. It also covers N.I.N.A. driving the on-device guider
  through the planned PHD2-compatible server on :4400, which is not a camera connection.

**Exception to read-only: automated polar alignment.** The camera's polar-alignment routine must
be able to move the mount even with a PC connected (user decision, 2026-10-15). It rotates RA
between solves, then drives OAT's motorised AZ/ALT adjusters (`onMoveAzimuth`/`onMoveAltitude`,
which already exist). Proposed: a **lease**, not a free-for-all.

- The camera requests a polar-alignment lease from the mount. The mount grants it only if the PC
  is not slewing at that moment. While the lease is held:
  - The secondary channel may slew RA/Dec and move AZ/ALT.
  - Sync, park, and time/site writes stay blocked. The PC owns the pointing model.
- The mount's status reports the lease, so the ASCOM driver and OATControl can show "polar
  alignment in progress (camera)". The PC's own motion commands get a busy error until the lease
  ends, rather than fighting the routine.
- The lease ends when the camera releases it on completion or abort, or after a timeout if the
  camera disappears. The camera returns the mount to where it started before releasing. The
  routine's RA rotation is undone, so the PC's model still matches.
- **Who starts it:** from the camera's page; possibly also from OATControl or a N.I.N.A. plugin
  via a command to the camera. Either way the mount arbitrates.

Everything else stays read-only for the camera while the PC holds the claim: no goto, no sync, no
time/site writes.

---

## OAT firmware changes this implies

All small, and the maintainer's call:

1. **Process Meade commands on a configurable spare hardware UART** as well as `Serial`
   (`f_serial.hpp`). Without this the camera's header cannot reach Mega/RP2040 OATs. Which UART
   is free varies by board (`boards/*/pins_*.hpp`: Serial1–3 go to GPS, TMC, focuser or debug).
2. **A "clock has been set" flag**, so the camera need not infer it from the 2021-01-01 boot date.
   Optionally also report whether the time came from OAT's GPS, so the camera can take time *from*
   the mount when it has a better source than the phone.
3. **Channel arbitration**: PC claim; secondary read-only while claimed, except guide pulses
   (always allowed) and moves under a polar-alignment lease; a claim/lease-state query; separate
   parse buffers per channel (see "Two masters" above). This also touches the ASCOM
   driver and OATControl (claim on connect, release on disconnect).
4. Nothing else is strictly needed: site get/set and EEPROM persistence, sync, slew, guide pulses
   and time setting all exist already (`core/meade/MeadeParser{Get,Set,Movement}.cpp`).

---

## Suggested order

1. Solver core on the host, synthetic fields (`README.md`, Plan step 1). Everything above depends
   on it, and it is the only part with real technical risk.
2. Mount link + LX200 client, with guide pulses first, because guiding needs them anyway. Then time,
   site, position, slew, sync. Test against a real OAT on the bench.
3. Solve → sync → center, then a minimal goto panel.
4. Polar alignment.
5. Mount-side arbitration (claim / read-only secondary) in OAT firmware, the ASCOM driver and
   OATControl. Needed before steps 2-4 are safe with a PC on the same mount, so in practice it
   ships together with step 2.
