# OpenAstroGuider Alpaca driver

An ASCOM Alpaca Camera server for this project's raw V4L2 capture pipeline. Runs **on the RV1103/RV1106
itself**, not on the PC — that's the point of Alpaca (as opposed to classic ASCOM/COM): any device with an
HTTP stack and a network link can *be* the driver, discoverable and controllable directly by Alpaca clients
(N.I.N.A., PHD2, etc.) with no PC-side driver install. Transport is usb0, a USB RNDIS gadget (see repo `CLAUDE.md`, "OpenAstroGuider project context").

Status: **working vertical slice, hardware-verified 2026-09-15, survives a real reboot.** Discovery,
management API, common device API, and the full camera exposure cycle (`startexposure` → `imageready` →
`imagearray`) all tested against the real SC3336: correct dimensions (1296×2304), valid 10-bit pixel range
(57–1023), well-formed JSON, `ExposureMax`/`ExposureMin` matching independently-verified sensor timing
exactly. `alpacad` is baked into the image (stripped binary + `S60alpacad` init script) and **confirmed
auto-starting after an actual physical reflash + power cycle**, not just build-verified — checked via
`ps` after reboot. No imaging pipeline beyond raw capture: no on-device dark/flat calibration, debayering,
or stacking, by design (see "Architecture note" below). Not yet tested against a real Alpaca client
(N.I.N.A./PHD2/ASCOM Conformance) — see "Known gaps" at the end.

## Architecture note: no on-device processing, by design

`alpacad` only does raw capture → RAW10 unpack → serve via `imagearray`. There's deliberately no dark/flat
calibration, debayering, or stacking on-device — that matches both this project's original architecture
decision (raw frames shipped to the PC, see repo `CLAUDE.md`) and Alpaca's own design philosophy: a camera
driver hands back raw pixel data, calibration/processing is the client's job (N.I.N.A./PHD2 already do
this). `grab.py`'s old debayer/mono-conversion/PNG logic was never wired into `alpacad` and isn't planned
to be — it's retired PC-side tooling (see repo `CLAUDE.md`).

## Networking: IPv4 link-local, not a fixed IP (added 2026-09-15)

The stock `S50usbdevice` script hardcodes usb0 to a static `172.32.0.70/16` — which means every PC that
connects needs someone to manually configure a matching static IP, a real production usability problem
(confirmed the hard way this session: host-side interface names change every reconnect since the gadget's
host MAC is randomized each boot, so even a one-off `sudo ip addr add`/udev rule needs constant upkeep).

Fixed via a new overlay script, `etc/init.d/S51usb0-linklocal`, positioned right after `S50usbdevice`:
it simply clears the hardcoded static address (`ifconfig usb0 0.0.0.0`) and lets **`dhcpcd`** (already
running, started by `S41dhcpcd`) take over. `dhcpcd` already implements the full DHCP-then-IPv4LL
(RFC 3927 / "APIPA") fallback correctly — confirmed via its own log: `probing for an IPv4LL address` →
`using IPv4LL address 169.254.x.x` → `adding route to 169.254.0.0/16`. It was racing with and losing to
`S50usbdevice`'s later static assignment the whole time; removing that static assignment was the entire
fix. No new code was needed — this reuses already-correct, already-running software.

**Why this matters for production**: IPv4 link-local is the standards-based, zero-configuration answer to
"two devices connected point-to-point with no DHCP server." Windows, macOS, and Linux all self-assign a
compatible `169.254.0.0/16` address automatically in this situation — no drivers, no udev rules, no manual
IP entry, on any OS. A production user should never need to configure anything network-side to reach the
device. Verified stable on real hardware: settled on the same address twice, 5 seconds apart, no flapping.
Alpaca's UDP discovery broadcast (port 32227) doesn't care what the specific address is, so nothing else
needed to change — `alpacad` already binds `0.0.0.0`, not a specific address.

## Build/rootfs versioning (added 2026-09-15)

`luckfox-astroguider-oem-pre.sh` now stamps `/etc/openastroguider-version` into every build
(`built=<UTC timestamp> git=<short hash>[-dirty]`), and `alpacad`'s `driverversion` Alpaca endpoint reads
it. This exists because of a real mixup this session: `./build.sh` (bare) is correct and does rebuild
everything (confirmed via byte-identical md5sum against a manual `rootfs`+`media`+`app`+`firmware` run),
but it also archives a **new dated snapshot** into `IMAGE/<board>_<timestamp>_RELEASE_TEST/` on every run
without cleaning up old ones — six accumulated in one session, all near-identically named, and it's easy
to flash the wrong one by habit. The version stamp means "which build is this" is now answerable by
asking the device directly (`adb shell cat /etc/openastroguider-version`, or `curl .../driverversion`)
instead of trusting a folder name. **Recommendation: flash from `output/image/sd_update.img` directly**
(always current) rather than the dated `IMAGE/` snapshots.

## Flashing note: SocToolKit

This board has no onboard eMMC/NAND (confirmed via `upgrade_tool`: detects an eMMC controller but
`Flash Size: 0MB`), so it's SD-card-only and can't be flashed over USB via rockusb — the "Download"/
"Firmware" tabs in Rockchip's SocToolKit don't apply. Use the **SD Card** tab instead (confirmed via its
own `config.json`, which has a distinct `SDcard` section, and its changelog, which added "create boot or
upgrade sdcard" as a dedicated feature in v1.7) — this writes a raw disk image directly to a physically
inserted SD card via a card reader, the same operation `dd`/balenaEtcher would do. Exactly what file
format that tab expects wasn't confirmed (its "Create SD" mode has no per-file picker), but a full
format-and-reflash was confirmed working end-to-end via the version-stamp check above.

## Why C, not Python

Buildroot's Python package was deliberately dropped in the debloat pass (see repo `CLAUDE.md`) to save
~38MB on a device with a ~45MB rootfs and, soon, a 128MB flash budget. Re-adding Python just for this
would undo that work. Alpaca's wire format makes a from-scratch C implementation genuinely tractable (see
below) — no JSON parser needed for requests, and responses are small flat JSON objects for every endpoint
except the image payload.

## HTTP library: CivetWeb (MIT)

Vendored (trimmed) at `third_party/civetweb/`, from `https://github.com/civetweb/civetweb` tag `v1.16`
(the `master` branch was mid-refactor and did not compile — see `VENDORED_FROM.txt`). MIT licensed,
forked from Mongoose specifically *before* Mongoose relicensed to GPLv2/commercial, so there's no
copyleft obligation either way (relevant even though this product's software will be open-sourced
anyway — avoids the question entirely). Built with `NO_SSL`/`NO_CGI` (plain HTTP only — the usb0 RNDIS
link is a private point-to-point USB connection, not exposed to a network that needs TLS), which keeps
the dependency footprint to just `civetweb.c` + `civetweb.h` + 4 small `.inl` files (~760KB source,
~140KB compiled `.o`) — no OpenSSL/mbedTLS/Lua/zlib pulled in. The minimal file set was found empirically
by cross-compiling and adding whatever the linker/compiler actually asked for, not by tracing every
`#include` by hand.

## What Alpaca actually requires (researched 2026-09-15)

### Discovery (UDP, port 32227)
- Client broadcasts the literal ASCII string `alpacadiscovery1` to UDP port 32227.
- Device listens on 32227, replies via **unicast** to the sender's address/port with a JSON body
  containing at minimum the TCP port the HTTP API is actually listening on: `{"AlpacaPort": 11111}`.
  Clients tolerate extra fields in that JSON, so nothing else is required.
- No retry/session state needed on the device side — it's stateless request/response.

### Request format — the useful simplification
- **GET** requests: parameters are ordinary URL query-string key/value pairs
  (e.g. `/api/v1/camera/0/canslew?clientid=231&clienttransactionid=23`).
- **PUT** requests: parameters are in the body as `application/x-www-form-urlencoded` key/value pairs.
- **Request bodies are never JSON.** Only responses are. This means the device side only needs a
  query-string/form parser (trivial), not a JSON parser, for every non-image endpoint.
- `ClientID` and `ClientTransactionID` are optional query params the device should echo back if present.

### Response envelope (JSON, all endpoints)
Every response is a flat JSON object:
```json
{"Value": <result>, "ClientTransactionID": <n>, "ServerTransactionID": <n>, "ErrorNumber": 0, "ErrorMessage": ""}
```
`ErrorNumber` 0 = success; ASCOM defines a fixed set of non-zero error codes for common failure cases
(not connected, invalid value, not implemented, etc.) — full table in the ASCOM Alpaca API Reference PDF
(`https://ascom-standards.org/ASCOM%20Alpaca%20API%20Reference.pdf`) if/when precise codes matter for
client compatibility testing (ASCOM Conformance tooling checks these).

### Management API (required, device-independent)
Three endpoints clients use to discover what's on the server before talking to a specific device:
- `/management/apiversions`
- `/management/v1/description`
- `/management/v1/configureddevices` — lists device type/number/name for everything this server hosts
  (here: one camera, device number 0).

### Common device API (required for every Alpaca device, camera included)
`/api/v1/camera/0/{member}` for: `connected` (GET/PUT), `description`, `driverinfo`, `driverversion`,
`interfaceversion`, `name`, `supportedactions`. All simple, mostly static string/bool responses.

### Camera-specific minimum viable set
For a **read-only, fixed-optics, no-cooler, single-shot raw camera** (matches this project — no filter
wheel, no cooling, exposure controlled by sensor row timing not a shutter), the realistic minimum:

| Endpoint | Notes |
|---|---|
| `cameraxsize` / `cameraysize` | sensor width/height in unbinned pixels |
| `startexposure` (PUT) | params `Duration` (seconds, float) and `Light` (bool) — maps to setting sensor exposure rows via existing v4l2-ctl control path, then triggering a single-frame capture |
| `imageready` (GET) | poll until capture + on-device RAW10→Int16 unpack is done |
| `imagearray` (GET) | the pixel data — start with plain JSON array response (spec-required baseline); `ImageBytes` binary format (see below) is an optional later optimization, not required for a working driver |
| `camerastate` | Idle / Exposing / Reading / Download / Error enum |
| `stopexposure` / `abortexposure` | can both just abort the in-flight v4l2 capture; `canstopexposure`/`canabortexposure` report what's actually supported |
| `maxadu` | sensor bit depth ceiling (1023 for RAW10) |
| `sensortype` | `Monochrome` for the eventual IMX290; SC3336 prototype is actually Bayer color — **open question, see below** |
| `bayeroffsetx` / `bayeroffsety` | only meaningful while prototyping on the SC3336 |
| `binx` / `biny` / `maxbinx` / `maxbiny` | can report fixed `1` / `1` if binning isn't implemented yet |
| `pixelsizex` / `pixelsizey` | from sensor datasheet |
| `exposuremin` / `exposuremax` / `exposureresolution` | derived from the row-timing math already in the project's sensor notes (~27.45µs/row on the SC3336) |
| `gain` / `gainmin` / `gainmax` | maps to existing v4l2-ctl gain control |
| `cansetccdtemperature` / `hasshutter` / etc. | all `false` — no cooler, no mechanical shutter |

Deliberately **not** in scope for a first version: binning, subframing (`startx`/`starty`/`numx`/`numy`),
cooler control, pulse guiding, fast readout modes, `ImageBytes` binary transfer. All are valid Alpaca
members that can be added later without breaking anything already built.

### ImageBytes (optional, deferred)
A binary alternative to the JSON `imagearray` response for performance, negotiated via an `Accept:
application/imagebytes` request header. Fixed 44-byte little-endian header (11 int32 fields: metadata
version, error number, client/server transaction IDs, data-start offset, image element type, transmission
element type, rank, and up to 3 dimensions) followed by raw pixel bytes. Worth implementing once the JSON
path works and frame sizes make JSON serialization overhead actually matter — not needed for a correct,
spec-compliant driver.

## Decisions (resolved 2026-09-15)

1. **Color and mono sensors both supported via one abstraction.** `sensor.h`'s `sensor_desc_t` carries a
   `bayer_pattern_t` (`BAYER_NONE` for monochrome, or the Bayer phase) plus per-sensor dimensions, pixel
   format, control names, and unpack function. `sensor_detect()` matches the connected sensor by substring
   against `/sys/class/video4linux/v4l-subdev2/name` (e.g. `"m00_b_sc3336 4-0030"` contains `"sc3336"`) and
   falls back to a clearly-marked placeholder rather than crashing if nothing matches — an unrecognised
   sensor is a visible Alpaca-side problem (wrong capability numbers, logged to stderr), not a dead server.
   `sensortype`/`sensorname`/`bayeroffsetx`/`bayeroffsety`/`maxadu`/`pixelsizex`/`pixelsizey` all read from
   this descriptor, so adding a new sensor is a new table entry, not new dispatch logic. An `imx290` entry
   already exists as a placeholder (explicitly marked TODO/unverified) for when that hardware arrives.
2. **HTTP library: CivetWeb**, vendored — see above.
3. **`grab.py`/adb-based manual capture is no longer a constraint** — the project has moved on from it
   (testing-only, "ok if it later breaks" per project decision), so `alpacad` owns `/dev/video0` and
   `/dev/v4l-subdev2` directly via V4L2 ioctls with no coordination needed.

## Module layout

```
alpaca/
  README.md          - this file
  Makefile           - cross-compiles with the SDK toolchain; excludes src/test_*.c from the alpacad build
  third_party/civetweb/  - vendored HTTP library (MIT), see above
  src/
    main.c            - server init, route registration
    sensor.h/.c        - sensor descriptor table + runtime detection (color + mono)
    unpack.h/.c        - compact RAW10 -> uint16 unpack (validated against real captures, see below)
    v4l2_capture.h/.c  - V4L2 multiplanar capture + named control get/set/range
    device_state.h/.c  - shared camera state (connected, exposure state machine, last frame), mutex-guarded
    util.h/.c          - query-string/form parsing + Alpaca JSON envelope building (no HTTP dependency,
                         unit-tested on the host directly -- see src/test_util.c)
    http_util.h        - tiny send_json() helper bridging util.c's buffers to CivetWeb
    discovery.h/.c     - UDP discovery responder (port 32227)
    management_api.h/.c - /management/* endpoints
    common_api.h/.c    - the 7 members shared by every Alpaca device type
    camera_api.h/.c    - camera-specific members + startexposure's background capture worker
    switch_api.h/.c    - ISwitchV2 device for the lens dew heater (persisted state + sysfs GPIO)
    setup_api.h/.c     - the browser-facing setup page served at /, /setup and every
                         /setup/v1/<type>/<n>/setup -- what a client's "Settings" button opens
    test_capture.c     - standalone hardware test: detect sensor, capture one frame, print stats
                         (not part of alpacad; `gcc ... -o test_capture` and run manually via adb)
    test_util.c        - standalone unit tests for util.c, runs on the host (no cross-compile needed)
```

## Verified on real hardware (2026-09-15)

- `sensor_detect()` correctly identifies the SC3336 and computes `row_time_us=27.451` **live** from the
  subdev's `horizontal_blanking`/`pixel_rate` controls — matches the project's independently-validated
  27.45µs/row exactly, without hardcoding it.
- V4L2 control access required normalizing control names: the kernel reports `"Exposure"`,
  `"Analogue Gain"` etc. (human-readable), not the lowercase-with-underscores form `v4l2-ctl` displays and
  this project's control names use — `v4l2_capture.c` normalizes both sides before comparing. Found by
  actually running it and seeing every control lookup fail, not by reading kernel source.
  Get/set/range all independently cross-checked against `v4l2-ctl` output.
- `unpack_bits_lsb()` produces genuinely different statistics per Bayer phase on a real captured frame
  (B/G1/G2/R means 78/97/99/91, std devs also distinct) — the opposite of the "identical across all four
  phases" signature the project's memory flags as the RAW10-bit-order bug. Real image structure, not noise.
- Full exposure cycle: `PUT startexposure Duration=0.05` → `camerastate` 2 (Exposing) → 0 (Idle) within
  ~1s → `imageready` true → `imagearray` returns a well-formed 1296×2304 JSON array, values 57–1023
  (valid 10-bit range), `ErrorNumber: 0`.
- `ExposureMax` (0.899s) computed non-invasively from live control ranges matches a hand-calculated
  ceiling from the same `vertical_blanking` max (31471) the project's existing notes already flagged as
  the open "push the exposure ceiling" question — this number will track that investigation automatically
  once it moves, since it's not a hardcoded constant.

## Persistence across a board reboot (added 2026-09-15)

`alpacad` is now baked into the image, the same way as everything else in the debloat work: the
cross-compiled, stripped binary lives at `project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-astroguider/usr/bin/alpacad`
(deployed to `/usr/bin/alpacad` on-device via `RK_POST_OVERLAY`), and
`.../overlay-luckfox-astroguider/etc/init.d/S60alpacad` starts it at boot (positioned after
`S50usbdevice` brings up usb0's static IP). **Build-verified only** — confirmed both files land correctly
in a full `./build.sh rootfs && media && app && firmware` output with the right permissions and
architecture, but not yet confirmed on an actual reboot, since this board has no onboard eMMC/NAND (see
repo `CLAUDE.md`) and can't be reflashed over USB — needs a physical SD card swap to test. Remember to
rebuild+reflash after any further `alpacad` source change; the overlay copies whatever binary is at that
path at build time, it doesn't rebuild it for you.

## Real hardware has ~32MB RAM, not 64MB (corrected 2026-09-15)

`MemTotal` is 32588kB per `/proc/meminfo`, not the 64MB assumed throughout earlier design discussion.
This was discovered the hard way: an initial fix for `imagearray`'s missing `Content-Length` (see below)
buffered the whole ~11MB response in one `malloc`, which **OOM'd and crash-rebooted the board** during
testing (confirmed via `adb devices` showing repeated re-enumeration, then `uptime` showing a fresh boot).
Available RAM was observed as low as ~2MB free under load. **Any future code touching a full-frame buffer
needs to budget against ~32MB total, not 64MB, and should stream rather than buffer whenever the data
scales with frame size.**

## Real client testing (Windows + N.I.N.A. or similar, 2026-09-15): two bugs found and fixed

Alpaca autodiscover from a Windows PC found the device correctly ("OpenAstroGuider Camera" at a
self-assigned `169.254.x.x:11111#0`) — validates the link-local networking fix over the real link, not
just `adb forward`. Takes ~15-20s from plugging in to being discovered; not yet investigated (likely
IPv4LL probe/announce timing — RFC 3927 allows up to ~9s of ARP probing alone). Connecting worked, but
starting an exposure errored out. Two real bugs, both fixed and verified on hardware:

1. **`StartX`/`StartY`/`NumX`/`NumY` weren't implemented at all.** My own `curl`-based testing never
   exercised these, but real Alpaca clients (N.I.N.A. included) `PUT` `NumX`/`NumY` to the full frame size
   before *every* exposure, even a full-frame one, and abort if that `PUT` returns "Not implemented."
   Fixed: `device_state_t` now carries `start_x`/`start_y`/`num_x`/`num_y`, accepted/stored/reported via
   GET/PUT. Real cropping still isn't implemented — capture always returns the full sensor frame
   regardless of what's requested, which is honest for the common case (guide cameras capturing full
   frame) but would mislead a client that actually wants a genuine subframe.
2. **`imagearray` streamed with `Connection: close` and no `Content-Length`**, which strict HTTP client
   libraries (e.g. .NET `HttpClient`, likely what N.I.N.A. uses internally) can be picky about. Fixed
   with a two-pass approach in `send_imagearray()`: pass 1 counts the exact output byte length with cheap
   integer digit-counting (no allocation), pass 2 streams the actual content through a small fixed 8KB
   buffer after declaring that exact `Content-Length`. Verified: `Content-Length` header matches
   `curl`'s actual downloaded size exactly, memory flat before/after (no growth, unlike the crash-inducing
   full-buffer attempt above), valid 1296×2304 JSON output.

## Follow-up (same day): got real PHD2 guiding, then chased two more issues

Got actual guide exposures out of PHD2 end-to-end. Two things came up:

1. **"Images arrive every 5-10s" — root cause found: the JSON `imagearray` payload itself.** PHD2 has no
   native Alpaca HTTP client; it connects via the classic ASCOM COM interface, so the actual HTTP requests
   come from the **ASCOM Platform's Alpaca-to-COM bridge**, a client library that (like `alpyca`)
   auto-negotiates for the faster binary format when available. `ImageBytes` is now implemented
   (`send_imagebytes()` in `camera_api.c`): 11 little-endian int32 header fields (44 bytes) followed by
   the raw `uint16_t` pixel buffer, streamed with a **single `mg_write`** straight from the existing
   capture buffer — no extra allocation at all, safer on this ~32MB device than even the JSON path's small
   scratch buffer. Field layout and enum values (`UInt16=8`) were taken directly from ASCOM's own `alpyca`
   reference client source (`_build_imagedata_array` in `camera.py`), not guessed. **Verified: 0.68-0.70s
   fetch time versus 6.4s for JSON — roughly 9x faster** — with `alpyca` itself successfully decoding it
   (correct shape, dtype, pixel values) and a manual Python `struct.unpack` decode matching exactly.
   This also **definitively resolves the previously-flagged "unverified row/column ordering" question**:
   alpyca's own docstring states the wire format is row-major, and its shape report
   (`(1296, 2304)` = `(CameraYSize, CameraXSize)`) confirms `camera_api.c`'s existing `[row][col]` nesting
   was already correct — no ordering bug ever existed.
2. **"Exposure duration doesn't seem to apply" — confirmed NOT a server-side bug.** Tested directly:
   0.01s exposure → mean pixel value 85 (max 650); 1s exposure → mean 677, **saturating at 1023** (full
   overexposure, exactly as expected in daylight). Server-side duration handling is correct.

**Both confirmed fixed in real end-to-end PHD2 testing on Windows** (not just isolated `curl`/`alpyca`
checks): images arrive noticeably faster, and exposure duration now looks correct. The likely explanation
for #2 having looked broken in the first place: with the old ~6-10s JSON fetch time, PHD2 could easily
have been displaying a stale/previous frame while a much shorter new exposure had already completed,
making brightness look mismatched from the duration setting — a symptom of #1, not a separate bug. Once
fetches dropped to ~0.7s, that confusion went away on its own.
3. **RAM re-confirmed via `dmesg`, with the full explanation**: `Memory: 32368K/65536K available ...
   24576K cma-reserved` — the chip genuinely has **64MB physical RAM** (not wrong to assume that), but
   24MB is reserved for the Contiguous Memory Allocator (`RK_BOOTARGS_CMA_SIZE="24M"` in the board
   config), leaving ~32MB for normal `malloc()`/`MemTotal` — which is what actually OOM'd. This 24MB was
   almost certainly sized for the stock ISP/RGA/MPP/rockit pipeline this project already stripped out;
   shrinking it would give some of that 32MB back, but hasn't been attempted (risk: verify rkcif's V4L2
   buffers still allocate fine at a smaller CMA size before shipping this).

## SharpCap black image + N.I.N.A. total failure, same root cause (2026-09-15)

N.I.N.A. gave a precise, actionable error: `The JSON value could not be converted to System.Int16. Path:
$.Value | LineNumber: 0 | BytePositionInLine: 14`. Doing the byte-offset math on a response shaped like
`{"Value":99614,...}` lands exactly on the comma after `99614` — our `gainmax`, which is the raw V4L2
`analogue_gain` maximum. **ASCOM's Camera interface types `Gain`/`GainMin`/`GainMax` as `Int16`** (a
holdover from the classic COM interface, max value 32767) — `99614` blows straight past that, and
N.I.N.A.'s strict .NET client correctly refuses to deserialize it. This almost certainly also explains
SharpCap's all-black-regardless-of-exposure symptom: if it likewise can't parse a valid gain range, it may
silently fall back to an unusable gain (e.g. 0, which is below our actual raw minimum of 128 = 1x) —
producing black frames no matter the exposure duration, since there'd be effectively no signal to
integrate. One bug, two different failure modes depending on how strictly each client handles it.

Fixed by no longer exposing the raw V4L2 gain range directly: `gain`/`gainmin`/`gainmax` now translate
to/from a fixed, `Int16`-safe ASCOM-facing scale (`0..1000`, `ascom_gain_to_raw`/`raw_gain_to_ascom` in
`camera_api.c`) at the API boundary, while `device_state.gain` still stores the raw value actually applied
to hardware.

### Follow-up: N.I.N.A. connected after the Int16 fix, but SharpCap was still black — captured a real request log

Added temporary request/exposure logging to `alpacad` (every request's method/params, every exposure's
requested-vs-applied hardware values, and actual captured min/max/mean) after running out of things
verifiable by hand. First attempt at pulling the log came back with only the two startup lines — turned
out to be a **stdio buffering bug in the diagnostic code itself**: `stderr` becomes fully block-buffered
once redirected to a regular file (as `S60alpacad` does), so `fprintf(stderr, ...)` calls were sitting in
libc's buffer instead of reaching `/tmp/alpacad.log`. Fixed with `setvbuf(stderr, NULL, _IONBF, 0)` in
`main()` — confirmed log lines appear immediately afterward.

With logging actually working, a real captured SharpCap session revealed the true story, and it's not a
decoding or capture bug at all: **every single exposure SharpCap triggered produced genuinely valid,
correctly-applied pixel data** (requested/applied exposure and gain matched exactly every time; e.g.
`mean=472.5`, `mean=977.8` for a since-saturated one). SharpCap runs an extensive gain auto-calibration
sweep on connect (dozens of `PUT gain` calls climbing 5→898 out of our 0-1000 scale) — completely normal
behavior for characterizing an unfamiliar camera. The problem: at ASCOM gain ≈898, our then-**linear**
mapping across the sensor's *entire* native range (128..99614 raw, i.e. 1x..~778x) produced raw gain
**89466 (~700x)**. At that gain even a modest exposure saturates completely; SharpCap compensated by
cutting exposure to ~2.2ms, and at that combination the sensor genuinely reads out as noise floor
(`mean=9.2`/1023) — visually indistinguishable from "black" without heavy stretching, but not corrupted
or wrong data.

Root cause: a guide camera has no real use for gains anywhere near 700x (it mostly just amplifies read
noise), yet exposing the sensor's full native range through the same `0..1000` ASCOM scale guarantees any
calibration routine that probes "near the top of the range" — which is normal, expected client behavior —
lands on an unusably extreme value. Fixed by capping the ASCOM-exposed range to a practical ceiling
(`GAIN_PRACTICAL_MULTIPLIER = 32`, i.e. up to 32x, clamped to the sensor's actual max in case a future
sensor's native range is smaller) instead of the sensor's full ~778x maximum. This is a real, independently
worthwhile fix (a guide camera has no use for 700x gain regardless), and stays in — **but it turned out not
to be the cause of SharpCap's black image.**

### Correction: the gain theory was wrong — disproven by direct evidence

Asked directly whether the very *first* exposure (server-measured `mean=472.5`, well-exposed, gain
untouched at the default) also displayed black in SharpCap. Answer: **yes, every single image was
identically, flatly black — "not even noisy, just black."** That rules out an exposure/gain data problem
entirely: real underexposed sensor data still shows read noise; a perfectly flat black display across
every test regardless of what the sensor actually captured (bright, dim, saturated — all looked the same)
points to something structurally broken in how the client receives or decodes the response, not in what's
in it. The gain-scale fix above is being kept because it's independently correct, but it was the wrong
explanation for this bug.

Re-examined the request timing more carefully too: the gain-sweeping sequence (`PUT gain=5,15,25...`)
starts *while a previous exposure using the untouched default gain is still capturing* — not after
examining that exposure's result — so "SharpCap's gain auto-calibration converging on a brightness target"
was also an overconfident inference, not a verified mechanism. Retracted.

**Current hypothesis, from SharpCap's own forum posts**: SharpCap does strict frame validation and "will
drop frames if there's a mismatch between the source array and destination buffer," and explicitly
verifies `NumX`/`NumY` are read back unchanged after setting them. Checked alpyca's own `ImageBytes`
reshape logic directly (`_build_imagedata_nested_list_array`: `rows = Dimension1`, `cols = Dimension2`,
row-major) — matches what `camera_api.c` already implements and what live-tested correctly via `alpyca`
earlier, so there's no *direct* evidence of a dimension-order bug, but a dimension mismatch specific to
SharpCap's own (unverifiable, closed-source) parsing remains a live possibility, especially since this
sensor is non-square (2304×1296) and would silently expose a width/height mix-up that a square sensor
never would.

**Diagnostic result and real root cause found.** Forced JSON-only and retested: SharpCap now threw an
explicit error instead of a silent black image —
`SendToRemoteDevice JSON - image array element type Unknown is not supported. The device returned this
value: 0 (error code: 0x8004040b)`. That error is precise: it means SharpCap read a `Type` field from the
JSON response and got `0` (`ImageArrayElementTypes.Unknown`) — but **our JSON response never included a
`Type` field at all**, so this is the value SharpCap defaults to when it's simply missing.

Confirmed the actual required envelope by reading a real, independent, open-source Alpaca server
implementation (`github.com/mikefsq/goalpaca`, `server/imagearray.go`) rather than continuing to infer
from a client library's tolerant reshaping code. It revealed two concrete bugs, both now fixed:

1. **The JSON response was missing required `"Type"` and `"Rank"` fields.** The correct envelope is
   `{"Type":<n>,"Rank":<n>,"Value":[...],...}` — `Type` follows a collapsing convention (integer types up
   to 32 bits present as `Int32`/`2` regardless of actual wire width; we're `UInt16`, so `Type` is always
   `2`). Directly explains the SharpCap error above.
2. **The array dimension order was backwards.** The reference implementation's comment is explicit: *"a
   [Width][Height] array with the second (Y) index varying fastest ... Value[x][y] = Pixels[y*Width+x]"*
   — i.e. **X is the outer index**, not Y. `camera_api.c` had this backwards in both the JSON path (row-major,
   Y outer) and the `ImageBytes` header (`Dimension1=height` instead of `width`). This had gone undetected
   because: (a) the earlier "verified via alpyca" claim only checked that pixel *statistics* (min/max/mean)
   matched, which are transposition-invariant and can't catch an orientation bug; (b) alpyca's own
   client-side reshape code just mirrors whatever `Dimension1`/`Dimension2` a server sends, without
   validating against the spec, so it silently tolerated the wrong order. This likely also explains the
   *original* `ImageBytes`-enabled black-image symptom (before this session's diagnostic disable): wrong
   dimension order silently failing SharpCap's own documented strict frame-validation (see below), which
   drops mismatched frames without surfacing an error — unlike the JSON path's more visible failure mode.

Fixed both in `camera_api.c`: `send_imagearray()` now emits `Type`/`Rank` and iterates X-outer/Y-inner;
`send_imagebytes()` now sets `Dimension1=width`/`Dimension2=height` and streams the transposed pixel order
through a small fixed scratch buffer (not a second full-size copy — this device has ~32MB usable RAM,
learned the hard way earlier this session). The loop logic was verified structurally correct via a small
hand-traceable host-side test (a 3×2 buffer with distinct values, confirming `Value[x][y] = Pixels[y*w+x]`
holds exactly) since the board wasn't reachable from this machine to test on real hardware directly.
`ImageBytes` re-enabled now that the same underlying fix applies to both paths.

**Not yet verified on real hardware or against a real SharpCap/N.I.N.A. session** — this fix has
meaningfully more confidence behind it than the two earlier (wrong) theories, since it's grounded in an
independent, authoritative reference implementation rather than inference from a log pattern, but it still
needs a real retest to confirm.

Also worth noting from SharpCap's own forum posts (found while researching this): it does strict frame
validation and "will drop frames if there's a mismatch between the source array and destination buffer,"
and explicitly verifies `NumX`/`NumY` are read back unchanged after setting them — consistent with the
dimension-order theory above, and worth keeping in mind for any future Alpaca property that reports a
size/shape.

## Real per-frame latency measured precisely, root cause found for both halves (2026-09-15)

User reported PHD2 0.1s exposures "still come in pretty slow" with a "decent delay." Rather than guess,
added `CLOCK_MONOTONIC`-based timing to every stage: `now_ms()` helpers in both `camera_api.c` and
`v4l2_capture.c`, per-V4L2-ioctl breakdown inside `v4l2_capture_frame()`, and a pack-vs-write split inside
`send_imagebytes()`. Pulled a real log from a live PHD2 session. Two large, *fixed* costs dominate every
frame, almost entirely independent of the requested exposure duration:

**1. `v4l2_capture_frame()` has a ~650-700ms floor, and `VIDIOC_STREAMOFF`+`munmap`+`close` (labelled
`teardown`) is ~515-523ms of it — every single call, steady state:**
```
[v4l2] open=20 fmt=0 reqbufs=0 mmap/qbuf=8 streamon=21 dqbuf=39 unpack=53 teardown=515 total=660
```
`dqbuf` (waiting for the first frame after `STREAMON`) is only ~35-65ms in steady state — the earlier
hypothesis that hardware pipeline warm-up dominated was wrong; it only shows up as ~836ms on the *first*
capture right after the daemon starts (CSI/DPHY link locking cold), then drops to normal immediately after.
`teardown` is the real, reproducible bottleneck: because `v4l2_capture_frame()` opens `/dev/video0`,
configures format, requests/maps buffers, streams on, grabs one frame, and tears the whole thing back down
**on every single exposure** (flagged as an unverified hypothesis earlier this session — now confirmed with
real numbers), it pays this ~515ms stop/unmap/close cost regardless of whether the exposure itself took
1ms or 900ms. Confirmed directly: a 1.1ms-exposure request (43 rows) still took 643ms total; a 3.2ms request
(119 rows) took 650-656ms twice in a row.

**2. `send_imagebytes()`'s ~1.3s "send" time is CPU-bound, not network-bound — the transposed pixel copy is
the entire cost:**
```
[imagebytes] pack=1078ms write=250ms
```
`write` is the actual `mg_write()`/network time — sending ~5.7MB (2304×1296×`uint16_t`) in ~250ms is
~23MB/s, a perfectly ordinary RNDIS-over-USB rate. `pack` — just copying pixels from the internal row-major
buffer into the scratch buffer in transposed (X-outer/Y-inner) order — takes **4x longer than the network
transfer itself**. This is the classic transpose cache-miss pattern: reading `pixels[y*w+x]` for fixed `x`
while varying `y` strides by `w*2` bytes (4608 bytes here) on every single read, so almost every access
misses cache. The dimension-order fix earlier this session was necessary for correctness, but the naive
loop it introduced is the actual performance bug.

**Combined:** every frame costs roughly 650ms (capture, dominated by V4L2 teardown) + ~1320ms (send,
dominated by the pack loop) ≈ **~2 seconds fixed floor per frame**, regardless of exposure duration — this
is the full, now-precisely-measured explanation for "0.1s exposures still come in slow."

**Two independently actionable next steps identified; #1 now implemented (see below), #2 still open:**
1. ~~Stop opening/closing `/dev/video0` per capture.~~ Done — see "Persistent V4L2 device" below.
2. Replace the naive per-pixel transposed copy with a cache-blocked/tiled transpose (process in small
   square tiles that fit in cache, e.g. 32x32), or restructure the internal frame buffer to already be
   stored in wire order so no runtime transpose is needed at all. The network write itself needs no change.

## Persistent V4L2 device — implemented and hardware-verified, including two real correctness bugs found and fixed (2026-09-15)

`v4l2_capture_frame()` used to open, configure, and stream-start `/dev/video0` from scratch on every single
exposure, then stream-stop/unmap/close it again — the ~515-523ms `STREAMOFF`+`munmap`+`close` teardown
alone (see "Real per-frame latency" above) was the single largest fixed cost per frame. Reworked into
`v4l2_capture_init()` (open/format/allocate one mmap'd buffer/`STREAMON`, called once from `main()` at
daemon startup, right after sensor detection — if it fails the daemon exits rather than starting the HTTP
server with no working capture path) + `v4l2_capture_frame()` (now takes no `sensor_desc_t*`, just uses the
descriptor stashed by `_init`) + `v4l2_capture_shutdown()` (not currently called anywhere — the daemon is
killed and restarted wholesale via `S60alpacad`, and the kernel reclaims the fd/mmap on process exit;
provided for symmetry and for `test_capture.c`).

**A single mmap'd buffer, not the old 4-buffer pool**, deliberately — the driver can only ever have one
frame in flight, which makes the correctness argument for continuous streaming tractable. The real
correctness problem persistent streaming introduces that the old per-capture open/close design never had:
with the device *never* stopped between exposures, a naive single `DQBUF` after setting new exposure/gain
could hand back a frame that was already in-flight (captured under the *previous* request's settings).
`v4l2_capture_frame()` discards-and-requeues whatever the driver already had ready before doing the "real"
`DQBUF` for the frame that actually reflects the caller's just-applied controls.

**Two real settling bugs found via on-hardware testing, both fixed:**
1. **First exposure after daemon startup.** With a single discard round, the very first client exposure
   after a fresh boot came back with a saturated/bright frame when a dim (short-exposure, minimum-gain)
   frame was requested — confirmed reproducible: repeated on a second clean reboot with identical steps,
   while every subsequent request in the same session (including far bigger exposure/gain swings) was
   correct on the first try. Fixed by having `v4l2_capture_init()` run one throwaway discard-then-capture
   cycle (calling `v4l2_capture_frame()` itself and discarding the result) immediately after `STREAMON`,
   before the HTTP server starts accepting connections — so any client's first real request never sees this
   cost.
2. **First request that raises `vertical_blanking`.** `exposure_worker()` raises `vertical_blanking` when a
   requested exposure exceeds the sensor's current frame-length ceiling (see "Validated sensor/driver
   facts" in the repo `CLAUDE.md`). The *first* such raise in a session came back with the *previous*
   request's data instead of its own (confirmed via a controlled alternating-settings test: identical
   dim/bright pairs repeated several times in one continuous run, only the first bright shot — the first to
   trigger a `vertical_blanking` raise — was wrong; every later raise, in the same session, was correct).
   Changing `vertical_blanking` changes the frame period itself, not just an exposure/gain value, and
   evidently needs strictly more settling than a plain control change. Fixed generally, without needing to
   detect "is this a `vertical_blanking`-raising request": `v4l2_capture_frame()` now does **two** discard
   rounds before the real capture on every call, not one.

Both fixes verified via a full reboot + a controlled alternating-settings sequence (dim/bright pairs
repeated multiple times), reading `exposure_worker()`'s existing `requested ... -> applied ...` /
`captured min/max/mean` log lines: every request now correctly and immediately tracks its own settings, on
the first try, with no lag from any previous request. Also end-to-end verified: `imagearray` (JSON and
`ImageBytes`) both return correctly-shaped, correctly-valued data matching the log, and `ImageBytes` is
still ~6x faster than JSON (1.4s vs. 8.9s for a full 2304×1296 frame over the same link).

**Net latency effect**, measured via the new `[v4l2] discard=.. dqbuf=.. unpack=.. requeue=.. total=..` log
line: when `vertical_blanking` is still at its low boot-default value, total V4L2 capture time is
**~80-150ms** (vs. the old design's ~650-700ms fixed floor) — a clear win. Once `vertical_blanking` has been
raised for a longer exposure, the sensor's actual frame period is much longer (steady-state `dqbuf` alone
measured ~447ms at one such elevated setting), and paying that period 3x per capture (2 discards + 1 real)
brings total capture time to **~1.1-1.3s** — slower than the low-vblank case, but this is the sensor's own
frame-period cost, not specific to this fix: the old per-capture-reopen design's `dqbuf` step would have hit
the same elongated real frame period (vertical_blanking is a sensor register, unaffected by reopening
`/dev/video0`) *in addition to* its fixed ~515ms teardown, so the old design was likely comparable or worse
in this same scenario, not measured at the time. **Update:** the blanket-always-2-rounds cost is now fixed
— see "Latency: conditional discard + cache-blocked transpose" below.

Cross-compiled cleanly (`-Wall -Wextra`, no warnings) at every step; binary stripped and kept in sync in the
`overlay-luckfox-astroguider` overlay so a full image build picks it up.

## Latency: conditional discard + cache-blocked transpose (2026-09-15)

User reported PHD2 0.5s exposures still taking noticeably longer than 0.5s per loop iteration even after
the persistent-V4L2 fix above. Two real costs, both fixed:

1. `v4l2_capture_frame()` took an `extra_settle` parameter; the extra discard round is now only paid on the
   request that actually raises `vertical_blanking`, not on every capture forever. `exposure_worker()`
   passes `job->rows > exp_max_cur` (the same condition that decides whether to raise `vertical_blanking`)
   straight through.
2. `send_imagebytes()`'s transpose loop was rewritten to read the source buffer in cache-blocked 32x32
   tiles (row-major reads, which are cache-friendly) instead of one `pixels[y*w+x]` at a time (stride-`w`,
   cache-hostile). Verified on hardware: **pack time dropped from ~1078ms to ~80-100ms (~12x)**.

A full 0.5s-exposure loop (startexposure → poll → imagebytes fetch) measured via `curl` through
`adb forward`: first iteration (pays the one-time `vertical_blanking` settle) ~1.8s, steady-state
~0.9-1.3s — down from ~2.3-2.5s before these two fixes.

**The remaining dominant cost is `mg_write()` itself** — measured ~620-655ms for a ~5.7MB frame here, vs.
the ~250ms (~23MB/s) measured previously during a real PHD2 session over the actual `usb0` RNDIS link.
This host has no route onto that interface (only `adb`), so this session's number was necessarily measured
through the `adb forward` TCP tunnel, which is not representative of a real client's network path — treat
today's write-time number as a testing-tunnel artifact, not a confirmed regression, until re-measured
against a real Alpaca client on the real link.

## Boot-to-discoverable delay: dhcpcd `usb0` timeout override (2026-09-15, effect not yet measured)

User reported several seconds' delay after boot before the board is discoverable on the network. Root
cause: `dhcpcd` always attempts a real DHCP lease first on every interface, including `usb0` — but `usb0`
is a point-to-point USB RNDIS gadget link with no DHCP server ever present on the other end, so that
solicit always times out (several seconds, confirmed via `dhcpcd`'s own log) before it falls back to
IPv4LL self-assignment (which is what actually ends up being used — see the link-local networking fix
above). Added a new overlay file, `etc/dhcpcd.conf` (the full stock config plus an appended
`interface usb0 { timeout 1 }` block), via the same `RK_POST_OVERLAY` mechanism as everything else — this
overrides the buildroot-generated default since the overlay applies last. Verified the block lands
correctly in the built rootfs. **Not yet measured on hardware how much boot-to-discoverable time this
actually saves** — the board did visibly pick up its IPv4LL address (`169.254.201.227`) quickly on the one
reboot observed this session, but no rigorous before/after timing comparison has been done yet.

## Hot-iteration workflow via `adb push` + a bus-power gotcha (2026-09-15)

For pure `alpacad`-source changes, a much faster loop than a full SD-card reflash: cross-compile locally
(`make` in `alpaca/`), copy the binary into
`project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-astroguider/usr/bin/alpacad` (keeps the source-tree
overlay in sync for the *next* full build), then `adb push` that same binary straight to `/usr/bin/alpacad`
on the live board and `/etc/init.d/S60alpacad restart`. Confirmed working and much faster than a physical
SD card swap for iterating on `alpacad` alone.

**Gotcha, hit for real this session:** this board is USB-bus-powered, so unplugging it to move it between
machines (or to reconnect it) is an *unclean power cut*, not a graceful shutdown. `adb push` writes to the
page cache but doesn't `fsync`, and ext4's delayed allocation can leave that data dirty in memory for a
while — if power is cut before it's flushed, the file comes back as **zero bytes** after the journal
replay on next boot (this happened: pushed a binary, it ran fine, then a later reconnect/reboot cycle came
back with `alpacad` a 0-byte file and the daemon simply not running — looked exactly like a network/
discovery bug at first but was pure filesystem data loss). **Fix: always run `sync; sync` on-device
immediately after `adb push`ing a binary you intend to survive a power cycle**, not just after the restart.

**Also worth flagging:** the live SD card now has (via this hot-push workflow) a newer `alpacad` than
whatever is baked into the last full `./build.sh ... firmware` run's `output/image/sd_update.img`. Run a
fresh full build before relying on that image for a reflash, or the hot-patched improvements from this
session will be lost.

## Stripes regression: the cache-blocked transpose emitted pixels in the wrong order (2026-09-16)

User reported the previous commit broke the image entirely -- "only stripes now." Root cause is the
cache-blocked transpose introduced in that same commit (`send_imagebytes()`): it streamed 32x32 tiles in
`x0`-outer / `y0`-inner order, so for a given 32-column band it emitted each column's rows *32 at a time*,
interleaved with the other 31 columns of the band, before moving to the next 32 rows. The wire format
needs all `h` rows of column `x` before column `x+1`. The byte *count* was right and the pixel *values*
were all present, which is why the pack-time measurement (~1078ms -> ~80-100ms) looked like a clean win:
only the ordering was wrong, and nothing in that session checked ordering.

Fixed by blocking over a **band of whole columns** instead of square tiles: transpose `IB_BAND` (32)
complete columns into a scratch buffer, then `mg_write()` that band. Concatenating bands left-to-right *is*
the wire order by construction, so ordering can't drift again, and the reads stay row-major/cache-friendly.
Verified on the host against a naive reference transpose (the old loop `DIFFERS`, the new one `MATCHES`,
including a 101x77 case that exercises partial bands and partial tiles).

**Confirmed on real hardware (2026-09-16).** The same frame fetched as `ImageBytes` and as JSON is
elementwise identical (two independently written loops agreeing), header reads
`Dim1=2304 Dim2=1296 Rank=2 ImageElementType=8`, and the decoded frame renders as a normal, coherent
image. Feeding the *same* pixels back through the old tiled loop reproduces the reported vertical stripes
exactly, which closes the diagnosis. Note that a "spatial coherence" check (correlation of adjacent rows)
was tried first and **does not discriminate** — the scrambled image scored *higher* than the correct one;
the elementwise comparison against the JSON path and simply looking at the rendered image are what
actually settle it.

## V4L2 buffer pool restored to 3 (2026-09-16)

The persistent-device change set `REQBUFS count = 1`, and `v4l2_capture_frame()` holds that single buffer
dequeued across the whole unpack. With nothing queued, the driver has no other DMA target for the frame
in flight -- on rkcif that means it can land back in the buffer userspace is still reading, i.e. a torn
frame. Restored to a 3-buffer pool so the DMA target is always a buffer we don't own, and the sensor keeps
streaming between exposures instead of stalling. The freshness argument is kept explicit rather than
relying on "only one buffer can exist": drain every already-completed buffer non-blocking, discard one more
blocking frame (it may have *started* during the drain, before the new controls latched), then take the
next one. The fd is now `O_NONBLOCK` with `poll()` supplying the blocking half, which also gives DQBUF a
real timeout instead of wedging the exposure thread if the CSI link stops delivering.

This was a plausible second contributor to the stripes, but the transpose bug above is the confirmed one.

## Gain and vertical_blanking pinned; per-frame subdev I/O collapsed (2026-09-16)

By project decision, to make frame-delivery latency the only moving part while it is being optimized:
`analogue_gain` is pinned to 128 (1x) and `vertical_blanking` to 64, set **once** at startup by
`camera_apply_fixed_sensor_settings()` before `STREAMON`. `exposure` is the only control touched per
frame. Undo by restoring the vblank-raising block in `exposure_worker()` and the `vblank_max`-based
ceiling in `compute_exposure_max_rows()`; both are commented as such.

**Consequence:** frame length is `height + vertical_blanking` and exposure is capped a few rows below it,
so a pinned vblank of 64 caps exposure at ~1352 rows ~= **37ms**. Long exposures are off the table until
the pin is lifted. `exposuremax` now reports that real ceiling (it just reads the exposure control's
current max, which already tracks vblank) and `startexposure` clamps to it instead of letting the driver
clip silently. `gain` PUTs are accepted but not applied -- erroring would make real clients abort the
session -- and GET reports the value actually in effect.

Separately, every `v4l2_ctrl_get/set` used to `open()` the subdev and walk the **entire**
`VIDIOC_QUERY_EXT_CTRL` enumeration before doing its one ioctl, and `exposure_worker()` did seven of those
per frame. The fd and each resolved control id are now cached behind a mutex (both the exposure thread and
CivetWeb's handler threads come through here); `v4l2_ctrl_get_range` still re-queries, since a control's
min/max can change at runtime. Combined with the pin, the per-frame subdev work is two ioctls on an
already-open fd. Cost not separately measured -- it was never instrumented.

## Where the per-frame time actually goes now (measured on hardware, 2026-09-16)

Full `startexposure` -> poll -> `imagebytes` loop at a 20ms exposure, via `curl` over `adb forward`:
**~0.89-0.93s steady state.** Device-side breakdown:

```
[v4l2] discard=67 dqbuf=32 unpack=64 requeue=0 total=163
[imagebytes] pack=74ms write=615ms
```

- `discard` ~60-67ms is ~1.6 frame periods at the pinned 37ms period — the cost of guaranteeing the frame
  reflects the just-applied exposure.
- `unpack` ~64ms and `pack` ~74ms are two separate full passes over ~6MB.
- `write` ~615ms looked dominant, but that was the `adb forward` TCP tunnel. **Settled the same day over
  the real RNDIS link** (see below for how to get onto it): `write=265ms`, i.e. 5.97MB at ~22.5MB/s,
  confirming the earlier live-PHD2 figure. The tunnel was inflating it ~2.3x, and every prior latency
  number in this file measured through `adb forward` should be read with that in mind.

**Real-link numbers (2026-09-16), full loop ~0.51-0.54s at a 20ms exposure:**

```
[v4l2] discard=46 dqbuf=34 unpack=67 requeue=0 total=148
[imagebytes] pack=76ms write=265ms
```

So the honest budget is: **write 265ms (~51%)**, capture 148ms (of which ~80ms is the two-frame-period
freshness guarantee and 67ms is unpack), pack 76ms, rest HTTP round trips. The write is at the link's
limit — only a smaller payload moves it.

**Removed from the hot path:** the `[exposure] captured min=/max=/mean=` diagnostic walked all 3M pixels
*before* marking the frame ready — ~60ms of pure latency per frame for one log line. Now gated behind the
`OAG_FRAME_STATS` environment variable (set it to re-enable without a rebuild). Measured effect: loop went
from ~0.95s to ~0.90s.

**Next levers, now that the write time is settled:**
1. ~~Fuse unpack and transpose into one pass.~~ Implemented — see below.
2. Binning or a real subframe implementation — the only thing that touches the 265ms write. 2x2 binning
   gives 1152x648 = 1.5MB, so write drops to ~66ms: a ~200ms saving, nearly 3x lever #1. Note the
   production target (IMX290) is mono, where 2x2 binning is trivial; the SC3336 stand-in is Bayer, so
   binning a 2x2 quad there mixes colour channels (defensible for guiding luminance, but it is a real
   design decision, not a free win).
At 2304x1296x16bpp the full frame is 5.97MB, so the full-frame ceiling is ~3.8 fps regardless.

## Unpack and transpose fused into one pass (2026-09-16, not yet measured on hardware)

The frame was being unpacked row-major (~67ms) and then transposed into ImageBytes wire order as a
separate pass (~76ms) — a full extra write *and* read over ~6MB on a CPU with a 32KB L1. Since the
daemon's only consumer of a frame is the wire format, `unpack_bits_lsb_transposed()` now writes
`out[x * height + y]` directly and `v4l2_frame_t.pixels` is stored that way throughout. Consequences:
`send_imagebytes()` is a single `mg_write` of the whole buffer with no per-pixel work at all, and the JSON
path's X-outer/Y-inner loops now read the buffer *sequentially* too (they were the strided ones before).

The unpack is blocked into 32x32 tiles so neither the strided packed reads nor the strided output writes
go wide: within a tile the writes touch 32 output cache lines that stay resident across all 32 rows. The
naive version (unpack a row, scatter it at stride `height`) would miss on essentially every write.

Verified on the host against the existing row-major unpack plus an explicit transpose, at 2304x1296,
1920x1080, 64x64, 100x70 and 96x33 (the last three exercise ragged tiles) — all match exactly, with a
deliberately padded stride so stride != width*10/8 is covered.

**Expected saving is ~50-70ms** (the eliminated 6MB write+read pass), not the full 76ms, since the
transpose work itself still has to happen somewhere. Note that for a *serial* client like PHD2 (expose,
poll, fetch, repeat) moving work off the request path onto the exposure thread does not help wall clock —
only the removed memory traffic does. **Not measured on hardware yet**; the board was disconnected for
Windows/PHD2 testing when this landed.

**`frame.pixels` is no longer row-major.** Anything reading it directly must index `[x * height + y]`.
`test_capture.c`'s raw dump changed format accordingly (reshape as `(width, height)` and transpose).

## Reaching the board over the real RNDIS link from a Linux host

The gadget shows up as an `enx*` interface that is up but has **no address** — which is why every prior
session measured through `adb forward` and had to caveat the number. The cause is NetworkManager: it owns
the interface and sits in `connecting (getting IP configuration)` forever, soliciting DHCP on a
point-to-point link that will never answer. A manually added `ip addr add` gets flushed by NM, which is
what makes this look unfixable at first. Tell NM to use link-local instead (no sudo needed; polkit allows
it for an active session):

```
nmcli con mod "Wired connection N" ipv4.method link-local
nmcli con up "Wired connection N"
```

The host then self-assigns a `169.254.x.x` and the board is directly reachable at its own. This is also
worth knowing as a *product* observation: a Linux client host does not get a working link out of the box
here, where Windows/macOS self-assign IPv4LL on their own.

## Starting alpacad over adb: use setsid

`adb shell "/etc/init.d/S60alpacad restart"` backgrounds the daemon as a child of the adb shell session; it
does not survive the session exiting, and the stale `/tmp/alpacad.log` from boot makes it look like it
started fine. Confirmed the hard way (2026-09-16). Start it detached instead:

```
adb shell "killall alpacad; sleep 1; setsid /usr/bin/alpacad </dev/null >/tmp/alpacad.log 2>&1 &"
```

## Known gaps / not yet done

Kept honest as of **2026-09-16** (end of the setup-page session). Several entries that used to live here
have since been closed by sections further down this file; if you are adding to this list, re-read it
first rather than appending, because a stale gap list is worse than none.

- **No mid-capture cancellation.** `stopexposure`/`abortexposure` reset the reported state but the
  in-flight V4L2 `DQBUF` call still runs to completion in its worker thread; its result still lands in
  `last_frame` when done. Tolerable while exposures are short, and now more of a real gap than it was,
  since `ExposureMax` is ~0.9s rather than ~37ms.
- **Single exposure at a time, not deeply concurrency-hardened.** `startexposure` refuses a second call
  while one is in flight, but there is no queueing or cancellation beyond that.
- **Gain is pinned at 128 (1x) and client gain control is a no-op.** PUTs are accepted rather than
  errored, because real clients abort on an error. `vertical_blanking` is *not* pinned any more (see
  "Long exposures restored"), so this is now the only remaining pin.
- **No subframing or binning.** `StartX`/`StartY`/`NumX`/`NumY` are accepted, stored and reported, but
  capture always returns the full sensor frame. Binning is the single largest remaining latency lever
  (2x2 would cut the 265ms `mg_write` to ~66ms) and is blocked on a real design decision: the SC3336 is
  Bayer, so binning a quad mixes colour channels. The production IMX290 is mono, where it is trivial.
- **`ReadoutModes`/`ReadoutMode` not implemented.** This is the correct ASCOM mechanism for a
  user-selectable output format ("Raw 10-bit" vs "Scaled 16-bit", or a binned mode) and the only one
  clients surface in their UI — see "On serving JPEG instead of raw16" for why the obvious alternatives
  are not available.
- **Never run against the ASCOM Conformance tool.** Individual members have been checked by hand and
  against real clients; the conformance suite has not been run, and it probes edge cases (invalid ids,
  out-of-range values, member-before-connected ordering) that hand-testing does not.
- **Boot-to-discoverable takes ~15-20s from plugging in**, not investigated. Most likely IPv4LL
  probe/announce timing — RFC 3927 permits ~9s of probing before a bind is even attempted. The
  `dhcpcd usb0 { timeout 1 }` override was added for this but its effect has never been timed
  before/after on hardware.
- **The dew heater's success path is unverified** — every failure mode is, but no real GPIO has ever been
  toggled. See the dew-heater section; that test belongs with the PCB.
- **CDC-NCM vs RNDIS is an untested hypothesis.** Measured RNDIS throughput is 22.5MB/s against
  ~40-45MB/s realistic for USB 2.0 high-speed bulk, and NCM aggregates frames per USB transfer where
  RNDIS does not — so roughly 2x may be available on existing hardware. The tradeoff is RNDIS's
  driver-free Windows support. Nobody has measured it.

## USB: no USB 3.0 on this SoC, but only ~half of USB 2.0 is being used (2026-09-16)

Asked whether the custom PCB could use a faster USB to cut the ~265ms write. Checked the SDK rather than
assuming: `rv1106.dtsi` has

```
usbdrd_dwc3: usb@ffb00000 {
	compatible = "snps,dwc3";
	maximum-speed = "high-speed";
	phys = <&u2phy_otg>;
	phy-names = "usb2-phy";
```

The controller *is* a Synopsys DWC3 (a USB3-capable core, which is what makes this look promising), but
the only PHY in the SoC is `u2phy` (`rockchip,rv1106-usb2phy`) and the node is pinned to high-speed. There
is no SS PHY node at all. **USB 2.0 only, and no board design can change it** — it is inside the RV1103/
RV1106 silicon. A faster link would mean a different SoC.

The more useful finding: high-speed is 480 Mbps ~= 60MB/s theoretical and ~40-45MB/s realistic for bulk,
while the measured RNDIS throughput is **22.5MB/s** — roughly half the bus. The bottleneck is the gadget
protocol, not the wire. RNDIS carries one Ethernet frame per USB transfer with no aggregation; CDC-NCM
aggregates many frames per transfer and typically lands much closer to the bus limit. So there is
potentially ~2x available on the *existing* hardware.

The catch is exactly why RNDIS was chosen (see "Decisions"): driver-free Windows support. NCM is native on
Windows 11 but not reliably on Windows 10, and recent Windows 11 builds have been deprecating RNDIS from
the other direction. Worth measuring properly before committing either way — this is an untested
hypothesis about protocol overhead, not a verified number.

## Long exposures restored; gain stays pinned (2026-09-16)

Real PHD2/SharpCap/N.I.N.A. testing confirmed the frame rate is now acceptable, so `vertical_blanking` is
client-driven again while `analogue_gain` stays pinned at 128 (1x). `exposure_worker()` computes the
blanking needed for the requested exposure from the empirically-validated relationship
(`exposure_max = height + vblank - margin`, margin measured as 8 rows) plus `VBLANK_HEADROOM` slack, and
`compute_exposure_max_rows()` is back to inferring the true ceiling from `vblank_max`.

Two things this needs that the original version did not do:
- **Blanking is lowered again, not just raised.** Left high after a long exposure, the frame period stays
  long and every subsequent short exposure waits out the old slow period — which is precisely the "frames
  come in slow" symptom this session removed. It returns to `OAG_FIXED_VBLANK` when a short exposure
  follows a long one.
- **Control write order depends on direction.** The driver derives the exposure control's max from current
  blanking and clamps against it. Going up: widen the frame, then set the longer exposure. Going down: set
  the shorter exposure first (a pending large value can block or be silently clamped by the narrower
  frame), then narrow, then re-assert the exposure.

`extra_settle` is passed whenever blanking actually changed (either direction), since changing the frame
period needs more settling than a plain exposure change.

## ImageBytes ImageElementType was inconsistent with the JSON path (2026-09-16)

The JSON response announces `"Type":2` (Int32) while the ImageBytes header announced
`ImageElementType = 8` (UInt16) — the same image described as two different types depending on transport.
Per the ImageBytes spec these fields mean different things: **ImageElementType** is the type the client
should materialize the array as (ASCOM's `Camera.ImageArray` is an Int32 array, so Int32 = 2), and
**TransmissionElementType** is the narrower type actually on the wire (UInt16 = 8), which the client widens
on receipt. Both were 8. Now 2 and 8 respectively, consistent with the JSON path.

Lenient clients read `TransmissionElementType` and were unaffected, which is why PHD2 and N.I.N.A. work.
This is a **candidate** explanation for SharpCap's remaining black-image behaviour, not a confirmed one —
see below.

## SharpCap still black while PHD2 and N.I.N.A. work (open, 2026-09-16)

After the stripes fix, PHD2 and N.I.N.A. both work correctly; **SharpCap alone still shows black**, and the
user reports it is not an autostretch problem. Three earlier SharpCap theories in this file were wrong, so
the discipline here is: no more fixes shipped as explanations without a captured log.

Ranked candidates:
1. **`ImageElementType` mismatch** (fixed above, untested). SharpCap is the strictest of the three clients
   observed and already produced one hard type error (`element type Unknown is not supported`) earlier in
   this project's history, so it is the one most likely to reject a header field the others ignore.
2. **Value range vs. container.** The data is 10-bit in a 16-bit container: real pixel values here are
   ~60-180 out of 65535, i.e. ~0.2% of full scale. A client that maps 16-bit data to display with a fixed
   shift rather than consulting `MaxADU` (which we correctly report as 1023) gets exactly zero for every
   pixel — "not even noisy, just black", which matches the original description precisely. If this is it,
   the fix is a selectable full-range scaling (10-bit << 6), not a format change.
3. **`SensorType`/Bayer description.** Noted at the time as a genuine bug that would produce wrong
   *colours*, not black. **That is exactly what it turned out to be** — see the Bayer section below.

**RESOLVED (2026-09-16): SharpCap now works.** After the `ImageElementType` fix above (candidate 1) the
user confirmed SharpCap displays images correctly, alongside PHD2 and N.I.N.A. The remaining issue was
colour, which was candidate 3 — see the Bayer section below.

Original next step, kept for the method: run SharpCap against the board and pull `/tmp/alpacad.log`. The
per-request logging already records method, path, `Accept` header and params, so it will show whether
SharpCap is negotiating ImageBytes or JSON, what it sets for `NumX`/`NumY`/`BinX`, and whether it fetches
the image at all.

## On serving JPEG instead of raw16

Asked whether the driver could offer "raw16 or JPEG" as a selectable output. **The Alpaca camera API has no
JPEG option and cannot have one.** `ImageArray`/`ImageBytes` are typed numeric arrays — the JSON form is
`{"Type":<ImageArrayElementType>,"Rank":..,"Value":[[..]]}` and the binary form is a 44-byte header plus
raw elements, where the element type enum covers Int16/Int32/Double/Single/Byte/Int64/UInt16 and nothing
else. There is no encoded-image transport in the interface, so a JPEG would not be something SharpCap,
N.I.N.A. or PHD2 could ask for or decode. It would also be lossy and 8-bit, which defeats the point of an
astronomy camera.

What ASCOM *does* provide for user-selectable output is **`ReadoutModes`/`ReadoutMode`** — a driver-defined
list of named modes that clients (SharpCap included) expose in their UI. That is the correct mechanism if
we want to offer e.g. "Raw 10-bit" vs "Scaled 16-bit" (candidate 2 above), or later a binned mode. Not
implemented yet.

## Hardware verification of the fusion + vblank restore (2026-09-16)

All measured over the real RNDIS link (see the NetworkManager note below — the gadget MAC is randomized
every boot, so the `enx*` interface name changes and a *new* NM profile appears in DHCP limbo on every
replug; a stable gadget MAC would be a real improvement, for Windows too, which otherwise creates a fresh
network profile each plug).

- **Fused unpack verified correct on hardware**: ImageBytes and JSON still elementwise identical, image
  renders clean and sharp, `ImageElementType=2 / TransmissionElementType=8 / Rank=2 / Dim1=2304 Dim2=1296`.
- **Fusion cost/benefit**: `unpack` rose from ~67ms to ~88ms (it now does the transpose too) while `pack`
  went to zero, against ~143ms combined before — net ~55ms, matching the predicted 50-70ms. A 0.02s
  exposure loop measures ~0.49s vs ~0.51-0.54s before. Note `unpack` is now noticeably more *variable*
  (88-158ms observed) where the row-major version was steady at 64-67ms; not yet investigated.
- **`ExposureMax` = 0.899s.** This is the SC3336's real driver-enforced ceiling, and it answers the
  long-standing open question in CLAUDE.md about how far this sensor's exposure can be pushed: ~0.9s, not
  the multi-second exposures the production IMX290 build needs. Good enough for guiding; the go/no-go for
  long exposures still has to be re-answered on the real IMX290.
- **Long exposures apply exactly**: `rows=18214 -> vblank=16990`, `rows=29143 -> vblank=27919`, applied
  exposure equal to requested in both cases.

**Newly measured cost, not yet optimized: the discard frames dominate long exposures.** Alternating
0.02 / 0.5 / 0.02 / 0.8 / 0.02s requests measured 0.49 / 1.39 / 0.91 / 1.90 / 0.78s, with device-side:

```
0.5s : discard=502 dqbuf=448 unpack=158  total=1108
0.02s (after 0.5s): discard=457 dqbuf=32  unpack=129 total=619
0.8s : discard=759 dqbuf=715 unpack=132  total=1610
0.02s (after 0.8s): discard=313 dqbuf=34  unpack=132 total=479
```

So a long exposure costs roughly **two** full frame periods (one discarded, one kept), and a short exposure
*following* a long one pays ~300-460ms discarding frames still running at the old long frame period.

The obvious candidate was `extra_settle`. **Experiment run, and it is obsolete — now removed.** See below.

## extra_settle removed after an A/B experiment (2026-09-16)

`extra_settle` discarded a second frame whenever `vertical_blanking` changed. It was added for a
stale-frame bug seen **under the old single-buffer capture scheme**; the 3-buffer pool plus non-blocking
drain that replaced it makes one discard sufficient. Rather than assume that, it was made runtime-
switchable (`OAG_EXTRA_SETTLE=0/1`) so the *same binary* could be A/B'd, and driven with a 14-exposure
sequence alternating 0.005 / 0.3 / 0.5 / 0.8s in both directions — **starting with a long exposure
immediately after daemon start, which is precisely the case the flag was added for**. Ground truth was the
device's own `OAG_FRAME_STATS` mean, not anything the server reports back about itself.

Correctness with the flag off was exact — every frame's mean tracked its own request, monotonically with
exposure and with no lag on any direction change:

```
rows=10929 mean=435.7   rows=182 mean=77.0   rows=18214 mean=564.7   rows=29143 mean=690.0
```

Timing, same sequence, `extra_settle` on -> off:

| requested | before | after | saved |
|---|---|---|---|
| 0.3s | ~1065ms | ~795ms | 25% |
| 0.5s | 1408ms  | 978ms  | 31% |
| 0.8s | ~1970ms | ~1258ms| 36% |
| 0.005s | ~590ms | ~575ms | — |

The flag, its env override, and the `rounds` loop are all gone; `v4l2_capture_frame()` no longer takes the
parameter. (The 0.005s rows above include `OAG_FRAME_STATS`'s own ~60ms, since it was enabled to get the
means — normal short-exposure loops are ~0.49s.)

## Bayer matrix was reported red/blue-swapped (2026-09-16, fixed and verified)

With SharpCap working, both it and N.I.N.A. rendered colours wrong — brown furniture appearing blue, i.e.
a clean red/blue swap.

**ASCOM's `SensorType` enum has no BGGR/GRBG/GBRG members** — RGGB (2) is its only Bayer value. The actual
arrangement can therefore *only* be conveyed via `BayerOffsetX/Y`, which state where the sensor's top-left
pixel sits inside the reference RGGB 2x2:

```
RGGB tiled:  R G R G      (0,0) -> RGGB     (1,0) -> GRBG
             G B G B      (0,1) -> GBRG     (1,1) -> BGGR
             R G R G
```

The SC3336 is BGGR (V4L2 fourcc `BG10` = `V4L2_PIX_FMT_SBGGR10`) but `bayer_offset_x/y` were both 0, i.e.
"red is top-left" when blue is. Fixed to (1,1).

**Verified empirically rather than from the descriptor**: one frame captured and debayered all four ways.

| assumed pattern | meanR | meanG | meanB | |
|---|---|---|---|---|
| BGGR | 121.1 | 112.6 | 83.7 | warm tungsten lamps, green plants, wooden floor — correct |
| RGGB | 83.7 | 112.6 | 121.1 | exact mirror: blue lamps |
| GRBG | 112.5 | 102.3 | 112.9 | R ~= B with G suppressed — wrong-phase demosaic |
| GBRG | 112.9 | 102.3 | 112.5 | same |

GRBG/GBRG are excluded by the R~=B/G-suppressed signature; BGGR vs RGGB are exact mirrors, decided by
looking at the rendered image (lamps warm, plants green).

**The offsets are now derived from `desc->bayer` by `bayer_offsets()` in `camera_api.c`, and the
`bayer_offset_x/y` fields have been removed from `sensor_desc_t`** — holding the pattern in two places is
what let them drift apart in the first place. Note for later: if real subframing is implemented, an odd
`StartX`/`StartY` shifts the effective pattern and must be XORed into these.

Verified on hardware: `sensortype=2`, `bayeroffsetx=1`, `bayeroffsety=1`.

## USB gadget MAC pinned to the SoC serial (2026-09-16)

`S50usbdevice` never set the RNDIS `host_addr`/`dev_addr`, so `f_rndis` generated random MACs at every
boot. `host_addr` is the MAC the *host* assigns to its own end of the link, so a random one means Linux
renames the interface (`enx<host_addr>`) and NetworkManager creates a fresh, unconfigured profile stuck in
DHCP on every replug — and Windows creates a new network profile each time too. Confirmed directly:
`host_addr` was `82:3b:15:91:0f:4e` while the host interface was `enx823b15910f4e`.

Fixed in the overlay's copy of `S50usbdevice` (a 2-line functional change right after
`mkdir ${USB_FUNCTIONS_DIR}/rndis.gs0`). The MAC is **derived from the SoC's unique chip serial**
(`/proc/cpuinfo` `Serial`, which is also the adb serial) rather than hardcoded, so each board is stable
across reboots but still distinct from every other board — two of these on one PC would otherwise collide.
First octet 0x02 (host) / 0x06 (device): locally administered, unicast.

Verified by a real reboot: `host_addr` came back `02:21:7a:b8:e8:3f`, adb returned in ~10s, and the host
interface is now permanently `enx02217ab8e83f`. Low-risk by construction — the vendor's `test_write` helper
is `test -e $2 && echo $1 > $2`, so a rejected write leaves the gadget coming up exactly as before.

Host-side setup is then a one-time `nmcli con add type ethernet con-name oag-usb ifname enx02217ab8e83f
ipv4.method link-local`, which auto-connects from then on.

Note this forks the vendor `S50usbdevice` into the overlay (as the overlay already does for `RkLunch.sh`
and `insmod_ko.sh`); there is no config-file hook for MACs — `parse_parameter()` only understands the
`ums_*` keys.

## Cooling: not a copied remnant, and not removable (2026-09-16)

Asked whether the camera's cooling setting was left over from some ASCOM example.
It is not. `CanSetCCDTemperature` and `CanGetCoolerPower` are **mandatory members of ASCOM's ICameraV3
interface** -- every conformant camera driver has to answer them -- and this driver answers `false`.
Everything else cooling-related already reports NotImplemented, verified live:

```
cooleron / coolerpower / ccdtemperature / setccdtemperature / heatsinktemperature
    -> ErrorNumber 1024 (0x400, NotImplemented)
cansetccdtemperature / cangetcoolerpower -> false
```

So the driver already declares "no cooler" as loudly as the interface permits; a client still drawing a
cooling panel is making its own UI choice, and the controls should be inert. Removing those two members
would make the driver *less* conformant, not cleaner.

## Dew heater: an Alpaca Switch device (2026-09-16, hardware-verified)

The OpenAstroGuider has a strip heater on the lens. ASCOM's Camera interface has no dew-heater member, so
there is nowhere legitimate to put it there -- but ASCOM's **Switch** interface (ISwitchV2) exists for
exactly this kind of auxiliary control, and one Alpaca server can host several devices. The heater is
therefore a second device at `/api/v1/switch/0/`, enumerated alongside the camera in
`/management/v1/configureddevices`, which SharpCap and N.I.N.A. surface in their existing Switch UI with
no custom support needed.

Exposed as a plain on/off switch (`MinSwitchValue=0`, `MaxSwitchValue=1`, `SwitchStep=1` -- how ISwitchV2
describes a boolean device), per project decision: a percentage would have needed PWM, and
`/sys/class/pwm` is not exported on this kernel (it would need a `pwm` DTS node plus `CONFIG_PWM_SYSFS`).
The value members (`getswitchvalue`/`setswitchvalue`) mirror the boolean ones over 0..1, since ASCOM
clients may drive a switch either way.

**Persistence.** The heater state is stored in `/userdata/dewheater.state` (the board's persistent ext4
partition, reserved for config -- `/tmp` is a RAM disk and would lose it on every power cut) and restored
*and re-applied to the GPIO* by `switch_api_init()` before the HTTP server starts. An unattended rig does
not need someone to re-tick a box after a power cycle. Written with `fsync()` before close, because this
board is USB-bus-powered and "unplug" is an unclean power cut -- the same trap that once zeroed a pushed
`alpacad` binary.

**GPIO binding.** The pin is board wiring, not a user setting, and is unknown until the custom PCB exists.
It is read from `OAG_DEWHEATER_GPIO` (documented and commented out in `S60alpacad`, so no rebuild is
needed to bind it) and driven through `/sys/class/gpio`. Unset, the switch remains a fully working logical
control that drives nothing -- which is what let the Alpaca side be finished and tested before the
hardware exists.

Verified on hardware: device enumeration, all ISwitchV2 metadata, on/off via both `setswitch` and
`setswitchvalue`, invalid id and out-of-range value both rejected with 0x401, and **state restored as ON
across a real reboot**. All three GPIO failure modes degrade gracefully without taking the daemon down:

```
OAG_DEWHEATER_GPIO=9999 -> export failed  -> "gpio 9999 UNAVAILABLE"
OAG_DEWHEATER_GPIO=118  -> pin held by leds-gpio, direction failed -> "gpio 118 UNAVAILABLE"
OAG_DEWHEATER_GPIO=abc  -> "ignoring malformed"
(unset)                 -> "no pin set"
```

The success path (a real pin actually toggling) is **not** verified -- only three GPIOs are claimed on this
board (audio PA, sensor `pwdn`, work LED) and driving an arbitrary unrouted pin on hardware that cannot be
observed is not worth the risk. That test belongs with the PCB.

`common_dispatch()` now takes a `common_device_t` (name, description, interface version, and its own
`Connected` flag + lock) instead of being hardwired to the camera, since ASCOM clients connect to each
device independently. `parse_request_params()` moved from `camera_api.c` into `http_util.h` so both
dispatchers share one copy.

## Setup page: making the dew heater reachable from a client that has no Switch UI (2026-09-16, hardware-verified)

The Switch device above is correct ASCOM, but it only helps in a client that implements ASCOM Switch and
lets the user connect a second device. **PHD2 does not** — it knows about cameras and mounts, and nothing
in its UI will ever enumerate a Switch device. So on the client that matters most for guiding, the heater
was implemented, working and completely unreachable.

PHD2 does have a per-camera **Settings** button. For a classic ASCOM driver that calls the driver's
`SetupDialog()`, which draws a native dialog. For an *Alpaca* device the ASCOM Platform's Alpaca-to-COM
bridge cannot draw a dialog for a driver running on another machine, so it implements `SetupDialog()` by
opening the system browser at the device's setup URL. That is why the button appeared to "just open the
browser at `<board IP>:11111`" — it was doing exactly what it is supposed to do, and nothing was listening.
**A native popup inside PHD2 is not something this driver can provide**; the browser page *is* the
Alpaca equivalent, and once it exists the button does the right thing.

Alpaca defines two setup URLs — `/setup` for the server and `/setup/v1/<devicetype>/<devicenumber>/setup`
per device — and neither was implemented (a conformance gap in its own right, independent of the heater).
`setup_api.c` now serves the same page at both, plus the bare root a user is most likely to type by hand:
whichever door a client opens, the toggle is behind it.

**The page.** One self-contained document — no external stylesheet, script or font, since the only link a
client has to the camera is the USB gadget's link-local network with no route to the internet; anything
fetched from a CDN would simply hang. Dark with a warm/red accent rather than the usual blue, because it
is opened at the telescope in the dark, where a blue-white UI costs the user their dark adaptation for
minutes.

Everything it shows about the camera (`sensorname`, resolution, pixel size, exposure range, `maxadu`,
`driverversion`) is **fetched from the existing Alpaca API by the page itself**, not re-derived in
`setup_api.c`. Two implementations of "what is the exposure range" is how `bayer_offset_x/y` drifted from
`desc->bayer` and swapped red and blue; this page cannot disagree with what clients are told because it is
just another client. The heater is the one exception: its state and GPIO status are rendered into the page
by the server, so the toggle is already in the right position on first paint instead of visibly flipping
once a request comes back — and then re-read from the API anyway, in case a Switch-aware client or a
second tab changed it in the meantime.

The toggle drives `PUT /api/v1/switch/0/setswitch`, the very same endpoint a Switch-aware client would
use, so the two views can never disagree and the change persists to `/userdata/dewheater.state` exactly as
before. The GPIO status is reported in the user's terms rather than the log's, keeping all three cases
distinguishable: "no pin bound on this board" is the expected state on the prototype and must not read
like a fault, while a pin that was asked for and could not be claimed must not read like everything is
fine.

**Routing note.** The root handler is registered as `"/$"`, not `"/"`. civetweb tries an exact match, then
`<handler>/anything`, then *pattern* matching — and as a pattern a bare `"/"` prefix-matches every URL on
the server. An unrecognised `/api/v1/...` path would then quietly return this HTML page instead of a JSON
error, which is a miserable thing to debug. The `$` anchors it to the root. `"/setup"` needs no such care:
civetweb's `<handler>/anything` step is what makes it cover the per-device URLs too.

**Verified on the real board over the real RNDIS link** (not `adb forward`), in a real browser:

- All four URLs (`/`, `/setup`, `/setup/v1/camera/0/setup`, `/setup/v1/switch/0/setup`) return the page,
  `Content-Type: text/html`, with a `Content-Length` that matches the bytes actually sent exactly.
- API routing is unaffected: `/api/v1/camera/0/sensorname` still returns JSON, an unknown member still
  returns a JSON `ErrorNumber 1024` rather than HTML, and `/management/*` is untouched.
- The page renders, populates live (SC3336, 2304 × 1296, 2 × 2 µm, 27 µs – 899.3 ms, 1023 ADU, plus the
  build stamp), and logs no console errors.
- Clicking the toggle in the browser flipped the real device: `getswitch` went to `false`,
  `/userdata/dewheater.state` went to `0`, the daemon logged `[switch] dew heater -> off`, and clicking
  again restored all three.

The exposure minimum is one sensor row (~27 µs), which a fixed millisecond format rendered as a
meaningless `0.0 ms` — caught by looking at the rendered page, not the code. The formatter now switches
between µs, ms and s.
