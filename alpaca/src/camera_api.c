#include "camera_api.h"
#include "common_api.h"
#include "device_state.h"
#include "http_util.h"
#include "util.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Monotonic milliseconds, for precisely timing where per-request latency
 * actually goes (V4L2 setup/capture overhead vs. HTTP serialization/
 * transfer) instead of guessing -- prefixed onto every log line below. */
static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ASCOM's Camera interface types Gain/GainMin/GainMax as Int16 (a holdover
 * from the classic COM interface) -- confirmed via a real N.I.N.A. error:
 * "The JSON value could not be converted to System.Int16 ... Path: $.Value
 * | BytePositionInLine: 14", which lands exactly on the value "99614" (our
 * subdev's raw analogue_gain max) in a {"Value":99614,...} response -- that's
 * 32767 (Int16.MaxValue) blown way past. Fixed by exposing a small,
 * Int16-safe ASCOM-facing scale (0..ASCOM_GAIN_MAX) translated to/from the
 * raw analogue_gain range at the API boundary; g_device.gain itself still
 * stores the raw value applied directly to hardware in exposure_worker().
 *
 * That fix alone wasn't enough, though -- confirmed via a captured request
 * log from a real SharpCap session (2026-09-15): SharpCap's own gain
 * auto-calibration swept up to ASCOM gain ~898/1000, which under a *linear*
 * mapping across the sensor's full native range (128..99614, i.e. 1x..~778x)
 * produced raw gain 89466 (~700x). At that gain even a short exposure
 * saturates completely; SharpCap compensated with a ~2ms exposure, which at
 * ~700x gain reads out as pure noise floor (mean 9/1023 -- indistinguishable
 * from "black" without deep stretching). This isn't a decoding bug -- the
 * server-side capture log confirms the sensor genuinely returned that data
 * for that (gain, duration) pair -- it's that a guide camera has no real use
 * for gains anywhere near 700x (it mostly just amplifies read noise), yet
 * exposing the sensor's full native range through the same 0..1000 scale
 * guarantees any calibration routine that probes "near the top" lands on an
 * unusably extreme value. Fixed by capping the ASCOM-exposed range to a
 * practical ceiling (GAIN_PRACTICAL_MULTIPLIER x, clamped to the sensor's
 * actual max in case a future sensor's native range is smaller) instead of
 * the sensor's full native maximum -- trades away access to the most
 * extreme hardware gain settings for far finer, more usable granularity
 * across the whole 0..1000 scale, and keeps "near the top of the range"
 * meaning "still usable" rather than "700x". */
#define ASCOM_GAIN_MAX 1000
#define GAIN_PRACTICAL_MULTIPLIER 32

static int64_t gain_practical_max(int64_t gmin, int64_t gmax) {
	int64_t ceiling = gmin * GAIN_PRACTICAL_MULTIPLIER;
	return ceiling < gmax ? ceiling : gmax;
}

/* The inverse (ASCOM scale -> raw) went away with the gain pin below; it is
 * a three-line mirror of raw_gain_to_ascom() when client-settable gain comes
 * back. */
static long raw_gain_to_ascom(long raw_v, int64_t gmin, int64_t gmax) {
	int64_t practical_max = gain_practical_max(gmin, gmax);
	if (practical_max <= gmin) return 0;
	long v = (long)(((raw_v - gmin) * ASCOM_GAIN_MAX) / (practical_max - gmin));
	if (v < 0) v = 0;
	if (v > ASCOM_GAIN_MAX) v = ASCOM_GAIN_MAX;
	return v;
}


/* Gain is pinned (project decision, 2026-09-16) at the subdev's own minimum,
 * 128 = 1x. vertical_blanking is NOT pinned any more (restored 2026-09-16
 * once the frame-delivery path was fast enough to stop needing a fixed frame
 * period): exposure_worker() raises it again when a requested exposure
 * exceeds the current frame-length ceiling, which is what makes exposures
 * longer than ~37ms possible at all.
 *
 * OAG_FIXED_VBLANK is now just the value blanking is initialised to at
 * startup, not a cap. Undo the gain pin by restoring the client-settable
 * mapping in the "gain" handler and passing job->gain to v4l2_ctrl_set()
 * again. */
#define OAG_FIXED_GAIN 128
#define OAG_FIXED_VBLANK 64
/* Spare rows kept between the requested exposure and the frame-length ceiling
 * when raising blanking, so the exposure lands strictly inside the frame
 * rather than exactly on the boundary. */
#define VBLANK_HEADROOM 64

void camera_apply_fixed_sensor_settings(const sensor_desc_t *s) {
	/* Blanking is set here only to establish a known starting frame period;
	 * exposure_worker() moves it as needed. */
	v4l2_ctrl_set(s->subdev_path, s->ctrl_vblank, OAG_FIXED_VBLANK);
	v4l2_ctrl_set(s->subdev_path, s->ctrl_gain, OAG_FIXED_GAIN);
	int64_t vb = -1, g = -1;
	v4l2_ctrl_get(s->subdev_path, s->ctrl_vblank, &vb);
	v4l2_ctrl_get(s->subdev_path, s->ctrl_gain, &g);
	fprintf(stderr, "[init] pinned vertical_blanking=%lld analogue_gain=%lld\n",
	        (long long)vb, (long long)g);
}

/* ASCOM's SensorType enum has no BGGR/GRBG/GBRG members -- RGGB (2) is its
 * only Bayer value -- so the actual colour arrangement can ONLY be conveyed
 * through BayerOffsetX/Y, which state where this sensor's top-left pixel sits
 * inside the reference RGGB 2x2:
 *
 *     RGGB tiled:  R G R G     (0,0) -> RGGB    (1,0) -> GRBG
 *                  G B G B     (0,1) -> GBRG    (1,1) -> BGGR
 *                  R G R G
 *
 * This was previously reported as (0,0) for the BGGR SC3336, i.e. "red is
 * top-left" when blue actually is -- a straight red/blue swap, which is
 * exactly how it showed up in SharpCap and N.I.N.A. (brown furniture
 * rendering blue). Confirmed empirically 2026-09-16 by debayering one frame
 * all four ways: BGGR gives warm tungsten lamps, green plants and a wooden
 * floor; RGGB gives the mirror (blue lamps); GRBG/GBRG collapse to R ~= B
 * with G suppressed, the signature of a wrong-phase demosaic.
 *
 * Derived from desc->bayer rather than stored per sensor, so the pattern and
 * the reported offset cannot drift apart -- that duplication is what produced
 * the bug. If real subframing is ever implemented, an odd StartX/StartY shifts
 * the effective pattern and must be XORed in here. */
static void bayer_offsets(bayer_pattern_t p, int *ox, int *oy) {
	switch (p) {
	case BAYER_RGGB: *ox = 0; *oy = 0; break;
	case BAYER_GRBG: *ox = 1; *oy = 0; break;
	case BAYER_GBRG: *ox = 0; *oy = 1; break;
	case BAYER_BGGR: *ox = 1; *oy = 1; break;
	default:         *ox = 0; *oy = 0; break;
	}
}

/* Computes the true achievable exposure ceiling non-invasively: reads the
 * exposure control's max at the CURRENT vertical_blanking plus the
 * vertical_blanking control's own max, and infers the fixed margin between
 * frame length and max exposure (observed empirically: vblank=64 gives
 * exposure max=1352, i.e. margin=(1296+64)-1352=8 rows) without actually
 * touching vertical_blanking just to answer the question. Restored
 * 2026-09-16 along with the vblank-raising in exposure_worker(); while
 * blanking was pinned this just returned the current max. */
static long compute_exposure_max_rows(const sensor_desc_t *s) {
	int64_t vblank_cur = 0, vblank_min = 0, vblank_max = 0;
	int64_t exp_max_cur = 0, exp_min = 0;
	if (v4l2_ctrl_get(s->subdev_path, s->ctrl_vblank, &vblank_cur) != 0)
		return 0;
	if (v4l2_ctrl_get_range(s->subdev_path, s->ctrl_vblank, &vblank_min,
	                         &vblank_max) != 0)
		return 0;
	if (v4l2_ctrl_get_range(s->subdev_path, s->ctrl_exposure, &exp_min,
	                         &exp_max_cur) != 0)
		return 0;

	long margin = (s->height + vblank_cur) - exp_max_cur;
	long true_max = s->height + vblank_max - margin;
	return true_max > 0 ? true_max : exp_max_cur;
}

typedef struct {
	const sensor_desc_t *sensor;
	long rows;
} exposure_job_t;

static void *exposure_worker(void *arg) {
	exposure_job_t *job = (exposure_job_t *)arg;
	const sensor_desc_t *s = job->sensor;

	/* Raise vertical_blanking first if the requested exposure exceeds the
	 * current headroom -- exposure is capped by frame length (height +
	 * vertical_blanking) minus a small fixed margin, so blanking has to move
	 * before a longer exposure will actually apply. Same relationship
	 * grab.py's apply_settings() validated on this sensor family.
	 *
	 * Lower it again when a short exposure follows a long one: blanking left
	 * high keeps the frame period long, which would make every subsequent
	 * short exposure wait out the old, slow frame period -- exactly the
	 * "frames come in slow" symptom this session spent its time removing.
	 * Analogue gain is pinned and is not touched here. */
	int64_t vblank_cur = 0, vblank_min = 0, vblank_max = 0;
	int64_t exp_min = 0, exp_max_cur = 0, exp_cur = 0;
	v4l2_ctrl_get(s->subdev_path, s->ctrl_vblank, &vblank_cur);
	v4l2_ctrl_get(s->subdev_path, s->ctrl_exposure, &exp_cur);
	v4l2_ctrl_get_range(s->subdev_path, s->ctrl_vblank, &vblank_min, &vblank_max);
	v4l2_ctrl_get_range(s->subdev_path, s->ctrl_exposure, &exp_min, &exp_max_cur);

	long margin = (s->height + vblank_cur) - exp_max_cur;
	int64_t need_vblank = job->rows + margin + VBLANK_HEADROOM - s->height;
	if (need_vblank < OAG_FIXED_VBLANK) need_vblank = OAG_FIXED_VBLANK;
	if (need_vblank > vblank_max) need_vblank = vblank_max;
	if (need_vblank < vblank_min) need_vblank = vblank_min;

	/* Order matters, because the driver derives the exposure control's max
	 * from the current blanking and clamps against it. Going UP: widen the
	 * frame first, then set the longer exposure. Going DOWN: shorten the
	 * exposure first, or the pending large value can block (or be silently
	 * clamped by) the narrower frame. */
	int vblank_changed = (need_vblank != vblank_cur);
	if (need_vblank > vblank_cur) {
		v4l2_ctrl_set(s->subdev_path, s->ctrl_vblank, need_vblank);
		v4l2_ctrl_set(s->subdev_path, s->ctrl_exposure, job->rows);
	} else {
		v4l2_ctrl_set(s->subdev_path, s->ctrl_exposure, job->rows);
		if (vblank_changed)
			v4l2_ctrl_set(s->subdev_path, s->ctrl_vblank, need_vblank);
		/* Re-assert: lowering blanking can clamp the value just set. */
		v4l2_ctrl_set(s->subdev_path, s->ctrl_exposure, job->rows);
	}

	/* Sampled after the control writes have completed: any frame that started
	 * before this instant was integrating under the previous settings.
	 *
	 * ...but only when something actually changed. Settling costs a full extra
	 * frame period (the timestamp discard plus the rolling-shutter one), and if
	 * neither exposure nor blanking moved, every frame already in flight was
	 * taken at exactly the settings being asked for -- so there is nothing to
	 * discard and the next frame out is correct by definition. Passing 0 here
	 * disables both discards.
	 *
	 * This is the common case in a guiding loop, where a client repeats the
	 * same exposure indefinitely, and it roughly halves the per-frame time
	 * there: measured 11.9s -> ~6.3s for a repeated 7s exposure. */
	int controls_changed = (need_vblank != vblank_cur) || (job->rows != exp_cur);
	uint64_t settle_ref_ns = controls_changed ? v4l2_capture_now_ns() : 0;

	double t_ctrl_start = now_ms();
	int64_t applied_exposure = -1, applied_vblank = -1;
	v4l2_ctrl_get(s->subdev_path, s->ctrl_exposure, &applied_exposure);
	v4l2_ctrl_get(s->subdev_path, s->ctrl_vblank, &applied_vblank);
	fprintf(stderr,
	        "[t=%.0f] [exposure] requested rows=%ld -> applied exposure=%lld "
	        "vblank=%lld (vblank_changed=%d settle=%d)\n",
	        now_ms(), job->rows, (long long)applied_exposure,
	        (long long)applied_vblank, vblank_changed, controls_changed);

	/* Derive the DQBUF timeout from the frame period actually programmed, so
	 * a long exposure is never cut short by the capture layer's own timeout.
	 * applied_vblank is -1 only if the readback failed; 0.0 then selects the
	 * floor rather than a nonsense negative period. */
	double frame_period_s =
	    applied_vblank >= 0
	        ? (double)(s->height + applied_vblank) * s->row_time_us / 1e6
	        : 0.0;

	double t_capture_start = now_ms();
	v4l2_frame_t frame;
	int ok = (v4l2_capture_frame(&frame, frame_period_s, settle_ref_ns) == 0);
	double t_capture_end = now_ms();
	fprintf(stderr,
	        "[t=%.0f] [exposure] v4l2_capture_frame took %.0fms (ctrl setup "
	        "took %.0fms before that)\n",
	        t_capture_end, t_capture_end - t_capture_start,
	        t_capture_start - t_ctrl_start);

	/* This min/max/mean walk is diagnostic only, and it costs ~60ms per frame
	 * on this CPU (3M pixels) *before* the frame is marked ready -- pure
	 * latency for one log line. Off unless OAG_FRAME_STATS is set in the
	 * environment, so it can be turned back on without a rebuild. */
	if (ok && getenv("OAG_FRAME_STATS") != NULL) {
		long n = (long)frame.width * frame.height;
		uint16_t vmin = 65535, vmax = 0;
		uint64_t sum = 0;
		for (long i = 0; i < n; i++) {
			uint16_t v = frame.pixels[i];
			if (v < vmin) vmin = v;
			if (v > vmax) vmax = v;
			sum += v;
		}
		fprintf(stderr, "[exposure] captured min=%u max=%u mean=%.1f\n", vmin,
		        vmax, (double)sum / (double)n);
	} else if (!ok) {
		fprintf(stderr, "[exposure] v4l2_capture_frame failed\n");
	}

	pthread_mutex_lock(&g_device.lock);
	if (g_device.last_frame.pixels != NULL)
		free(g_device.last_frame.pixels);
	if (ok) {
		g_device.last_frame = frame;
		g_device.image_ready = 1;
		g_device.state = CAM_IDLE;
	} else {
		g_device.last_frame.pixels = NULL;
		g_device.image_ready = 0;
		g_device.state = CAM_ERROR;
	}
	pthread_mutex_unlock(&g_device.lock);

	free(job);
	return NULL;
}

static int h_startexposure(struct mg_connection *conn, params_t *params,
                            long client_txn_id) {
	char buf[256];
	double duration_s = params_get_double(params, "duration", -1.0);
	if (duration_s < 0) {
		alpaca_response_error(buf, sizeof(buf), client_txn_id, 0x401,
		                       "Duration is required");
		send_json(conn, buf);
		return 1;
	}

	pthread_mutex_lock(&g_device.lock);
	const sensor_desc_t *s = g_device.sensor;
	if (g_device.state == CAM_EXPOSING) {
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_error(buf, sizeof(buf), client_txn_id, 0x407,
		                       "Exposure already in progress");
		send_json(conn, buf);
		return 1;
	}
	g_device.state = CAM_EXPOSING;
	g_device.image_ready = 0;
	g_device.last_exposure_duration_s = duration_s;
	pthread_mutex_unlock(&g_device.lock);

	/* Clamp rather than let the driver silently clip: with vertical_blanking
	 * pinned, anything past the exposure control's max is unreachable and a
	 * client asking for more should at least get the frame it can have. */
	long rows = lround(duration_s * 1e6 / s->row_time_us);
	if (rows < 1) rows = 1;
	long rows_max = compute_exposure_max_rows(s);
	if (rows_max > 0 && rows > rows_max) rows = rows_max;

	exposure_job_t *job = malloc(sizeof(*job));
	job->sensor = s;
	job->rows = rows;

	pthread_t t;
	pthread_create(&t, NULL, exposure_worker, job);
	pthread_detach(t);

	alpaca_response(buf, sizeof(buf), NULL, client_txn_id, 0, "");
	send_json(conn, buf);
	return 1;
}

/* CORRECTED 2026-09-15: the wire format is [Width][Height] with Y varying
 * fastest (Value[x][y] = Pixels[y*Width+x]), NOT row-major [Height][Width]
 * as an earlier version of this code claimed. That claim was based on
 * alpyca's client-side reshape code, which just mirrors whatever
 * Dimension1/Dimension2 (or nested-list depth) a server sends without
 * validating it against the spec -- so it never caught the orientation
 * being backwards; pixel statistics (min/max/mean) are transposition-
 * invariant, so even direct byte-level verification against alpyca missed
 * this. Confirmed correct by reading a real, independent reference server
 * implementation (github.com/mikefsq/goalpaca, server/imagearray.go),
 * whose comment states this exact convention explicitly and whose
 * `{"Type":...,"Rank":...,"Value":[...],...}` envelope also revealed a
 * second bug: **this response was missing the required "Type" and "Rank"
 * fields entirely**, which a real SharpCap session surfaced as
 * `image array element type Unknown is not supported (0x8004040b)` --
 * SharpCap reads the missing Type as its zero-default (Unknown). This
 * likely also explains the original ImageBytes-enabled black-image
 * symptom: wrong dimension order silently failing SharpCap's own
 * documented strict frame-dimension validation (drops mismatched frames
 * without an error, unlike the JSON path's more visible failure mode).
 * `Type` follows the same reference server's collapsing convention
 * (jsonImageType): integer types up to 32 bits present as Int32 (2) in
 * JSON regardless of the actual on-the-wire width.
 *
 * Sends application/imagebytes (see send_imagebytes below) when the client
 * requests it via `Accept: application/imagebytes` (this is what real
 * Alpaca clients -- including whatever PHD2/N.I.N.A. use under the hood --
 * negotiate for automatically to avoid the JSON path's ~2x size and heavy
 * parse cost), otherwise falls back to this streamed JSON path.
 *
 * Two-pass: first pass counts the exact output byte length with cheap
 * integer digit-counting (no allocation, no formatting), then a second
 * pass streams the actual content through a small fixed chunk buffer after
 * declaring that exact Content-Length. This gives a real, correct
 * Content-Length (some strict HTTP client libraries -- e.g. .NET
 * HttpClient, which real Alpaca clients like N.I.N.A. are often built on
 * -- are picky about a response with none) *without* ever holding the
 * ~11MB response in memory at once. That matters a lot here: this device
 * has only ~32MB total RAM (as little as 2MB free was observed under
 * load), not the 64MB assumed earlier -- an initial version of this
 * function that buffered the whole response in one malloc'd block OOM'd
 * and crash-rebooted the board during testing. Don't reintroduce that. */
static int udigits(unsigned v) {
	int n = 1;
	while (v >= 10) {
		v /= 10;
		n++;
	}
	return n;
}

/* ASCOM ImageBytes binary transfer (see
 * https://www.ascom-standards.org/Developer/AlpacaImageBytes.pdf) -- 11
 * little-endian int32 fields (44 bytes) followed by raw pixel data. Field
 * layout and ImageArrayElementTypes enum values (UInt16=8) confirmed
 * 2026-09-15 by reading ASCOM's own alpyca reference client source
 * directly (alpaca/camera.py's _build_imagedata_array):
 *   [0:4]   MetadataVersion   [4:8]   ErrorNumber
 *   [8:12]  ClientTransactionID  [12:16] ServerTransactionID
 *   [16:20] DataStart (=44)   [20:24] ImageElementType (Int32=2)
 *   [24:28] TransmissionElementType   [28:32] Rank
 *   [32:36] Dimension1 (=width)  [36:40] Dimension2 (=height)
 *   [40:44] Dimension3 (0 for Rank 2)
 * Dimension1/2 corrected 2026-09-15 (was height/width, backwards -- see the
 * comment above send_imagearray for how this was found and why alpyca alone
 * didn't catch it). The wire pixel order is therefore [Width][Height] (x
 * outer, y inner), NOT a straight copy of the row-major capture buffer --
 * see the transposed streaming loop below. */
#define IMAGEBYTES_HEADER_LEN 44
static void put_le32(unsigned char *p, int32_t v) {
	p[0] = (unsigned char)(v & 0xff);
	p[1] = (unsigned char)((v >> 8) & 0xff);
	p[2] = (unsigned char)((v >> 16) & 0xff);
	p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void send_imagebytes(struct mg_connection *conn, long client_txn_id) {
	int w = g_device.last_frame.width, h = g_device.last_frame.height;
	uint16_t *pixels = g_device.last_frame.pixels;
	long long body_len = IMAGEBYTES_HEADER_LEN + (long long)w * h * (long long)sizeof(uint16_t);

	unsigned char hdr[IMAGEBYTES_HEADER_LEN];
	put_le32(hdr + 0, 1);                          /* MetadataVersion */
	put_le32(hdr + 4, 0);                          /* ErrorNumber */
	put_le32(hdr + 8, (int32_t)client_txn_id);
	put_le32(hdr + 12, (int32_t)alpaca_next_server_txn());
	put_le32(hdr + 16, IMAGEBYTES_HEADER_LEN);     /* DataStart */
	/* ImageElementType is the type the CLIENT should materialize the array
	 * as; TransmissionElementType is the narrower type actually on the wire,
	 * which the client widens on receipt. ASCOM's Camera.ImageArray is an
	 * Int32 array, so ImageElementType is Int32 (2) -- matching what the JSON
	 * path already reports as "Type":2 -- while the wire stays UInt16 (8) to
	 * halve the transfer. These two were both 8 before (2026-09-16), i.e. the
	 * same image was announced as Int32 over JSON and UInt16 over ImageBytes;
	 * lenient clients use TransmissionElementType and never noticed. */
	put_le32(hdr + 20, 2);                         /* ImageElementType = Int32 */
	put_le32(hdr + 24, 8);                         /* TransmissionElementType = UInt16 */
	put_le32(hdr + 28, 2);                         /* Rank */
	put_le32(hdr + 32, w);                         /* Dimension1 = width */
	put_le32(hdr + 36, h);                         /* Dimension2 = height */
	put_le32(hdr + 40, 0);                         /* Dimension3 */

	char len_str[32];
	snprintf(len_str, sizeof(len_str), "%lld", body_len);

	mg_response_header_start(conn, 200);
	mg_response_header_add(conn, "Content-Type", "application/imagebytes", -1);
	mg_response_header_add(conn, "Content-Length", len_str, -1);
	mg_response_header_send(conn);
	mg_write(conn, hdr, sizeof(hdr));

	/* The capture buffer is already in wire order (pixels[x*h+y], X outer) --
	 * desc->unpack produces it that way, so there is nothing to transpose
	 * here any more and the whole frame goes out in one write. This replaced
	 * a separate blocked-transpose pass that cost ~76ms per frame on top of
	 * the unpack's own ~67ms; folding it into the unpack removes a full
	 * write+read pass over ~6MB. */
	double tw0 = now_ms();
	mg_write(conn, pixels, (size_t)w * h * sizeof(uint16_t));
	double t_write_total = now_ms() - tw0;
	fprintf(stderr, "[imagebytes] write=%.0fms\n", t_write_total);
}

static void send_imagearray(struct mg_connection *conn, long client_txn_id) {
	pthread_mutex_lock(&g_device.lock);
	if (!g_device.image_ready || g_device.last_frame.pixels == NULL) {
		pthread_mutex_unlock(&g_device.lock);
		char buf[256];
		alpaca_response_error(buf, sizeof(buf), client_txn_id, 0x400,
		                       "No image available");
		send_json(conn, buf);
		return;
	}

	int w = g_device.last_frame.width, h = g_device.last_frame.height;
	uint16_t *pixels = g_device.last_frame.pixels;

	char head[32];
	int head_len = snprintf(head, sizeof(head), "{\"Type\":2,\"Rank\":2,\"Value\":[");
	char tail[64];
	int tail_len = snprintf(tail, sizeof(tail),
	                         "],\"ClientTransactionID\":%ld,\"ErrorNumber\":0,"
	                         "\"ErrorMessage\":\"\"}",
	                         client_txn_id);

	/* Pass 1: exact length, no allocation. Wire order is [Width][Height]
	 * (x outer, y inner) -- see the comment above send_imagearray. Total
	 * digit/bracket/comma count is identical regardless of which dimension
	 * is outer, so this only needs the loop bounds swapped to match pass 2. */
	size_t total = (size_t)head_len;
	for (int x = 0; x < w; x++) {
		total += (x ? 1 : 0) + 1; /* leading comma (if any) + '[' */
		for (int y = 0; y < h; y++)
			total += (y ? 1 : 0) + (size_t)udigits(pixels[(size_t)x * h + y]);
		total += 1; /* ']' */
	}
	total += (size_t)tail_len;

	mg_send_http_ok(conn, "application/json", (long long)total);

	/* Pass 2: stream through a small fixed buffer, x outer / y inner. */
	char chunk[8192];
	size_t pos = 0;
	memcpy(chunk, head, (size_t)head_len);
	pos += (size_t)head_len;
	for (int x = 0; x < w; x++) {
		if (x) chunk[pos++] = ',';
		chunk[pos++] = '[';
		for (int y = 0; y < h; y++) {
			if (y) chunk[pos++] = ',';
			pos += (size_t)snprintf(chunk + pos, sizeof(chunk) - pos, "%u",
			                         pixels[(size_t)x * h + y]);
			if (pos > sizeof(chunk) - 16) {
				mg_write(conn, chunk, pos);
				pos = 0;
			}
		}
		chunk[pos++] = ']';
		if (pos > sizeof(chunk) - 16) {
			mg_write(conn, chunk, pos);
			pos = 0;
		}
	}
	memcpy(chunk + pos, tail, (size_t)tail_len);
	pos += (size_t)tail_len;
	mg_write(conn, chunk, pos);

	pthread_mutex_unlock(&g_device.lock);
}

static int camera_dispatch(struct mg_connection *conn, void *cbdata) {
	(void)cbdata;
	const struct mg_request_info *ri = mg_get_request_info(conn);
	const char *slash = strrchr(ri->local_uri, '/');
	const char *member = slash ? slash + 1 : ri->local_uri;

	params_t params;
	parse_request_params(conn, &params);
	long client_txn_id = params_get_int(&params, "clienttransactionid", 0);

	/* Request logging (2026-09-15): added while chasing the SharpCap/N.I.N.A.
	 * black-image bug (root cause found: missing Type/Rank fields and a
	 * backwards dimension order, see the comment above send_imagearray).
	 * Kept in since it's generically useful for any future client-behavior
	 * question, not just that one bug. Logs to /tmp/alpacad.log (wherever
	 * stderr is redirected -- see S60alpacad). */
	{
		const char *accept = mg_get_header(conn, "Accept");
		fprintf(stderr, "[t=%.0f] [req] %s /%s accept=%s params=[", now_ms(),
		        ri->request_method, member, accept ? accept : "(none)");
		for (int i = 0; i < params.count; i++)
			fprintf(stderr, "%s%s=%s", i ? "," : "", params.params[i].key,
			        params.params[i].value);
		fprintf(stderr, "]\n");
	}

	const common_device_t dev = {
	    .description = "OpenAstroGuider raw V4L2 camera",
	    .driverinfo = "OpenAstroGuider Alpaca camera driver (alpacad)",
	    .interface_version = 3, /* ICameraV3 */
	    .name = g_device.sensor->display_name,
	    .connected = &g_device.connected,
	    .lock = &g_device.lock,
	};
	if (common_dispatch(conn, &dev, member, ri->request_method, &params,
	                     client_txn_id))
		return 200;

	char buf[256];
	const sensor_desc_t *s;

	if (strcasecmp(member, "cameraxsize") == 0) {
		pthread_mutex_lock(&g_device.lock);
		s = g_device.sensor;
		alpaca_response_int(buf, sizeof(buf), s->width, client_txn_id);
		pthread_mutex_unlock(&g_device.lock);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "cameraysize") == 0) {
		pthread_mutex_lock(&g_device.lock);
		s = g_device.sensor;
		alpaca_response_int(buf, sizeof(buf), s->height, client_txn_id);
		pthread_mutex_unlock(&g_device.lock);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "maxadu") == 0) {
		pthread_mutex_lock(&g_device.lock);
		s = g_device.sensor;
		alpaca_response_int(buf, sizeof(buf), s->max_adu, client_txn_id);
		pthread_mutex_unlock(&g_device.lock);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "sensortype") == 0) {
		pthread_mutex_lock(&g_device.lock);
		s = g_device.sensor;
		/* ASCOM SensorType enum: 0=Monochrome, 2=RGGB (Bayer). This project
		 * only distinguishes mono vs Bayer today; the finer-grained
		 * CMYG/CMYG2/LRGB values aren't reachable from any sensor this
		 * driver currently supports. */
		int val = (s->bayer == BAYER_NONE) ? 0 : 2;
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_int(buf, sizeof(buf), val, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "sensorname") == 0) {
		pthread_mutex_lock(&g_device.lock);
		char name[128];
		snprintf(name, sizeof(name), "%s", g_device.sensor->display_name);
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_string(buf, sizeof(buf), name, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "bayeroffsetx") == 0) {
		pthread_mutex_lock(&g_device.lock);
		s = g_device.sensor;
		int val, oy_unused;
		bayer_offsets(s->bayer, &val, &oy_unused);
		int is_mono = (s->bayer == BAYER_NONE);
		pthread_mutex_unlock(&g_device.lock);
		if (is_mono) {
			alpaca_response_error(buf, sizeof(buf), client_txn_id, 0x400,
			                       "Not valid for a monochrome camera");
		} else {
			alpaca_response_int(buf, sizeof(buf), val, client_txn_id);
		}
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "bayeroffsety") == 0) {
		pthread_mutex_lock(&g_device.lock);
		s = g_device.sensor;
		int ox_unused, val;
		bayer_offsets(s->bayer, &ox_unused, &val);
		int is_mono = (s->bayer == BAYER_NONE);
		pthread_mutex_unlock(&g_device.lock);
		if (is_mono) {
			alpaca_response_error(buf, sizeof(buf), client_txn_id, 0x400,
			                       "Not valid for a monochrome camera");
		} else {
			alpaca_response_int(buf, sizeof(buf), val, client_txn_id);
		}
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "pixelsizex") == 0) {
		pthread_mutex_lock(&g_device.lock);
		double v = g_device.sensor->pixel_size_um_x;
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_double(buf, sizeof(buf), v, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "pixelsizey") == 0) {
		pthread_mutex_lock(&g_device.lock);
		double v = g_device.sensor->pixel_size_um_y;
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_double(buf, sizeof(buf), v, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "binx") == 0 || strcasecmp(member, "biny") == 0 ||
	    strcasecmp(member, "maxbinx") == 0 ||
	    strcasecmp(member, "maxbiny") == 0) {
		/* Binning isn't implemented -- fixed at 1, deliberately out of
		 * scope for this first version (see README.md). */
		alpaca_response_int(buf, sizeof(buf), 1, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "startx") == 0 || strcasecmp(member, "starty") == 0 ||
	    strcasecmp(member, "numx") == 0 || strcasecmp(member, "numy") == 0) {
		/* Accepted/stored/reported for client compatibility -- real
		 * capture always returns the full sensor frame (V4L2 cropping
		 * isn't implemented), see device_state.h. Many Alpaca clients
		 * (N.I.N.A. included) PUT NumX/NumY to the full frame size before
		 * every exposure and abort if that PUT isn't implemented. */
		long *field = strcasecmp(member, "startx") == 0   ? &g_device.start_x
		              : strcasecmp(member, "starty") == 0 ? &g_device.start_y
		              : strcasecmp(member, "numx") == 0    ? &g_device.num_x
		                                                    : &g_device.num_y;
		if (strcasecmp(ri->request_method, "PUT") == 0) {
			pthread_mutex_lock(&g_device.lock);
			*field = params_get_int(&params, member, *field);
			pthread_mutex_unlock(&g_device.lock);
			alpaca_response(buf, sizeof(buf), NULL, client_txn_id, 0, "");
		} else {
			pthread_mutex_lock(&g_device.lock);
			long v = *field;
			pthread_mutex_unlock(&g_device.lock);
			alpaca_response_int(buf, sizeof(buf), v, client_txn_id);
		}
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "cansetccdtemperature") == 0 ||
	    strcasecmp(member, "hasshutter") == 0 ||
	    strcasecmp(member, "canpulseguide") == 0 ||
	    strcasecmp(member, "canasymmetricbin") == 0 ||
	    strcasecmp(member, "canfastreadout") == 0 ||
	    strcasecmp(member, "cangetcoolerpower") == 0) {
		alpaca_response_bool(buf, sizeof(buf), 0, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "canabortexposure") == 0 ||
	    strcasecmp(member, "canstopexposure") == 0) {
		alpaca_response_bool(buf, sizeof(buf), 1, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "gain") == 0) {
		/* Gain is pinned at OAG_FIXED_GAIN (see the top of this file), so a
		 * PUT is accepted and recorded but not applied to hardware -- real
		 * clients abort the whole session if this errors, and this is a
		 * temporary pin, not a permanent capability change. GET reports the
		 * value actually in effect, not the one that was PUT. */
		pthread_mutex_lock(&g_device.lock);
		s = g_device.sensor;
		int64_t gmin = 0, gmax = 0;
		v4l2_ctrl_get_range(s->subdev_path, s->ctrl_gain, &gmin, &gmax);
		if (strcasecmp(ri->request_method, "PUT") == 0) {
			g_device.gain = OAG_FIXED_GAIN;
			pthread_mutex_unlock(&g_device.lock);
			alpaca_response(buf, sizeof(buf), NULL, client_txn_id, 0, "");
		} else {
			long ascom_v = raw_gain_to_ascom(OAG_FIXED_GAIN, gmin, gmax);
			pthread_mutex_unlock(&g_device.lock);
			alpaca_response_int(buf, sizeof(buf), ascom_v, client_txn_id);
		}
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "gainmin") == 0 || strcasecmp(member, "gainmax") == 0) {
		/* Always 0/ASCOM_GAIN_MAX -- see raw_gain_to_ascom/ascom_gain_to_raw
		 * above for why the raw V4L2 range (up to 99614) can't be exposed
		 * directly. */
		alpaca_response_int(buf, sizeof(buf),
		                     strcasecmp(member, "gainmin") == 0 ? 0 : ASCOM_GAIN_MAX,
		                     client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "exposuremin") == 0) {
		pthread_mutex_lock(&g_device.lock);
		s = g_device.sensor;
		int64_t emin = 1, emax = 0;
		v4l2_ctrl_get_range(s->subdev_path, s->ctrl_exposure, &emin, &emax);
		double seconds = (double)emin * s->row_time_us / 1e6;
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_double(buf, sizeof(buf), seconds, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "exposuremax") == 0) {
		pthread_mutex_lock(&g_device.lock);
		s = g_device.sensor;
		long rows = compute_exposure_max_rows(s);
		double seconds = (double)rows * s->row_time_us / 1e6;
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_double(buf, sizeof(buf), seconds, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "exposureresolution") == 0) {
		pthread_mutex_lock(&g_device.lock);
		double seconds = g_device.sensor->row_time_us / 1e6;
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_double(buf, sizeof(buf), seconds, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "camerastate") == 0) {
		pthread_mutex_lock(&g_device.lock);
		int st = g_device.state;
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_int(buf, sizeof(buf), st, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "imageready") == 0) {
		pthread_mutex_lock(&g_device.lock);
		int r = g_device.image_ready;
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_bool(buf, sizeof(buf), r, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "percentcompleted") == 0) {
		pthread_mutex_lock(&g_device.lock);
		int r = g_device.image_ready || g_device.state == CAM_IDLE ? 100 : 0;
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_int(buf, sizeof(buf), r, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "lastexposureduration") == 0) {
		pthread_mutex_lock(&g_device.lock);
		double d = g_device.last_exposure_duration_s;
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_double(buf, sizeof(buf), d, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "imagearray") == 0 ||
	    strcasecmp(member, "imagearrayvariant") == 0) {
		/* Re-enabled 2026-09-15 after finding and fixing the real bug (see
		 * the comment above send_imagearray): was temporarily disabled to
		 * isolate the black-image cause to the binary vs. JSON path, which
		 * ruled out the wrong "extreme gain" theory but the real fix
		 * (missing Type/Rank fields, backwards dimension order) applies to
		 * both paths, so both are back in use. */
		const char *accept = mg_get_header(conn, "Accept");
		pthread_mutex_lock(&g_device.lock);
		int ready = g_device.image_ready && g_device.last_frame.pixels != NULL;
		pthread_mutex_unlock(&g_device.lock);
		double t_send_start = now_ms();
		int used_imagebytes =
		    (ready && accept != NULL && strstr(accept, "application/imagebytes") != NULL);
		if (used_imagebytes) {
			pthread_mutex_lock(&g_device.lock);
			send_imagebytes(conn, client_txn_id);
			pthread_mutex_unlock(&g_device.lock);
		} else {
			send_imagearray(conn, client_txn_id);
		}
		fprintf(stderr, "[t=%.0f] [req] imagearray (%s) send took %.0fms\n",
		        now_ms(), used_imagebytes ? "imagebytes" : "json",
		        now_ms() - t_send_start);
		return 200;
	}
	if (strcasecmp(member, "startexposure") == 0) {
		h_startexposure(conn, &params, client_txn_id);
		return 200;
	}
	if (strcasecmp(member, "stopexposure") == 0 ||
	    strcasecmp(member, "abortexposure") == 0) {
		pthread_mutex_lock(&g_device.lock);
		/* No mid-capture cancellation implemented -- a single v4l2 DQBUF
		 * call is already committed to completing by the time this could
		 * be called. Reset state so the client isn't left waiting
		 * forever; the in-flight capture's result (if it completes) will
		 * still land in last_frame. */
		g_device.state = CAM_IDLE;
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response(buf, sizeof(buf), NULL, client_txn_id, 0, "");
		send_json(conn, buf);
		return 200;
	}

	alpaca_response_error(buf, sizeof(buf), client_txn_id, 0x400,
	                       "Not implemented");
	send_json(conn, buf);
	return 200;
}

void camera_api_register(struct mg_context *ctx) {
	mg_set_request_handler(ctx, "/api/v1/camera/0/*", camera_dispatch, NULL);
}
