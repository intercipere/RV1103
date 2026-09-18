#include "guide_loop.h"

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
	volatile int want_reselect;
	volatile int want_source;   /* -1 = no change */
	volatile double want_exposure;
	volatile int quit;

	const sensor_desc_t *desc;
	guide_source_t src;
	int src_open;

	guide_algo_t algo_ra, algo_dec;
	double min_move;

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

	unsigned char *preview;
	uint16_t *preview16; /* downsampled 10-bit, kept so the stretch is one pass */
	size_t preview_len;
	int preview_w, preview_h;
} g = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.want_source = -1,
	.want_exposure = 0.0,
	.min_move = 0.15,
};

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void history_push(double ra, double dec, double cra, double cdec) {
	guide_status_t *st = &g.st;
	if (st->count == GUIDE_HISTORY) {
		memmove(&st->history[0], &st->history[1],
		        sizeof(st->history[0]) * (GUIDE_HISTORY - 1));
		st->count--;
	}
	st->history[st->count].ra = (float)ra;
	st->history[st->count].dec = (float)dec;
	st->history[st->count].cra = (float)cra;
	st->history[st->count].cdec = (float)cdec;
	st->history[st->count].valid = 1;
	st->count++;
}

static void recompute_rms(void) {
	guide_status_t *st = &g.st;
	double sr = 0, sd = 0;
	int i, n = 0;
	for (i = 0; i < st->count; i++) {
		if (!st->history[i].valid)
			continue;
		sr += (double)st->history[i].ra * st->history[i].ra;
		sd += (double)st->history[i].dec * st->history[i].dec;
		n++;
	}
	if (n == 0) {
		st->rms_ra = st->rms_dec = st->rms_total = 0.0;
		return;
	}
	st->rms_ra = sqrt(sr / n);
	st->rms_dec = sqrt(sd / n);
	st->rms_total = sqrt((sr + sd) / (2 * n));
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

	if (g.preview_len != need) {
		unsigned char *p = realloc(g.preview, need);
		uint16_t *q = realloc(g.preview16, need * sizeof(uint16_t));
		if (!p || !q) {
			/* realloc failure leaves the originals valid; keep whichever
			 * succeeded so the next call can try again from a sane state. */
			if (p) g.preview = p;
			if (q) g.preview16 = q;
			return;
		}
		g.preview = p;
		g.preview16 = q;
		g.preview_len = need;
	}
	g.preview_w = dw;
	g.preview_h = dh;

	memset(hist, 0, sizeof(hist));
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
			g.preview[(size_t)y * dw + x] = (unsigned char)o;
		}
	}
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

	pthread_mutex_lock(&g.lock);
	g.st.source = kind;
	snprintf(g.st.source_name, sizeof(g.st.source_name), "%s",
	         guide_source_name(&g.src));
	g.st.width = g.src.width;
	g.st.height = g.src.height;
	/* Counters describe one run against one source; carrying them across a
	 * switch would make the frame and lost counts meaningless. */
	g.st.count = 0;
	g.st.frame = 0;
	g.st.lost_frames = 0;
	g.ncand = 0;
	g.pick = 0;
	g.st.rms_ra = g.st.rms_dec = g.st.rms_total = 0.0;
	g.st.star_found = 0;
	g.st.err[0] = '\0';
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
		double t_frame, t_find;
		guide_star_t star;
		guide_star_result_t r;
		int want_sel, explicit_sel;

		if (!g.want_run) {
			if (g.src_open) {
				guide_source_close(&g.src);
				g.src_open = 0;
			}
			pthread_mutex_lock(&g.lock);
			if (g.st.state != GUIDE_STATE_ERROR)
				g.st.state = GUIDE_STATE_STOPPED;
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
		if (!g.src_open && open_source(GUIDE_SRC_V4L2) != 0) {
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
				g.pick = explicit_sel ? (g.pick + 1) % n : 0;
			}
			if (n == 0) {
				pthread_mutex_lock(&g.lock);
				g.st.state = GUIDE_STATE_LOST;
				g.st.star_found = 0;
				g.st.lost_frames++;
				snprintf(g.st.err, sizeof(g.st.err), "no star found");
				pthread_mutex_unlock(&g.lock);
				continue;
			}
			g.lock_x = (int)(g.cand[g.pick].x + 0.5);
			g.lock_y = (int)(g.cand[g.pick].y + 0.5);
			g.origin_x = g.cand[g.pick].x;
			g.origin_y = g.cand[g.pick].y;
			g.have_star = 1;
			g.st.count = 0;
			guide_algo_reset(&g.algo_ra);
			guide_algo_reset(&g.algo_dec);
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
			pthread_mutex_unlock(&g.lock);
			guide_source_release(&g.src);
			if (g.src.kind == GUIDE_SRC_SIM)
				usleep(700 * 1000);
			continue;
		}

		t_find = now_ms();
		r = guide_star_find(&g.src.image, SEARCH_REGION, g.lock_x, g.lock_y,
		                    MIN_HFD, MAX_HFD, &star);
		t_find = now_ms() - t_find;

		if (!want_sel)
			build_preview(&g.src.image);

		pthread_mutex_lock(&g.lock);
		g.st.frame++;
		g.st.last_frame_ms = t_frame;
		g.st.last_find_ms = t_find;
		g.st.last_result = r;
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
			g.st.state = GUIDE_STATE_SELECTED;
			g.st.err[0] = '\0';
			g.lock_x = (int)(star.x + 0.5);
			g.lock_y = (int)(star.y + 0.5);
			/* Track the origin too, so switching to guiding starts from zero
			 * error rather than from however far it drifted while looping. */
			g.origin_x = star.x;
			g.origin_y = star.y;
		} else {
			double dx = star.x - g.origin_x;
			double dy = star.y - g.origin_y;
			double cra, cdec;

			g.algo_ra.min_move = g.min_move;
			g.algo_dec.min_move = g.min_move;
			cra = guide_algo_result(&g.algo_ra, -dx);
			cdec = guide_algo_result(&g.algo_dec, -dy);

			g.st.star_found = 1;
			g.st.star_x = star.x;
			g.st.star_y = star.y;
			g.st.snr = star.snr;
			g.st.hfd = star.hfd;
			g.st.mass = star.mass;
			g.st.state = GUIDE_STATE_GUIDING;
			g.st.err[0] = '\0';
			history_push(dx, dy, cra, cdec);
			recompute_rms();

			/* Follow the star so it cannot walk out of the search region over
			 * a long run; the reported error stays relative to the origin. */
			g.lock_x = (int)(star.x + 0.5);
			g.lock_y = (int)(star.y + 0.5);
		}
		pthread_mutex_unlock(&g.lock);

		/* Everything needed from this frame -- the centroid and the preview --
		 * has been extracted, so let the ~6 MB go rather than sitting on it
		 * until the next capture. On a 32 MB board this is the difference
		 * between coexisting with an Alpaca client and being OOM-killed. */
		guide_source_release(&g.src);

		/* A simulated source produces frames as fast as the CPU allows, which
		 * would spin this thread at 100% for no benefit. Pace it at something
		 * near a plausible guide cadence. */
		if (g.src.kind == GUIDE_SRC_SIM)
			usleep(700 * 1000);
	}

	free(scratch);
	return NULL;
}

int guide_loop_init(const void *desc) {
	g.desc = (const sensor_desc_t *)desc;
	g.st.exposure_s = 0.5;
	g.st.pixel_scale_arcsec = 0.0;
	g.st.source = GUIDE_SRC_V4L2;
	snprintf(g.st.source_name, sizeof(g.st.source_name), "camera");
	g.st.width = g.desc->width;
	g.st.height = g.desc->height;
	g.min_move = 0.15;
	guide_algo_init(&g.algo_ra, GUIDE_ALGO_HYSTERESIS, g.min_move);
	guide_algo_init(&g.algo_dec, GUIDE_ALGO_RESIST_SWITCH, g.min_move);
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
	g.st.count = 0;
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
void guide_loop_reselect(void) { g.want_reselect = 1; }

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

size_t guide_loop_preview(unsigned char *out, size_t cap, int *w, int *h) {
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
	pthread_mutex_unlock(&g.lock);
	return n;
}
