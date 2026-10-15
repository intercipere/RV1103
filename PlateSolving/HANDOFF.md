# Plate solving: handoff (end of session 2026-10-15)

Read this first, then `README.md` for the detail and evidence, and `STANDALONE.md` for the
phone-only / mount-control design. Committed in `f2b6a14d3` (2026-10-15), after the
2026-10-07 guiding work in `a5ae70a36`.

## State in one paragraph

Hinted plate solving on the RV1103 works, on a synthetic sky. ASTAP's solver core is ported
to C (`src/astap_solve.c`, `src/astap_db.c`, MPL-2.0, from upstream SourceForge changeset
`4ea2c2f5`). It passes a host test matrix (`make test`) and was benchmarked on the board
with `alpacad` running:

- **~60 ms per solve and 0.64 MB peak RSS**;
- **~0.2" centre accuracy**;
- **no wrong solve in ~1800 trials**;
- failures only in fields with under 10–15 detected stars.

Not yet done: real sky frames, a real catalogue, integration into `alpacad`.

## Files

| Path | What |
|---|---|
| `README.md` | Feasibility, candidates, star density, catalogue licence, NPU, hint sources, time, measurements, plan, PCB notes |
| `STANDALONE.md` | The camera as the mount's "second PC": time/site to the mount, goto/solve/sync, PC-is-primary arbitration, polar-alignment lease, OAT firmware changes |
| `src/astap_solve.[ch]` | Port: quads, matching, lsq fit, spiral search, second pass. MPL-2.0 |
| `src/astap_db.[ch]` | Port: `.1476` tile reader and four-tile area split. MPL-2.0 |
| `src/catalog.[ch]` | Our `.1476` writer, the start of the real catalogue builder |
| `src/solve_test.c` | Host test: synthetic Gaia-density sky, failure modes, independent truth projection |
| `src/solve_bench.c` | Board bench: solves cases written by `solve_test -k DIR -w FILE` |

How to run:

```
cd PlateSolving/src && make test                       # host, ~10 s, prints PASS/FAIL
./solve_test -d 0.35 -c 12 -m 11.5 -n 300 -o 5 -v      # one configuration, verbose
export PATH=$PWD/../../tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH
make board                                             # solve_bench.arm
./solve_test -d 1 -c 12 -m 11 -n 40 -k /tmp/cat -w /tmp/cases.txt
#   then tar + adb push to the board's rootfs (not /tmp: it is RAM), run
#   solve_bench.arm <catdir> <cases.txt>, and delete it again (~15-20 MB)
```

The upstream ASTAP sources used for the port were fetched into a session scratch directory,
which is gone. Re-fetch single files with
`https://sourceforge.net/p/astap-program/code/ci/default/tree/<file>?format=raw`.
`hg` is not installed on this machine.

## Decisions made by the user this session

- **The ESA/Gaia catalogue licence is deferred.** The user has ESA contacts and will ask
  later. Prototyping proceeds without it.
- **Production gets a Wi-Fi module.** The standalone user runs everything from a phone or
  tablet.
- **The user maintains OpenAstroTech.** Firmware, ASCOM driver and OATControl changes are
  all in scope.
- **A connected PC is always primary for the mount.** The camera never proxies the mount.
  Arbitration lives in OAT firmware: the PC claims the mount and the camera's channel turns
  read-only.
- **Exceptions to read-only:** the camera's guide pulses always pass. Auto polar alignment
  may slew under a mount-granted lease.
- **Location** is read from the mount's EEPROM, shown on the page and editable; edits are
  written back. **Time** comes from the phone's browser and is pushed to the mount.

## Findings worth not re-deriving

- **A solve needs an RA/Dec hint, not the site location.** Location only refines polar
  alignment (alt/az wording, refraction).
- **The RV1103 has no RTC.** A hinted solve needs no time. Sync and polar alignment need
  the date to about a month.
- **Browser geolocation does not work** on `http://169.254.x.y` or `http://192.168.4.1`
  pages: it requires a secure context.
- **OAT accepts Meade commands only on USB and Wi-Fi (ESP32), not on a spare UART**
  (`f_serial.hpp`). The camera's LX200 header cannot reach a stock OAT. OAT does not keep
  time: it boots at 2021-01-01, so its RA is wrong until something sets the clock.
- **The NPU is not useful here.** Only star extraction is convolution-shaped, it is
  already under 1 s on the CPU, and NPU buffers come out of the CMA pool we want to shrink.
- **The PCB baseline is the RV1103G1 (64 MB),** so the CMA trim (24M to ~14M, about
  +10 MB of RAM) matters for production.
- **Galactic-pole fields need the camera to reach ~V 11.5.** V 10.5 fails ~35% there.
- **~1% of solves have 30–60" corner error** from false quad matches. ASTAP has no residual
  rejection. The centre stays fine and `resid_max` flags these.
- **Search radius trades time, not RAM:** ~5 ms per 1° window on the board. 20° is a
  6.6 s worst case. Use 5–10° with a mount hint. A blind solve is plausible in minutes,
  but untested (see README).
- **Corrected this session:** the GitHub ASTAP mirror is *not* modified. Its "2026 perf"
  comments are upstream's own.

## Open items, in suggested order

1. **Cross-check against ASTAP's real D05 database on the host** (102 MB download from
   SourceForge `star_databases/`). The reader already understands its format. This tests
   the port on real sky structure without the board.
2. **Real sky frames.** These settle the limiting magnitude. Solve each with desktop ASTAP
   too and compare.
3. **A star-list mode for `guide_star_autofind()`** (`alpaca/src/guide/guide_star.c`). It
   currently drops saturated and near-edge stars, which is right for guiding. The solver
   matches the brightest image stars against the brightest catalogue stars, so it wants
   those kept. Read ASTAP's `find_stars` (`unit_star_align.pas`) first, per the port rule.
4. **Integration into `alpacad`**: a solve action plus API, results on the guide page,
   search radius as a setting.
5. **A real catalogue builder** (host tool around `catalog.c`), once the licence is clear.
   It will be ~14 MB at V<12.
6. **Measure the wide-radius false-match rate** before offering a blind solve, which also
   needs two seeds (the spiral covers ±90° of RA).
7. **Mount side** (`STANDALONE.md`): the LX200 client, shared with guiding's
   `guide_mount_pulse()` backend; OAT firmware arbitration; then goto / solve-sync-center,
   then polar alignment, ported from an established routine.

## Things waiting on another session

The PCB session has not yet been told the plate-solving implications. Pass these on:

- The LX200 RX on J203 needs 5 V tolerance (buffer + series R) and ESD.
- The LX200 header cannot reach a stock OAT.
- The Wi-Fi module's SDIO must work at 1.8 V (VCCIO6), and its 32 kHz clock needs checking.
- No RTC and no USB host port are needed.

A ready-to-paste note is in `README.md`, "What this means for the PCB".
