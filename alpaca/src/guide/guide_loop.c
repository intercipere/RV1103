#include "guide_loop.h"

#include "guide_settings.h"
#include "sensor.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Autofind cost and memory both scale with this; 4 is measured as the best
 * trade on this board (321 ms, 0.69 MB) and detection quality is unchanged
 * because the centroid is always computed at full resolution. */
#define AUTOFIND_DOWNSAMPLE 4
#define AUTOFIND_BAND 128
#define SEARCH_REGION 15
#define MIN_HFD 1.5
#define MAX_HFD 25.0

/* Smallest display range the preview stretch will use. Below this the frame
 * has no real structure, and stretching it just renders noise at full
 * contrast -- which reads as a decoding bug rather than an empty sky. */
#define MIN_STRETCH_SPAN 24u

static struct {
	pthread_mutex_t lock;
	pthread_t thread;
	int thread_started;

	/* requests from HTTP threads */
	volatile int want_run;   /* 0 = idle, 1 = looping, 2 = guiding */
	volatile int want_reselect; /* 1 = next candidate, 2 = best (PHD2 Auto-select) */
	volatile int want_source;   /* -1 = no change */
	volatile double want_exposure;
	volatile int want_clear_cal;
	volatile int want_dec_mode; /* -1 = no change */
	volatile int quit;

	const sensor_desc_t *desc;
	guide_source_t src;
	int src_open;

	guide_algo_t algo_ra, algo_dec;
	double min_move;

	/* Owned by the loop thread; HTTP threads only raise the want_* flags. */
	guide_mount_t mount;
	guide_cal_t cal;

	int lock_x, lock_y;
	double origin_x, origin_y;
	int have_star;

	/* Candidates from the last detection, and which one is being tracked.
	 * "Re-select" advances through this list: re-running detection alone is
	 * deterministic and always returns the same brightest star, so the button
	 * looked broken even though it worked. */
	guide_star_t cand[8];
	int ncand;
	int pick;

	guide_status_t st;

	/*
	 * Double-buffered: build_preview() fills the back buffer with no lock
	 * held, and publish_preview() swaps it in under g.lock together with the
	 * overlay metadata. Building in place, as this used to, let an HTTP
	 * thread copy a half-written preview.
	 */
	unsigned char *preview, *preview_back;
	uint16_t *preview16; /* downsampled 10-bit, kept so the stretch is one pass */
	size_t preview_len, back_len;
	int preview_w, preview_h, back_w, back_h;
	int back_ready;
	double back_ms; /* how long the back buffer took to build */
	unsigned int back_lo, back_hi; /* its stretch */
	guide_preview_meta_t pv_meta;
} g = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.want_source = -1,
	.want_dec_mode = -1,
	.want_exposure = 0.0,
	.min_move = 0.15,
};

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/*
 * The simulated source has no exposure of its own and would produce frames as
 * fast as the CPU allows. Pace it like a camera: one frame per exposure, timed
 * from the start of the iteration so processing time is not added on top.
 * The floor keeps a 10 ms exposure from spinning the thread; the real sensor's
 * own floor is its 133 ms shortest frame.
 */
#define SIM_MIN_PERIOD_MS 100.0
static void sim_pace(double t_start_ms) {
	double period = g.st.exposure_s * 1000.0, wait;
	if (period < SIM_MIN_PERIOD_MS)
		period = SIM_MIN_PERIOD_MS;
	wait = period - (now_ms() - t_start_ms);
	if (wait > 0)
		usleep((useconds_t)(wait * 1000.0));
}

/* Caller holds g.lock and fills everything but seq and t. */
static void history_push(guide_sample_t *s) {
	guide_status_t *st = &g.st;
	if (st->count == GUIDE_HISTORY) {
		memmove(&st->history[0], &st->history[1],
		        sizeof(st->history[0]) * (GUIDE_HISTORY - 1));
		st->count--;
	}
	if (st->steps == 0 || st->count == 0)
		st->guide_t0_ms = now_ms();
	s->seq = st->steps++;
	s->t = (float)((now_ms() - st->guide_t0_ms) / 1000.0);
	st->history[st->count++] = *s;
}

/* The raw pixels around the star, for the star profile. Loop thread only;
 * published with the preview. */
static uint16_t g_crop_back[GUIDE_CROP * GUIDE_CROP], g_crop_pub[GUIDE_CROP * GUIDE_CROP];
static int g_crop_back_x, g_crop_back_y, g_crop_back_valid;

static void capture_crop(const guide_image_t *im, double sx, double sy) {
	int x0 = (int)floor(sx + 0.5) - GUIDE_CROP / 2;
	int y0 = (int)floor(sy + 0.5) - GUIDE_CROP / 2;
	int x, y;
	if (im->width < GUIDE_CROP || im->height < GUIDE_CROP)
		return;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x0 > im->width - GUIDE_CROP) x0 = im->width - GUIDE_CROP;
	if (y0 > im->height - GUIDE_CROP) y0 = im->height - GUIDE_CROP;
	for (y = 0; y < GUIDE_CROP; y++)
		for (x = 0; x < GUIDE_CROP; x++)
			g_crop_back[y * GUIDE_CROP + x] = guide_px(im, x0 + x, y0 + y);
	g_crop_back_x = x0;
	g_crop_back_y = y0;
	g_crop_back_valid = 1;
}

/* Copies the mount and calibration picture into the status. Caller holds
 * g.lock. */
static void publish_cal(void) {
	guide_status_t *st = &g.st;
	snprintf(st->mount_name, sizeof(st->mount_name), "%s",
	         g.mount.name ? g.mount.name : "none");
	st->mount_connected = g.mount.connected;
	st->calibrated = guide_cal_is_calibrated(&g.cal);
	st->cal = g.cal.cal;
	st->cal_state = g.cal.state;
	st->cal_dist_px = g.cal.distance_px;
	st->dec_mode = g.cal.dec_mode;
	snprintf(st->cal_status, sizeof(st->cal_status), "%s", g.cal.status);
	snprintf(st->cal_advisory, sizeof(st->cal_advisory), "%s", g.cal.advisory);
	/* Image scale, PHD2's MyFrame::GetPixelScale (binning is always 1). 0 means
	 * unknown, where PHD2 uses 1.0 and falls back to pixels. */
	st->focal_length_mm = guide_settings_get("/frame/focalLength");
	st->pixel_um = g.desc ? g.desc->pixel_size_um_x : 0.0;
	st->pixel_scale_arcsec = st->focal_length_mm > 0 && st->pixel_um > 0
	                             ? 206.265 * st->pixel_um / st->focal_length_mm
	                             : 0.0;
}

/* A calibration only means something if it ran to the end; one interrupted by
 * Stop or Loop is discarded, as PHD2 does. */
static void abandon_calibration(void) {
	if (!g.cal.calibrated && g.cal.state != GUIDE_CAL_CLEARED) {
		guide_cal_clear(&g.cal);
		g.cal.status[0] = '\0';
	}
}

/*
 * Builds the 8-bit preview the browser renders. Box-downsampled, then scaled so
 * the frame's own range fills 0..255 -- a fixed >>2 would show a 10-bit night
 * sky as almost uniformly black, which is the same class of mistake that made
 * SharpCap show black frames. Scaling is display-only; nothing downstream sees
 * these values.
 */
static void build_preview(const guide_image_t *im) {
	const int f = GUIDE_PREVIEW_DOWNSAMPLE;
	int dw = im->width / f, dh = im->height / f;
	size_t need = (size_t)dw * dh;
	int x, y, i, j;
	unsigned int hist[1024];
	unsigned long total, acc_n;
	unsigned int lo = 0, hi = 1023;
	unsigned int span;
	double t0 = now_ms();

	/* The back buffer and preview16 belong to this thread alone, so they can
	 * be resized without the lock. */
	if (g.back_len != need) {
		unsigned char *p = realloc(g.preview_back, need);
		uint16_t *q = realloc(g.preview16, need * sizeof(uint16_t));
		if (!p || !q) {
			/* realloc failure leaves the originals valid; keep whichever
			 * succeeded so the next call can try again from a sane state. */
			if (p) g.preview_back = p;
			if (q) g.preview16 = q;
			return;
		}
		g.preview_back = p;
		g.preview16 = q;
		g.back_len = need;
	}
	g.back_w = dw;
	g.back_h = dh;

	memset(hist, 0, sizeof(hist));

	/*
	 * Column-major frames (alpacad's wire order, y_stride == 1) are walked in
	 * storage order: each source column is read contiguously into a per-
	 * preview-row accumulator. The generic loop below walks rows, which on this
	 * layout strides 2.6 kB between consecutive reads -- measured on the board
	 * at ~100 ms per frame, more than the 72 ms unpack, and the reason the
	 * guide loop was CPU-bound at 5.5 frames/s. Same result either way.
	 */
	if (im->y_stride == 1 && dh <= 1024) {
		unsigned int colacc[1024];
		for (x = 0; x < dw; x++) {
			memset(colacc, 0, sizeof(unsigned int) * (size_t)dh);
			for (i = 0; i < f; i++) {
				const uint16_t *col = im->px + (size_t)(x * f + i) * im->x_stride;
				for (y = 0; y < dh; y++) {
					const uint16_t *p = col + y * f;
					unsigned int a = 0;
					for (j = 0; j < f; j++)
						a += p[j];
					colacc[y] += a;
				}
			}
			for (y = 0; y < dh; y++) {
				unsigned int acc = colacc[y] / (unsigned)(f * f);
				if (acc > 1023)
					acc = 1023;
				g.preview16[(size_t)y * dw + x] = (uint16_t)acc;
				hist[acc]++;
			}
		}
	} else
	for (y = 0; y < dh; y++) {
		for (x = 0; x < dw; x++) {
			unsigned int acc = 0;
			for (j = 0; j < f; j++)
				for (i = 0; i < f; i++)
					acc += guide_px(im, x * f + i, y * f + j);
			acc /= (unsigned)(f * f);
			if (acc > 1023)
				acc = 1023;
			g.preview16[(size_t)y * dw + x] = (uint16_t)acc;
			hist[acc]++;
		}
	}

	/*
	 * Stretch for display. Three things this has to survive, each of which
	 * broke an earlier version:
	 *
	 *  - min/max puts the white point on the brightest hot pixel, so every
	 *    real star renders near black.
	 *  - a "99.9th percentile" white point is still *background*: a guide
	 *    frame is ~99.9% sky, and at 576x324 the 99.9th percentile is pixel
	 *    rank 186, nowhere near the few dozen pixels a star actually covers.
	 *    Stars need a percentile up in the 99.99s.
	 *  - amplifying a flat frame turns read noise into full-contrast static,
	 *    which looks exactly like a broken decoder. A minimum span stops the
	 *    stretch from manufacturing detail that is not there.
	 *
	 * So: black point at the 5th percentile, white point at the 99.99th (the
	 * stars), and a floor under the span.
	 *
	 * The black point is deliberately NOT the median. A median black point
	 * clips half the frame to zero by definition -- fine for a star field,
	 * which is nearly all sky, but it renders any scene with real shadow
	 * detail as half solid black. 5% keeps a normal image readable while
	 * still putting sky noise within a few counts of black.
	 *
	 * Display only. Nothing downstream sees these values and the centroid
	 * never touches them.
	 */
	total = 0;
	for (i = 0; i < 1024; i++)
		total += hist[i];
	if (total == 0)
		return;

	acc_n = 0;
	for (i = 0; i < 1024; i++) {
		acc_n += hist[i];
		if (acc_n * 20 >= total) { /* 5th percentile -> black */
			lo = (unsigned int)i;
			break;
		}
	}
	acc_n = 0;
	hi = lo + 1;
	for (i = 1023; i >= 0; i--) {
		acc_n += hist[i];
		if (acc_n * 10000 >= total) { /* 99.99th percentile -> white */
			hi = (unsigned int)i;
			break;
		}
	}
	if (hi < lo + MIN_STRETCH_SPAN)
		hi = lo + MIN_STRETCH_SPAN;
	span = hi - lo;

	for (y = 0; y < dh; y++) {
		for (x = 0; x < dw; x++) {
			unsigned int v = g.preview16[(size_t)y * dw + x];
			unsigned int o;
			if (v <= lo)
				o = 0;
			else if (v >= hi)
				o = 255;
			else
				o = 255u * (v - lo) / span;
			g.preview_back[(size_t)y * dw + x] = (unsigned char)o;
		}
	}
	g.back_ready = 1;
	g.back_ms = now_ms() - t0;
	g.back_lo = lo;
	g.back_hi = hi;
}

/* Swaps a freshly built preview in and records what to draw over it. Caller
 * holds g.lock and has already updated g.st for this frame. */
static void publish_preview(void) {
	guide_preview_meta_t *m = &g.pv_meta;
	if (g.back_ready) {
		unsigned char *p = g.preview;
		size_t n = g.preview_len;
		g.preview = g.preview_back;
		g.preview_len = g.back_len;
		g.preview_w = g.back_w;
		g.preview_h = g.back_h;
		g.preview_back = p;
		g.back_len = n;
		g.back_ready = 0;
		g.st.last_preview_ms = g.back_ms;
		m->stretch_lo = g.back_lo;
		m->stretch_hi = g.back_hi;
	}
	if (g_crop_back_valid) {
		memcpy(g_crop_pub, g_crop_back, sizeof(g_crop_pub));
		m->crop_x = g_crop_back_x;
		m->crop_y = g_crop_back_y;
		m->crop_valid = 1;
	}
	m->frame = g.st.frame;
	m->state = g.st.state;
	m->selected = g.have_star;
	m->star_found = g.st.star_found;
	m->star_x = g.st.star_x;
	m->star_y = g.st.star_y;
	m->lock_valid = g.st.lock_valid;
	m->lock_x = g.st.lock_x;
	m->lock_y = g.st.lock_y;
	m->search_region = SEARCH_REGION;
}

static int open_source(guide_source_kind_t kind) {
	guide_source_t s;
	int rc;

	if (kind == GUIDE_SRC_SIM)
		rc = guide_source_open_sim(&s, NULL, g.desc->width, g.desc->height, 6);
	else
		rc = guide_source_open_v4l2(&s, g.desc, g.st.exposure_s);

	if (rc != 0) {
		pthread_mutex_lock(&g.lock);
		snprintf(g.st.err, sizeof(g.st.err), "%s", s.err);
		g.st.state = GUIDE_STATE_ERROR;
		pthread_mutex_unlock(&g.lock);
		return -1;
	}
	if (g.src_open)
		guide_source_close(&g.src);
	g.src = s;
	g.src_open = 1;
	g.have_star = 0;

	/* The simulated sky comes with a simulated mount; the real camera has no
	 * mount backend yet. Bind against g.src, not the local copy: the mount
	 * keeps a pointer to the source it moves. */
	if (kind == GUIDE_SRC_SIM)
		guide_source_sim_mount(&g.src, &g.mount);
	else
		guide_mount_none(&g.mount);
	/* A calibration describes one mount; a different source is a different
	 * one. Reopening the same source (Stop, then Start) keeps it, as PHD2
	 * keeps its calibration across guiding sessions. */
	int new_sky = (kind != g.st.source);
	if (new_sky) {
		guide_cal_clear(&g.cal);
		g.cal.status[0] = g.cal.advisory[0] = '\0';
	}

	pthread_mutex_lock(&g.lock);
	g.st.source = kind;
	snprintf(g.st.source_name, sizeof(g.st.source_name), "%s",
	         guide_source_name(&g.src));
	g.st.width = g.src.width;
	g.st.height = g.src.height;
	/* Counters describe one run against one source; carrying them across a
	 * switch would make the frame and lost counts meaningless. */
	/* A different source is a different sky: PHD2 keeps its graph across
	 * stars and guiding runs (only Clear empties it), but nothing from the old
	 * source means anything now. The epoch tells the page to drop its copy. */
	if (new_sky) {
		g.st.count = 0;
		g.st.hist_epoch++;
	}
	g.st.frame = 0;
	g.st.lost_frames = 0;
	g.ncand = 0;
	g.pick = 0;
	g.st.star_found = 0;
	g.st.err[0] = '\0';
	g.st.pulses = 0;
	g.st.lock_valid = 0;
	publish_cal();
	pthread_mutex_unlock(&g.lock);

	guide_algo_reset(&g.algo_ra);
	guide_algo_reset(&g.algo_dec);
	return 0;
}

static void *guide_thread(void *arg) {
	void *scratch = NULL;
	size_t scratch_len = 0;
	(void)arg;

	while (!g.quit) {
		double t_frame, t_find, t_iter = now_ms();
		guide_star_t star;
		guide_star_result_t r;
		int want_sel, explicit_sel;
		int cal_rc = GUIDE_CAL_CONTINUE;
		guide_dir_t pulse_dir[2] = { GUIDE_DIR_NONE, GUIDE_DIR_NONE };
		int pulse_ms[2] = { 0, 0 };

		if (g.want_dec_mode >= 0) {
			g.cal.dec_mode = (guide_dec_mode_t)g.want_dec_mode;
			g.want_dec_mode = -1;
		}
		if (g.want_clear_cal) {
			g.want_clear_cal = 0;
			guide_cal_clear(&g.cal);
			g.cal.status[0] = g.cal.advisory[0] = '\0';
		}
		if (g.want_run != 2)
			abandon_calibration();

		if (!g.want_run) {
			if (g.src_open) {
				guide_source_close(&g.src);
				g.src_open = 0;
			}
			pthread_mutex_lock(&g.lock);
			if (g.st.state != GUIDE_STATE_ERROR)
				g.st.state = GUIDE_STATE_STOPPED;
			publish_cal();
			pthread_mutex_unlock(&g.lock);
			usleep(120 * 1000);
			continue;
		}

		if (g.want_source >= 0) {
			int k = g.want_source;
			g.want_source = -1;
			if (open_source((guide_source_kind_t)k) != 0) {
				g.want_run = 0;
				continue;
			}
		}
		if (g.want_exposure > 0.0) {
			double e = g.want_exposure;
			g.want_exposure = 0.0;
			pthread_mutex_lock(&g.lock);
			g.st.exposure_s = e;
			pthread_mutex_unlock(&g.lock);
			if (g.src_open)
				guide_source_set_exposure(&g.src, e);
		}
		/* Reopen whichever source was last chosen -- after Stop, Start used
		 * to come back on the camera even with Simulated selected. */
		if (!g.src_open && open_source(g.st.source) != 0) {
			g.want_run = 0;
			continue;
		}

		t_frame = now_ms();
		if (guide_source_next(&g.src) != 0) {
			pthread_mutex_lock(&g.lock);
			g.st.state = GUIDE_STATE_ERROR;
			snprintf(g.st.err, sizeof(g.st.err), "%s", g.src.err);
			pthread_mutex_unlock(&g.lock);
			usleep(500 * 1000);
			continue;
		}
		t_frame = now_ms() - t_frame;

		explicit_sel = g.want_reselect;
		/* Looping never auto-selects -- that is the mode you use before you
		 * have chosen a star. Guiding does, so the one-click path works. */
		want_sel = explicit_sel || (g.want_run == 2 && !g.have_star);
		g.want_reselect = 0;

		if (want_sel) {
			size_t need = guide_star_autofind_scratch(
			    g.src.width, g.src.height, AUTOFIND_DOWNSAMPLE, AUTOFIND_BAND);
			guide_star_t found[8];
			int n;

			pthread_mutex_lock(&g.lock);
			g.st.state = GUIDE_STATE_SELECTING;
			pthread_mutex_unlock(&g.lock);

			if (scratch_len != need) {
				void *p = realloc(scratch, need);
				if (!p) {
					pthread_mutex_lock(&g.lock);
					g.st.state = GUIDE_STATE_ERROR;
					snprintf(g.st.err, sizeof(g.st.err),
					         "out of memory for %.2f MB of autofind scratch",
					         need / 1e6);
					pthread_mutex_unlock(&g.lock);
					g.want_run = 0;
					continue;
				}
				scratch = p;
				scratch_len = need;
			}

			n = guide_star_autofind(&g.src.image, SEARCH_REGION,
			                        AUTOFIND_DOWNSAMPLE, AUTOFIND_BAND,
			                        scratch, scratch_len, found,
			                        (int)(sizeof(found) / sizeof(found[0])));
			build_preview(&g.src.image);
			if (n > 0) {
				memcpy(g.cand, found, sizeof(found[0]) * (size_t)n);
				g.ncand = n;
				/* Advance on an explicit re-select; start at the brightest
				 * when this is the loop picking a star on its own. */
				g.pick = explicit_sel == 1 ? (g.pick + 1) % n : 0;
			}
			if (n == 0) {
				pthread_mutex_lock(&g.lock);
				g.st.state = GUIDE_STATE_LOST;
				g.st.star_found = 0;
				g.st.lost_frames++;
				snprintf(g.st.err, sizeof(g.st.err), "no star found");
				g.st.lock_valid = 0;
				publish_preview();
				pthread_mutex_unlock(&g.lock);
				if (g.src.kind == GUIDE_SRC_SIM)
					sim_pace(t_iter);
				continue;
			}
			g.lock_x = (int)(g.cand[g.pick].x + 0.5);
			g.lock_y = (int)(g.cand[g.pick].y + 0.5);
			g.origin_x = g.cand[g.pick].x;
			g.origin_y = g.cand[g.pick].y;
			g.have_star = 1;
			guide_algo_reset(&g.algo_ra);
			guide_algo_reset(&g.algo_dec);
			/* Calibration measures one star's motion; switching stars part
			 * way through would mix two. Start it again from this one. */
			abandon_calibration();
		}

		/* With no star yet (looping before a selection) there is nothing to
		 * centroid; just show the frame. */
		if (!g.have_star) {
			build_preview(&g.src.image);
			pthread_mutex_lock(&g.lock);
			g.st.frame++;
			g.st.last_frame_ms = t_frame;
			g.st.state = GUIDE_STATE_LOOPING;
			g.st.star_found = 0;
			g.st.star_index = -1;
			g.st.star_count = 0;
			g.st.lock_valid = 0;
			publish_preview();
			pthread_mutex_unlock(&g.lock);
			guide_source_release(&g.src);
			if (g.src.kind == GUIDE_SRC_SIM)
				sim_pace(t_iter);
			continue;
		}

		t_find = now_ms();
		r = guide_star_find(&g.src.image, SEARCH_REGION, g.lock_x, g.lock_y,
		                    MIN_HFD, MAX_HFD, &star);
		t_find = now_ms() - t_find;
		/* PHD2's profile keeps the last good data while the star is lost. */
		if (r == GUIDE_STAR_OK)
			capture_crop(&g.src.image, star.x, star.y);

		if (!want_sel)
			build_preview(&g.src.image);

		pthread_mutex_lock(&g.lock);
		g.st.frame++;
		g.st.last_frame_ms = t_frame;
		g.st.last_find_ms = t_find;
		g.st.last_result = r;
		/* A new selection has no lock position until guiding sets one; a lost
		 * frame otherwise keeps the old one, so the crosshair stays put while
		 * the star is missing. */
		if (want_sel)
			g.st.lock_valid = 0;
		/* A simulated source has no exposure; leave the requested value
		 * showing rather than reporting 0. */
		if (g.src.kind != GUIDE_SRC_SIM)
			g.st.exposure_s = g.src.exposure_s;
		snprintf(g.st.algo_ra, sizeof(g.st.algo_ra), "%s",
		         guide_algo_name(g.algo_ra.kind));
		snprintf(g.st.algo_dec, sizeof(g.st.algo_dec), "%s",
		         guide_algo_name(g.algo_dec.kind));
		g.st.min_move = g.min_move;
		g.st.star_index = g.ncand ? g.pick : -1;
		g.st.star_count = g.ncand;

		if (r != GUIDE_STAR_OK) {
			g.st.star_found = 0;
			g.st.state = GUIDE_STATE_LOST;
			g.st.lost_frames++;
			snprintf(g.st.err, sizeof(g.st.err), "star lost: %s",
			         guide_star_result_str(r));
			/* Keep the lock position: the star usually comes back where it
			 * was, and re-running autofind on every dropped frame would cost
			 * a third of a second each time for nothing. */
		} else if (g.want_run == 1) {
			/* Looping with a star selected: report the measurement (HFD is
			 * the focus signal) but compute no corrections and keep no error
			 * history -- none of it would mean anything without guiding. */
			g.st.star_found = 1;
			g.st.star_x = star.x;
			g.st.star_y = star.y;
			g.st.snr = star.snr;
			g.st.hfd = star.hfd;
			g.st.mass = star.mass;
			g.st.peak = star.peak;
			g.st.state = GUIDE_STATE_SELECTED;
			g.st.err[0] = '\0';
			g.st.lock_valid = 0;
			g.lock_x = (int)(star.x + 0.5);
			g.lock_y = (int)(star.y + 0.5);
			/* Track the origin too, so switching to guiding starts from zero
			 * error rather than from however far it drifted while looping. */
			g.origin_x = star.x;
			g.origin_y = star.y;
		} else if (g.mount.connected && !guide_cal_is_calibrated(&g.cal)) {
			/* Calibrating: one step per frame, the pulse sent after the
			 * lock is dropped. PHD2 begins on entering the state and takes
			 * its first measurement on the next frame. */
			g.st.star_found = 1;
			g.st.star_x = star.x;
			g.st.star_y = star.y;
			g.st.snr = star.snr;
			g.st.hfd = star.hfd;
			g.st.mass = star.mass;
			g.st.peak = star.peak;
			g.st.state = GUIDE_STATE_CALIBRATING;
			g.st.err[0] = '\0';
			g.st.lock_valid = 0;

			/* Begin unless one is already under way. A COMPLETE but
			 * insufficient calibration (RA only, Dec since enabled) starts
			 * over; COMPLETE with calibrated == 0 is the Dec-off path waiting
			 * one frame to install its result, and must be updated. */
			if (g.cal.state == GUIDE_CAL_CLEARED || g.cal.calibrated) {
				guide_cal_begin(&g.cal, &g.mount, star.x, star.y);
				snprintf(g.cal.status, sizeof(g.cal.status), "Starting calibration");
			} else {
				cal_rc = guide_cal_update(&g.cal, &g.mount, star.x, star.y);
				pulse_dir[0] = g.cal.move_dir;
				pulse_ms[0] = g.cal.move_ms;
			}
			/* PHD2 draws the lock crosshair during calibration too (dotted
			 * yellow), at the start point the nudges return the star to.
			 * After begin/update, so the first frame shows this run's start
			 * rather than the previous calibration's. */
			if (g.cal.state != GUIDE_CAL_CLEARED && g.cal.initial.valid) {
				g.st.lock_valid = 1;
				g.st.lock_x = g.cal.initial.x;
				g.st.lock_y = g.cal.initial.y;
			}
			if (cal_rc == GUIDE_CAL_DONE) {
				/* PHD2 sets the lock position on entering GUIDING. */
				g.origin_x = star.x;
				g.origin_y = star.y;
				g.st.lock_valid = 1;
				g.st.lock_x = g.origin_x;
				g.st.lock_y = g.origin_y;
				guide_algo_reset(&g.algo_ra);
				guide_algo_reset(&g.algo_dec);
				g.st.state = GUIDE_STATE_GUIDING;
			} else if (cal_rc == GUIDE_CAL_FAILED) {
				/* PHD2 drops back out of guiding; keep the star and keep
				 * looping so the failure can be read and retried. */
				snprintf(g.st.err, sizeof(g.st.err), "%s", g.cal.err);
				g.st.state = GUIDE_STATE_SELECTED;
				g.want_run = 1;
			}
			g.lock_x = (int)(star.x + 0.5);
			g.lock_y = (int)(star.y + 0.5);
		} else {
			double dx = star.x - g.origin_x;
			double dy = star.y - g.origin_y;
			double ex, ey, cra, cdec;
			guide_sample_t smp;

			memset(&smp, 0, sizeof(smp));
			smp.ra_dir = smp.dec_dir = GUIDE_DIR_NONE;

			g.algo_ra.min_move = g.min_move;
			g.algo_dec.min_move = g.min_move;
			if (g.mount.connected) {
				/* Calibrated: PHD2's MoveOffset. The error is turned into
				 * mount axes, the algorithms run there, and the result
				 * becomes pulses. */
				guide_move_t mv;
				guide_cal_guide_step(&g.cal, &g.algo_ra, &g.algo_dec, dx, dy, &mv);
				ex = mv.mount_x;
				ey = mv.mount_y;
				cra = mv.dist_x;
				cdec = mv.dist_y;
				pulse_dir[0] = mv.dir_x;
				pulse_ms[0] = mv.ms_x;
				pulse_dir[1] = mv.dir_y;
				pulse_ms[1] = mv.ms_y;
				g.st.pulse_dir_ra = mv.dir_x;
				g.st.pulse_ms_ra = mv.ms_x;
				g.st.pulse_dir_dec = mv.dir_y;
				g.st.pulse_ms_dec = mv.ms_y;
				smp.flags = GUIDE_SAMPLE_CALIBRATED |
				            (mv.limited_x ? GUIDE_SAMPLE_RA_LIMITED : 0) |
				            (mv.limited_y ? GUIDE_SAMPLE_DEC_LIMITED : 0);
				smp.ra_ms = (short)mv.ms_x;
				smp.dec_ms = (short)mv.ms_y;
				smp.ra_dir = (signed char)(mv.ms_x ? mv.dir_x : GUIDE_DIR_NONE);
				smp.dec_dir = (signed char)(mv.ms_y ? mv.dir_y : GUIDE_DIR_NONE);
			} else {
				/* No mount: measure only, in camera pixels. */
				ex = dx;
				ey = dy;
				cra = guide_algo_result(&g.algo_ra, -dx);
				cdec = guide_algo_result(&g.algo_dec, -dy);
			}

			g.st.star_found = 1;
			g.st.star_x = star.x;
			g.st.star_y = star.y;
			g.st.snr = star.snr;
			g.st.hfd = star.hfd;
			g.st.mass = star.mass;
			g.st.peak = star.peak;
			g.st.state = GUIDE_STATE_GUIDING;
			g.st.err[0] = '\0';
			g.st.lock_valid = 1;
			g.st.lock_x = g.origin_x;
			g.st.lock_y = g.origin_y;
			smp.dx = (float)dx;
			smp.dy = (float)dy;
			if (smp.flags & GUIDE_SAMPLE_CALIBRATED) {
				smp.ra = (float)ex;
				smp.dec = (float)ey;
			}
			smp.cra = (float)cra;
			smp.cdec = (float)cdec;
			smp.mass = (float)star.mass;
			smp.snr = (float)star.snr;
			history_push(&smp);

			/* Follow the star so it cannot walk out of the search region over
			 * a long run; the reported error stays relative to the origin. */
			g.lock_x = (int)(star.x + 0.5);
			g.lock_y = (int)(star.y + 0.5);
		}
		publish_cal();
		publish_preview();
		pthread_mutex_unlock(&g.lock);

		/* Everything needed from this frame -- the centroid and the preview --
		 * has been extracted, so let the ~6 MB go rather than sitting on it
		 * until the next capture. On a 32 MB board this is the difference
		 * between coexisting with an Alpaca client and being OOM-killed. */
		guide_source_release(&g.src);

		/* Pulses go out after the lock is dropped -- an ST-4 pulse blocks
		 * for its whole duration -- and before the next exposure starts, so
		 * that frame sees the mount at rest. RA first, then Dec, as PHD2. */
		{
			int i;
			for (i = 0; i < 2; i++) {
				char perr[128];
				if (pulse_ms[i] <= 0)
					continue;
				if (guide_mount_pulse(&g.mount, pulse_dir[i], pulse_ms[i],
				                      perr, sizeof(perr)) != 0) {
					pthread_mutex_lock(&g.lock);
					g.st.state = GUIDE_STATE_ERROR;
					snprintf(g.st.err, sizeof(g.st.err), "mount: %s", perr);
					pthread_mutex_unlock(&g.lock);
					g.want_run = 0;
					break;
				}
				pthread_mutex_lock(&g.lock);
				g.st.pulses++;
				pthread_mutex_unlock(&g.lock);
			}
		}

		if (g.src.kind == GUIDE_SRC_SIM)
			sim_pace(t_iter);
	}

	free(scratch);
	return NULL;
}

int guide_loop_init(const void *desc) {
	g.desc = (const sensor_desc_t *)desc;
	g.st.exposure_s = 0.5;
	g.st.search_region = SEARCH_REGION;
	g.st.pixel_scale_arcsec = 0.0;
	g.st.source = GUIDE_SRC_V4L2;
	snprintf(g.st.source_name, sizeof(g.st.source_name), "camera");
	g.st.width = g.desc->width;
	g.st.height = g.desc->height;
	g.min_move = 0.15;
	guide_algo_init(&g.algo_ra, GUIDE_ALGO_HYSTERESIS, g.min_move);
	guide_algo_init(&g.algo_dec, GUIDE_ALGO_RESIST_SWITCH, g.min_move);
	guide_mount_none(&g.mount);
	guide_cal_init(&g.cal);
	publish_cal();
	snprintf(g.st.algo_ra, sizeof(g.st.algo_ra), "%s",
	         guide_algo_name(g.algo_ra.kind));
	snprintf(g.st.algo_dec, sizeof(g.st.algo_dec), "%s",
	         guide_algo_name(g.algo_dec.kind));

	if (pthread_create(&g.thread, NULL, guide_thread, NULL) != 0) {
		fprintf(stderr, "[guide] cannot start thread\n");
		return -1;
	}
	g.thread_started = 1;
	return 0;
}

static void reset_run(void) {
	pthread_mutex_lock(&g.lock);
	g.st.err[0] = '\0';
	g.st.lost_frames = 0;
	g.st.frame = 0;
	if (g.st.state == GUIDE_STATE_ERROR)
		g.st.state = GUIDE_STATE_STOPPED;
	pthread_mutex_unlock(&g.lock);
}

void guide_loop_loop(void) {
	reset_run();
	g.want_run = 1;
}

void guide_loop_start(void) {
	reset_run();
	/* Reset the error history: it described the previous run. */
	guide_algo_reset(&g.algo_ra);
	guide_algo_reset(&g.algo_dec);
	g.want_run = 2;
}

void guide_loop_stop(void) { g.want_run = 0; }

void guide_loop_calibrate(void) {
	g.want_clear_cal = 1;
	guide_loop_start();
}

void guide_loop_clear_calibration(void) { g.want_clear_cal = 1; }

void guide_loop_set_dec_mode(guide_dec_mode_t mode) {
	if (mode < GUIDE_DEC_NONE || mode > GUIDE_DEC_SOUTH)
		return;
	g.want_dec_mode = (int)mode;
}
void guide_loop_reselect(void) { g.want_reselect = 1; }

void guide_loop_autoselect(void) {
	/* PHD2's Auto-select works from a looping guider; start looping if
	 * nothing is running so the button does something from a standstill. */
	g.want_reselect = 2;
	if (!g.want_run) {
		reset_run();
		g.want_run = 1;
	}
}

int guide_loop_set_source(guide_source_kind_t kind) {
	if (kind != GUIDE_SRC_SIM && kind != GUIDE_SRC_V4L2)
		return -1;
	/* Only drop the current star -- do not request a new one. The two sources
	 * do not show the same sky, so the old selection is meaningless, but
	 * forcing a fresh selection would defeat looping, which exists precisely
	 * for the state where no star has been chosen. Guiding re-selects on its
	 * own when it finds none. */
	g.want_source = (int)kind;
	return 0;
}

void guide_loop_set_exposure(double seconds) {
	if (seconds < 0.0001) seconds = 0.0001;
	if (seconds > 20.0) seconds = 20.0;
	g.want_exposure = seconds;
}

void guide_loop_set_algo(int is_dec, guide_algo_kind_t kind) {
	pthread_mutex_lock(&g.lock);
	if (is_dec)
		guide_algo_init(&g.algo_dec, kind, g.min_move);
	else
		guide_algo_init(&g.algo_ra, kind, g.min_move);
	pthread_mutex_unlock(&g.lock);
}

void guide_loop_set_min_move(double px) {
	if (px < 0.0) px = 0.0;
	if (px > 5.0) px = 5.0;
	g.min_move = px;
}

void guide_loop_snapshot(guide_status_t *out) {
	pthread_mutex_lock(&g.lock);
	*out = g.st;
	pthread_mutex_unlock(&g.lock);
}

size_t guide_loop_preview_size(void) {
	size_t n;
	pthread_mutex_lock(&g.lock);
	n = g.preview_len;
	pthread_mutex_unlock(&g.lock);
	return n;
}

int guide_loop_crop(unsigned short *out, guide_preview_meta_t *meta) {
	int ok;
	pthread_mutex_lock(&g.lock);
	ok = g.pv_meta.crop_valid;
	if (ok)
		memcpy(out, g_crop_pub, sizeof(g_crop_pub));
	if (meta)
		*meta = g.pv_meta;
	pthread_mutex_unlock(&g.lock);
	return ok;
}

size_t guide_loop_preview(unsigned char *out, size_t cap, int *w, int *h,
                          guide_preview_meta_t *meta) {
	size_t n;
	pthread_mutex_lock(&g.lock);
	n = g.preview_len;
	if (n == 0 || n > cap) {
		pthread_mutex_unlock(&g.lock);
		*w = *h = 0;
		return 0;
	}
	memcpy(out, g.preview, n);
	*w = g.preview_w;
	*h = g.preview_h;
	if (meta)
		*meta = g.pv_meta;
	pthread_mutex_unlock(&g.lock);
	return n;
}
