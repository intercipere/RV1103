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

/* Reads the PUT body (application/x-www-form-urlencoded) into `params`,
 * merging in any query-string parameters too -- Alpaca clients are only
 * required to use the body for PUT, but tolerating both is harmless and
 * matches how permissive real-world clients tend to be. */
static void parse_request_params(struct mg_connection *conn, params_t *out) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	params_parse(ri->query_string, out);

	if (strcasecmp(ri->request_method, "PUT") == 0) {
		char body[2048];
		int n = mg_read(conn, body, sizeof(body) - 1);
		if (n > 0) {
			body[n] = '\0';
			params_t body_params;
			params_parse(body, &body_params);
			for (int i = 0; i < body_params.count && out->count < PARAMS_MAX; i++)
				out->params[out->count++] = body_params.params[i];
		}
	}
}

/* Computes the true achievable exposure ceiling non-invasively: reads the
 * exposure control's max at the CURRENT vertical_blanking plus the
 * vertical_blanking control's own max, and infers the fixed margin between
 * frame length and max exposure (observed empirically: default vblank=64
 * gives exposure max=1352, i.e. margin=(1296+64)-1352=8 rows) without
 * actually touching vertical_blanking just to ask the question. See
 * grab.py's apply_settings() for the same relationship used when actually
 * raising the exposure past the current headroom. */
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
	long gain;
} exposure_job_t;

static void *exposure_worker(void *arg) {
	exposure_job_t *job = (exposure_job_t *)arg;
	const sensor_desc_t *s = job->sensor;

	/* Raise vertical_blanking first if the requested exposure exceeds the
	 * current headroom -- exact same relationship grab.py's
	 * apply_settings() already validated on this sensor family. */
	int64_t vblank_cur = 0, vblank_max = 0, exp_min = 0, exp_max_cur = 0;
	v4l2_ctrl_get(s->subdev_path, s->ctrl_vblank, &vblank_cur);
	v4l2_ctrl_get_range(s->subdev_path, s->ctrl_vblank, &exp_min, &vblank_max);
	v4l2_ctrl_get_range(s->subdev_path, s->ctrl_exposure, &exp_min, &exp_max_cur);
	if (job->rows > exp_max_cur) {
		int64_t new_vblank = vblank_cur + (job->rows - exp_max_cur) + 64;
		if (new_vblank > vblank_max) new_vblank = vblank_max;
		v4l2_ctrl_set(s->subdev_path, s->ctrl_vblank, new_vblank);
	}
	v4l2_ctrl_set(s->subdev_path, s->ctrl_gain, job->gain);
	v4l2_ctrl_set(s->subdev_path, s->ctrl_exposure, job->rows);

	v4l2_frame_t frame;
	int ok = (v4l2_capture_frame(s, &frame) == 0);

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
	long gain = g_device.gain;
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

	long rows = lround(duration_s * 1e6 / s->row_time_us);
	if (rows < 1) rows = 1;

	exposure_job_t *job = malloc(sizeof(*job));
	job->sensor = s;
	job->rows = rows;
	job->gain = gain;

	pthread_t t;
	pthread_create(&t, NULL, exposure_worker, job);
	pthread_detach(t);

	alpaca_response(buf, sizeof(buf), NULL, client_txn_id, 0, "");
	send_json(conn, buf);
	return 1;
}

/* Streams the ImageArray as a JSON 2D array directly to the socket instead
 * of building the whole (multi-MB, for a 2304x1296 frame) string in RAM
 * first -- this device has 64MB total RAM. Sent with "Connection: close"
 * and no Content-Length so the body can be streamed without knowing its
 * final size up front (valid per RFC 7230 -- the connection close marks
 * the end of body). ImageBytes (the binary alternative) is not implemented
 * yet -- see README.md.
 *
 * NOTE: row-major [row][col] nesting is what's implemented here; the exact
 * element ordering the Alpaca spec requires has NOT been independently
 * re-verified against the API Reference PDF or a real client (e.g. the
 * ASCOM Conformance tool) -- treat this as unverified until checked.
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

	char tail[64];
	int tail_len = snprintf(tail, sizeof(tail),
	                         "],\"ClientTransactionID\":%ld,\"ErrorNumber\":0,"
	                         "\"ErrorMessage\":\"\"}",
	                         client_txn_id);

	/* Pass 1: exact length, no allocation. */
	size_t total = strlen("{\"Value\":[");
	for (int y = 0; y < h; y++) {
		total += (y ? 1 : 0) + 1; /* leading comma (if any) + '[' */
		for (int x = 0; x < w; x++)
			total += (x ? 1 : 0) + (size_t)udigits(pixels[(size_t)y * w + x]);
		total += 1; /* ']' */
	}
	total += (size_t)tail_len;

	mg_send_http_ok(conn, "application/json", (long long)total);

	/* Pass 2: stream through a small fixed buffer. */
	char chunk[8192];
	size_t pos = 0;
	pos += (size_t)snprintf(chunk + pos, sizeof(chunk) - pos, "{\"Value\":[");
	for (int y = 0; y < h; y++) {
		if (y) chunk[pos++] = ',';
		chunk[pos++] = '[';
		for (int x = 0; x < w; x++) {
			if (x) chunk[pos++] = ',';
			pos += (size_t)snprintf(chunk + pos, sizeof(chunk) - pos, "%u",
			                         pixels[(size_t)y * w + x]);
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

	if (common_dispatch(conn, member, ri->request_method, &params,
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
		int val = s->bayer_offset_x;
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
		int val = s->bayer_offset_y;
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
		if (strcasecmp(ri->request_method, "PUT") == 0) {
			long v = params_get_int(&params, "gain", 128);
			pthread_mutex_lock(&g_device.lock);
			g_device.gain = v;
			pthread_mutex_unlock(&g_device.lock);
			alpaca_response(buf, sizeof(buf), NULL, client_txn_id, 0, "");
		} else {
			pthread_mutex_lock(&g_device.lock);
			long v = g_device.gain;
			pthread_mutex_unlock(&g_device.lock);
			alpaca_response_int(buf, sizeof(buf), v, client_txn_id);
		}
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "gainmin") == 0 || strcasecmp(member, "gainmax") == 0) {
		pthread_mutex_lock(&g_device.lock);
		s = g_device.sensor;
		int64_t gmin = 0, gmax = 0;
		v4l2_ctrl_get_range(s->subdev_path, s->ctrl_gain, &gmin, &gmax);
		pthread_mutex_unlock(&g_device.lock);
		alpaca_response_int(buf, sizeof(buf),
		                     strcasecmp(member, "gainmin") == 0 ? gmin : gmax,
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
		send_imagearray(conn, client_txn_id);
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
