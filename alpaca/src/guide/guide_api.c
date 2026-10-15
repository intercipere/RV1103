#include "guide_api.h"

#include "civetweb.h"
#include "guide_loop.h"
#include "guide_settings.h"
#include "http_util.h"
#include "web.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The page lives in alpaca/web/guide/ as ordinary HTML, CSS and JS, embedded at
 * build time (tools/embed_web.py, served by web.c). It used to be a C string
 * literal here, which was workable at 300 lines and not at the size a port of
 * PHD2's interface needs. Still self-contained: nothing is fetched from a CDN,
 * since the link-local USB network has no route to the internet.
 */

static int h_page(struct mg_connection *conn, void *cbdata) {
	(void)cbdata;
	return web_send(conn, "guide/index.html");
}

static const char *state_name(guide_state_t s) {
	switch (s) {
	case GUIDE_STATE_LOOPING:   return "looping";
	case GUIDE_STATE_SELECTING: return "selecting";
	case GUIDE_STATE_SELECTED:  return "selected";
	case GUIDE_STATE_CALIBRATING: return "calibrating";
	case GUIDE_STATE_GUIDING:   return "guiding";
	case GUIDE_STATE_LOST:      return "lost";
	case GUIDE_STATE_ERROR:     return "error";
	default:                    return "stopped";
	}
}

/* Escapes the few characters that can appear in an error string and would
 * break the JSON. Everything else here is numeric or a fixed token. */
static void json_escape(const char *in, char *out, size_t cap) {
	size_t o = 0;
	for (; *in && o + 2 < cap; in++) {
		if (*in == '"' || *in == '\\') {
			out[o++] = '\\';
			out[o++] = *in;
		} else if ((unsigned char)*in >= 0x20) {
			out[o++] = *in;
		}
	}
	out[o] = '\0';
}

static int h_state(struct mg_connection *conn, void *cbdata) {
	static guide_status_t st; /* large; not worth a worker stack */
	static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
	char *buf;
	/* Worst case is a fresh page asking for all 400 steps at ~110 bytes. */
	size_t cap = 4096 + (size_t)GUIDE_HISTORY * 128, n = 0;
	int i;
	long since = -1;
	char esc[192], esc_cs[128], esc_adv[256], yr[24];
	(void)cbdata;

	/* ?since=<seq> returns only the steps after it: the page keeps its own
	 * copy, so a poll every 100 ms carries one new step, not 400. */
	{
		const struct mg_request_info *ri = mg_get_request_info(conn);
		char v[32];
		if (ri->query_string &&
		    mg_get_var(ri->query_string, strlen(ri->query_string), "since", v, sizeof(v)) > 0)
			since = atol(v);
	}

	buf = malloc(cap);
	if (!buf)
		return 500;

	pthread_mutex_lock(&st_lock);
	guide_loop_snapshot(&st);
	json_escape(st.err, esc, sizeof(esc));
	json_escape(st.cal_status, esc_cs, sizeof(esc_cs));
	json_escape(st.cal_advisory, esc_adv, sizeof(esc_adv));
	/* Rates in px/s, as PHD2 displays them. Dec has none when it was not
	 * calibrated (Dec guide mode off). */
	if (st.cal.y_rate == GUIDE_CAL_RATE_UNCALIBRATED)
		snprintf(yr, sizeof(yr), "null");
	else
		snprintf(yr, sizeof(yr), "%.3f", st.cal.y_rate * 1000.0);

	n += (size_t)snprintf(buf + n, cap - n,
	    "{\"state\":\"%s\",\"source\":\"%s\",\"frame\":%ld,\"lost\":%ld,"
	    "\"exposure\":%.4f,\"width\":%d,\"height\":%d,"
	    "\"star_found\":%s,\"star_x\":%.3f,\"star_y\":%.3f,"
	    "\"star_index\":%d,\"star_count\":%d,"
	    "\"snr\":%.2f,\"hfd\":%.3f,\"mass\":%.0f,"
	    "\"peak\":%u,\"pixel_scale\":%.4f,\"focal_length\":%.1f,\"pixel_um\":%.3f,"
	    "\"min_move\":%.3f,\"frame_ms\":%.1f,\"find_ms\":%.3f,\"preview_ms\":%.1f,"
	    "\"algo_ra\":\"%s\",\"algo_dec\":\"%s\",\"error\":\"%s\",",
	    state_name(st.state), st.source_name, st.frame, st.lost_frames,
	    st.exposure_s, st.width, st.height,
	    st.star_found ? "true" : "false", st.star_x, st.star_y,
	    st.star_index, st.star_count,
	    st.snr, st.hfd, st.mass,
	    (unsigned)st.peak, st.pixel_scale_arcsec, st.focal_length_mm, st.pixel_um,
	    st.min_move, st.last_frame_ms, st.last_find_ms, st.last_preview_ms,
	    st.algo_ra, st.algo_dec, esc);

	n += (size_t)snprintf(buf + n, cap - n,
	    "\"mount\":\"%s\",\"mount_connected\":%s,\"calibrated\":%s,"
	    "\"cal_state\":\"%s\",\"cal_status\":\"%s\",\"cal_advisory\":\"%s\","
	    "\"cal_distance\":%d,\"dec_mode\":%d,"
	    "\"cal\":{\"ra_angle\":%.2f,\"dec_angle\":%.2f,\"ra_rate\":%.3f,"
	    "\"dec_rate\":%s,\"ra_steps\":%d,\"dec_steps\":%d},"
	    "\"pulse_ra\":{\"dir\":\"%s\",\"ms\":%d},"
	    "\"pulse_dec\":{\"dir\":\"%s\",\"ms\":%d},\"pulses\":%ld,",
	    st.mount_name, st.mount_connected ? "true" : "false",
	    st.calibrated ? "true" : "false",
	    guide_cal_state_name(st.cal_state), esc_cs, esc_adv,
	    st.cal_dist_px, (int)st.dec_mode,
	    st.cal.x_angle * 180.0 / 3.14159265358979, st.cal.y_angle * 180.0 / 3.14159265358979,
	    st.cal.x_rate * 1000.0, yr, st.cal.ra_steps, st.cal.dec_steps,
	    guide_dir_name(st.pulse_dir_ra), st.pulse_ms_ra,
	    guide_dir_name(st.pulse_dir_dec), st.pulse_ms_dec, st.pulses);

	/* History as compact rows, oldest first:
	 * [seq, t, dx, dy, ra, dec, ra_ms, ra_dir, dec_ms, dec_dir, flags, mass, snr]
	 * Directions are guide_dir_t: -1 none, 0 N, 1 S, 2 E, 3 W. */
	n += (size_t)snprintf(buf + n, cap - n,
	    "\"hist_epoch\":%d,\"steps\":%ld,\"hist\":[", st.hist_epoch, st.steps);
	{
		int first = 1;
		for (i = 0; i < st.count && n < cap - 160; i++) {
			const guide_sample_t *h = &st.history[i];
			if (h->seq <= since)
				continue;
			n += (size_t)snprintf(buf + n, cap - n,
			    "%s[%ld,%.2f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%u,%.0f,%.1f]",
			    first ? "" : ",", h->seq, (double)h->t, (double)h->dx, (double)h->dy,
			    (double)h->ra, (double)h->dec, h->ra_ms, h->ra_dir, h->dec_ms,
			    h->dec_dir, (unsigned)h->flags, (double)h->mass, (double)h->snr);
			first = 0;
		}
	}
	n += (size_t)snprintf(buf + n, cap - n, "]}");
	pthread_mutex_unlock(&st_lock);

	mg_response_header_start(conn, 200);
	mg_response_header_add(conn, "Content-Type", "application/json", -1);
	mg_response_header_add(conn, "Cache-Control", "no-store", -1);
	{
		char len[32];
		snprintf(len, sizeof(len), "%zu", n);
		mg_response_header_add(conn, "Content-Length", len, -1);
	}
	mg_response_header_send(conn);
	mg_write(conn, buf, n);
	free(buf);
	return 200;
}

static int h_preview(struct mg_connection *conn, void *cbdata) {
	/* Exactly one downsampled frame (576x324 = 182 kB at the current factor),
	 * allocated per request and freed immediately. Asking the loop for the size
	 * rather than guessing a megabyte matters here: this runs while a capture
	 * may be allocating a full 6 MB frame, and the slack is not available. */
	size_t cap = guide_loop_preview_size(), n;
	unsigned char *buf;
	int w = 0, h = 0;
	char v[64];
	guide_preview_meta_t m;
	(void)cbdata;

	if (cap == 0) {
		mg_send_http_error(conn, 503, "no preview yet");
		return 503;
	}
	buf = malloc(cap);
	if (!buf)
		return 500;
	n = guide_loop_preview(buf, cap, &w, &h, &m);
	if (n == 0) {
		free(buf);
		mg_send_http_error(conn, 503, "no preview yet");
		return 503;
	}

	mg_response_header_start(conn, 200);
	mg_response_header_add(conn, "Content-Type", "application/octet-stream", -1);
	mg_response_header_add(conn, "Cache-Control", "no-store", -1);
	snprintf(v, sizeof(v), "%d", w);
	mg_response_header_add(conn, "X-Preview-Width", v, -1);
	snprintf(v, sizeof(v), "%d", h);
	mg_response_header_add(conn, "X-Preview-Height", v, -1);
	/* Overlay for this exact frame, in full-frame pixels: the page divides by
	 * X-Preview-Scale. Sent with the pixels rather than read from /guide/state
	 * so the box can never be drawn on a different frame than it describes. */
	snprintf(v, sizeof(v), "%d", GUIDE_PREVIEW_DOWNSAMPLE);
	mg_response_header_add(conn, "X-Preview-Scale", v, -1);
	snprintf(v, sizeof(v), "%ld", m.frame);
	mg_response_header_add(conn, "X-Frame", v, -1);
	snprintf(v, sizeof(v), "%.2f,%.2f,%d,%d", m.star_x, m.star_y, m.selected, m.star_found);
	mg_response_header_add(conn, "X-Star", v, -1);
	if (m.lock_valid)
		snprintf(v, sizeof(v), "%.2f,%.2f", m.lock_x, m.lock_y);
	else
		snprintf(v, sizeof(v), "none");
	mg_response_header_add(conn, "X-Lock", v, -1);
	snprintf(v, sizeof(v), "%d", m.search_region);
	mg_response_header_add(conn, "X-Search", v, -1);
	mg_response_header_add(conn, "X-State", state_name(m.state), -1);
	snprintf(v, sizeof(v), "%zu", n);
	mg_response_header_add(conn, "Content-Length", v, -1);
	mg_response_header_send(conn);
	mg_write(conn, buf, n);
	free(buf);
	return 200;
}

/*
 * GET /guide/star: GUIDE_CROP x GUIDE_CROP raw uint16 (little-endian, row-major)
 * around the guide star, for the star profile -- 3.4 kB. Geometry and the
 * preview's display stretch in headers, so the zoomed view matches the image.
 */
static int h_star(struct mg_connection *conn, void *cbdata) {
	static unsigned short crop[GUIDE_CROP * GUIDE_CROP];
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	guide_preview_meta_t m;
	char v[64];
	(void)cbdata;

	pthread_mutex_lock(&lock);
	if (!guide_loop_crop(crop, &m)) {
		pthread_mutex_unlock(&lock);
		mg_send_http_error(conn, 503, "no star yet");
		return 503;
	}
	mg_response_header_start(conn, 200);
	mg_response_header_add(conn, "Content-Type", "application/octet-stream", -1);
	mg_response_header_add(conn, "Cache-Control", "no-store", -1);
	snprintf(v, sizeof(v), "%d,%d,%d", m.crop_x, m.crop_y, GUIDE_CROP);
	mg_response_header_add(conn, "X-Crop", v, -1);
	snprintf(v, sizeof(v), "%u,%u", m.stretch_lo, m.stretch_hi);
	mg_response_header_add(conn, "X-Stretch", v, -1);
	snprintf(v, sizeof(v), "%ld", m.frame);
	mg_response_header_add(conn, "X-Frame", v, -1);
	snprintf(v, sizeof(v), "%zu", sizeof(crop));
	mg_response_header_add(conn, "Content-Length", v, -1);
	mg_response_header_send(conn);
	mg_write(conn, crop, sizeof(crop));
	pthread_mutex_unlock(&lock);
	return 200;
}

/*
 * GET /guide/settings: every implemented setting with value, default and
 * range, keyed by PHD2's config key. PUT key=<key>&value=<v> changes one.
 */
static int h_settings(struct mg_connection *conn, void *cbdata) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	char buf[4096];
	(void)cbdata;

	if (!strcmp(ri->request_method, "PUT") || !strcmp(ri->request_method, "POST")) {
		params_t params;
		const char *key, *value;
		char err[160];
		parse_request_params(conn, &params);
		key = params_get(&params, "key");
		value = params_get(&params, "value");
		if (!key || !value) {
			mg_send_http_error(conn, 400, "need key and value");
			return 400;
		}
		if (guide_settings_set(key, value, err, sizeof(err)) != 0) {
			mg_send_http_error(conn, 400, "%s", err);
			return 400;
		}
	}
	guide_settings_json(buf, sizeof(buf));
	send_json(conn, buf);
	return 200;
}

static int h_control(struct mg_connection *conn, void *cbdata) {
	params_t params;
	const char *action;
	(void)cbdata;

	/* Reads the PUT body and the query string both, same as every Alpaca
	 * dispatcher here. */
	parse_request_params(conn, &params);

	action = params_get(&params, "action");
	if (!action) {
		mg_send_http_error(conn, 400, "missing action");
		return 400;
	}

	if (strcasecmp(action, "loop") == 0) {
		guide_loop_loop();
	} else if (strcasecmp(action, "start") == 0) {
		guide_loop_start();
	} else if (strcasecmp(action, "stop") == 0) {
		guide_loop_stop();
	} else if (strcasecmp(action, "reselect") == 0) {
		guide_loop_reselect();
	} else if (strcasecmp(action, "autoselect") == 0) {
		guide_loop_autoselect();
	} else if (strcasecmp(action, "calibrate") == 0) {
		guide_loop_calibrate();
	} else if (strcasecmp(action, "clearcal") == 0) {
		guide_loop_clear_calibration();
	} else if (strcasecmp(action, "decmode") == 0) {
		/* 0 off, 1 auto, 2 north only, 3 south only -- PHD2's values */
		int v = (int)params_get_double(&params, "value", 1.0);
		if (v < GUIDE_DEC_NONE || v > GUIDE_DEC_SOUTH) {
			mg_send_http_error(conn, 400, "bad dec mode");
			return 400;
		}
		guide_loop_set_dec_mode((guide_dec_mode_t)v);
	} else if (strcasecmp(action, "source") == 0) {
		/* 0 = the real camera, 1 = simulated. The page sends the same two
		 * values the loop's enum uses for V4L2 and SIM. */
		int v = (int)params_get_double(&params, "value", 0.0);
		if (guide_loop_set_source(v == 1 ? GUIDE_SRC_SIM : GUIDE_SRC_V4L2) != 0) {
			mg_send_http_error(conn, 400, "bad source");
			return 400;
		}
	} else if (strcasecmp(action, "exposure") == 0) {
		guide_loop_set_exposure(params_get_double(&params, "value", 0.5));
	} else if (strcasecmp(action, "minmove") == 0) {
		guide_loop_set_min_move(params_get_double(&params, "value", 0.15));
	} else if (strcasecmp(action, "algo") == 0) {
		const char *axis = params_get(&params, "axis");
		int v = (int)params_get_double(&params, "value", 0.0);
		if (v < 0 || v > GUIDE_ALGO_IDENTITY) {
			mg_send_http_error(conn, 400, "bad algorithm");
			return 400;
		}
		guide_loop_set_algo(axis && strcasecmp(axis, "dec") == 0,
		                    (guide_algo_kind_t)v);
	} else {
		mg_send_http_error(conn, 400, "unknown action");
		return 400;
	}

	send_json(conn, "{\"ok\":true}");
	return 200;
}

void guide_api_register(struct mg_context *ctx) {
	/* Most specific first: civetweb tries an exact match, then
	 * "<handler>/anything", so /guide alone would otherwise swallow these. */
	mg_set_request_handler(ctx, "/guide/state", h_state, NULL);
	mg_set_request_handler(ctx, "/guide/preview", h_preview, NULL);
	mg_set_request_handler(ctx, "/guide/control", h_control, NULL);
	mg_set_request_handler(ctx, "/guide/star", h_star, NULL);
	mg_set_request_handler(ctx, "/guide/settings", h_settings, NULL);
	mg_set_request_handler(ctx, "/guide", h_page, NULL);
}
