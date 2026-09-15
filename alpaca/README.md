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

## Known gaps / not yet done

- **No mid-capture cancellation.** `stopexposure`/`abortexposure` reset the reported state but the
  in-flight V4L2 `DQBUF` call still runs to completion in its worker thread; its result still lands in
  `last_frame` when done. Fine for now (captures are sub-second), would matter for longer exposures.
- **Single exposure at a time, not deeply concurrency-hardened.** `startexposure` refuses a second call
  while one is in flight, but there's no queueing/cancellation beyond that.
- **Gain/exposure aren't restored to any particular value between captures** — `device_state.gain`
  defaults to the subdev's own default (128 = 1x) but there's no "reset to default" on connect.
- **`mg_write()`'s real network throughput hasn't been re-measured on the actual `usb0` RNDIS link since the
  transpose fix** — see "Latency: conditional discard + cache-blocked transpose" above. This host can only
  reach the board via `adb`, not the RNDIS interface, so the ~620-655ms figure measured this session is
  likely a tunnel artifact, not a real number.
- **Not yet retested against a real SharpCap/N.I.N.A./PHD2 session** (this session verified correctness and
  latency via direct HTTP calls and log inspection, not a live ASCOM client) — the underlying data
  (dimension order, `Type`/`Rank` fields, exposure/gain correctness) is hardware-verified correct, so this
  is expected to work and be fast, but a real client hasn't confirmed it end-to-end since these fixes.
