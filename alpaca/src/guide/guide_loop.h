#ifndef GUIDE_LOOP_H
#define GUIDE_LOOP_H

#include "guide_algo.h"
#include "guide_source.h"
#include "guide_star.h"

/*
 * The guide loop: one thread that acquires a frame, re-centroids the guide
 * star, runs each axis through its guide algorithm, and publishes the result.
 *
 * It does not talk to a mount. The corrections it computes are published in
 * pixels and stop there -- ST-4 output is not wired yet, so this measures and
 * reports but does not yet close the loop. That is deliberate: the measurement
 * half is worth having working and visible before anything starts pushing a
 * telescope around.
 *
 * Everything a caller reads goes through guide_loop_snapshot(), which copies
 * under the lock. The HTTP handlers run on civetweb worker threads and must
 * never touch the live state directly.
 */

#define GUIDE_HISTORY 100

/*
 * Mirrors the part of PHD2's GUIDER_STATE that applies here. PHD2 separates
 * looping from guiding for a real reason: you loop to frame and focus the guide
 * scope, which needs frames and a live HFD but no corrections and no star lock.
 * Collapsing the two (as the first version of this did) removes the only mode
 * that is useful before a star has been chosen.
 *
 * PHD2's CALIBRATING_PRIMARY / CALIBRATING_SECONDARY / CALIBRATED have no
 * counterpart yet because there is no calibration -- see the README.
 */
typedef enum {
	GUIDE_STATE_STOPPED = 0,
	GUIDE_STATE_LOOPING,   /* acquiring frames, no star lock, no corrections */
	GUIDE_STATE_SELECTING, /* looking for a star */
	GUIDE_STATE_SELECTED,  /* star locked, looping, not yet guiding */
	GUIDE_STATE_GUIDING,
	GUIDE_STATE_LOST,      /* had a star, lost it; keeps trying */
	GUIDE_STATE_ERROR,
} guide_state_t;

typedef struct {
	float ra, dec;   /* measured error, pixels */
	float cra, cdec; /* correction the algorithm asked for, pixels */
	unsigned char valid;
} guide_sample_t;

typedef struct {
	guide_state_t state;
	guide_source_kind_t source;
	char source_name[16];

	int width, height;
	double pixel_scale_arcsec; /* 0 if unknown */
	double exposure_s;

	/* current star */
	double star_x, star_y, snr, hfd, mass;
	int star_found;
	int star_index;  /* which candidate is being tracked, -1 if none */
	int star_count;  /* how many the last detection offered */
	guide_star_result_t last_result;

	/* error history, oldest first, `count` entries */
	guide_sample_t history[GUIDE_HISTORY];
	int count;

	double rms_ra, rms_dec, rms_total;
	long frame;
	long lost_frames;
	double last_frame_ms;
	double last_find_ms;

	char algo_ra[24], algo_dec[24];
	double min_move;
	char err[256]; /* wide enough for guide_source_t::err verbatim */
} guide_status_t;

/*
 * Starts the loop thread. desc is alpacad's sensor_desc_t; the loop uses the
 * in-daemon V4L2 source and shares the streaming device under
 * v4l2_exposure_lock(). Safe to call once at startup -- the thread idles until
 * guide_loop_start() is called, and costs nothing while idle.
 */
int guide_loop_init(const void *desc);

/*
 * Begins looping: acquire frames and update the preview, nothing else. This is
 * PHD2's "Loop Exposures" -- what you use to frame and focus before choosing a
 * star. A star already selected keeps being measured (its HFD is the focus
 * signal) but no corrections are computed.
 */
void guide_loop_loop(void);

/*
 * Begins guiding. Auto-selects a star first if none is selected, so the
 * one-click path still works; with a star already chosen it starts from that.
 */
void guide_loop_start(void);

/* Stops acquiring. The last state stays readable. */
void guide_loop_stop(void);

/* Forces a fresh auto-select on the next frame. */
void guide_loop_reselect(void);

/*
 * Switches frame source. Takes effect on the next frame; the star selection is
 * dropped, since the two sources do not show the same sky. Returns 0 on
 * success. A simulated source needs no camera and works with the lens capped,
 * indoors, or with the mount disconnected.
 */
int guide_loop_set_source(guide_source_kind_t kind);

void guide_loop_set_exposure(double seconds);
void guide_loop_set_algo(int is_dec, guide_algo_kind_t kind);
void guide_loop_set_min_move(double px);

/* Copies the current state out. Safe from any thread. */
void guide_loop_snapshot(guide_status_t *out);

/*
 * Copies the latest 8-bit preview into `out` (at most cap bytes) and reports
 * its geometry. Returns the number of bytes written, or 0 if no preview exists
 * yet. The preview is downsampled by GUIDE_PREVIEW_DOWNSAMPLE; the browser
 * renders it straight into a canvas, which is why there is no encoder here --
 * this image never becomes a PNG or a JPEG on the device. See Guiding/README.md.
 */
#define GUIDE_PREVIEW_DOWNSAMPLE 4
size_t guide_loop_preview(unsigned char *out, size_t cap, int *w, int *h);

/* Bytes guide_loop_preview() needs, so a caller can allocate exactly that. */
size_t guide_loop_preview_size(void);

#endif
