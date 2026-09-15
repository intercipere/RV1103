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

## Known gaps / not yet done

- **No mid-capture cancellation.** `stopexposure`/`abortexposure` reset the reported state but the
  in-flight V4L2 `DQBUF` call still runs to completion in its worker thread; its result still lands in
  `last_frame` when done. Fine for now (captures are sub-second), would matter for longer exposures.
- **Single exposure at a time, not deeply concurrency-hardened.** `startexposure` refuses a second call
  while one is in flight, but there's no queueing/cancellation beyond that.
- **Gain/exposure aren't restored to any particular value between captures** — `device_state.gain`
  defaults to the subdev's own default (128 = 1x) but there's no "reset to default" on connect.
