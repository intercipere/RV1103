#ifndef GUIDE_LOOP_H
#define GUIDE_LOOP_H

#include "guide_algo.h"
#include "guide_cal.h"
#include "guide_source.h"
#include "guide_star.h"

/*
 * The guide loop: one thread that acquires a frame, re-centroids the guide
 * star, runs each axis through its guide algorithm, and publishes the result.
 *
 * With a mount connected it calibrates first (PHD2's sequence, guide_cal.c),
 * then closes the loop: errors are transformed into RA/Dec, run through the
 * algorithms and sent as pulses. The only mount backend today is the simulated
 * one, which the simulated frame source brings with it -- so the whole cycle
 * runs on the bench, and on the real camera there is still no mount to drive.
 * With no mount, guiding measures and reports in camera pixels, as before.
 *
 * Everything a caller reads goes through guide_loop_snapshot(), which copies
 * under the lock. The HTTP handlers run on civetweb worker threads and must
 * never touch the live state directly.
 */

/* PHD2's graph keeps 400 steps (graph.h DefaultMaxLength) and offers 50, 100,
 * 200 and 400 on its x axis; the target keeps 400 too. */
#define GUIDE_HISTORY 400

/*
 * Mirrors the part of PHD2's GUIDER_STATE that applies here. PHD2 separates
 * looping from guiding for a real reason: you loop to frame and focus the guide
 * scope, which needs frames and a live HFD but no corrections and no star lock.
 * Collapsing the two (as the first version of this did) removes the only mode
 * that is useful before a star has been chosen.
 *
 * CALIBRATING is PHD2's CALIBRATING_PRIMARY. There is no secondary mount (an
 * adaptive-optics unit) and CALIBRATED is transient in PHD2 too, so neither
 * gets a state of its own.
 */
typedef enum {
	GUIDE_STATE_STOPPED = 0,
	GUIDE_STATE_LOOPING,   /* acquiring frames, no star lock, no corrections */
	GUIDE_STATE_SELECTING, /* looking for a star */
	GUIDE_STATE_SELECTED,  /* star locked, looping, not yet guiding */
	GUIDE_STATE_CALIBRATING,
	GUIDE_STATE_GUIDING,
	GUIDE_STATE_LOST,      /* had a star, lost it; keeps trying */
	GUIDE_STATE_ERROR,
} guide_state_t;

/*
 * One guide step, PHD2's S_HISTORY (graph.h) minus what has no source here.
 * Pixels throughout; the page converts to arc-seconds with pixel_scale_arcsec.
 */
#define GUIDE_SAMPLE_CALIBRATED  1 /* ra/dec are meaningful */
#define GUIDE_SAMPLE_RA_LIMITED  2 /* pulse clipped to the max RA duration */
#define GUIDE_SAMPLE_DEC_LIMITED 4
typedef struct {
	long seq;              /* step number, increasing; /guide/state?since= */
	float t;               /* seconds since guiding started */
	float dx, dy;          /* camera offset from the lock position (cameraOffset) */
	float ra, dec;         /* the same in mount axes (mountOffset); 0 uncalibrated */
	float cra, cdec;       /* what the algorithms asked for */
	short ra_ms, dec_ms;   /* pulses actually sent */
	signed char ra_dir, dec_dir; /* guide_dir_t */
	unsigned char flags;
	float mass, snr;
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

	/* RMS is not computed here: PHD2 takes the population sigma over the
	 * graph's x-axis length, which is a display choice, so the page does it
	 * from the history. */
	long steps;     /* guide steps ever recorded; seq of the newest is steps-1 */
	int hist_epoch; /* bumped when the history is discarded (source switch) */
	double guide_t0_ms;
	long frame;
	long lost_frames;
	double last_frame_ms;
	double last_find_ms;
	double last_preview_ms; /* building the 8-bit preview from the frame */

	char algo_ra[24], algo_dec[24];
	double min_move;
	char err[256]; /* wide enough for guide_source_t::err verbatim */

	/* mount and calibration */
	char mount_name[16];
	int mount_connected;
	int calibrated;
	guide_calibration_t cal;
	guide_cal_state_t cal_state;
	int cal_dist_px;          /* how far each calibration leg must go */
	guide_dec_mode_t dec_mode;
	char cal_status[96];
	char cal_advisory[200];
	guide_dir_t pulse_dir_ra, pulse_dir_dec; /* last guide step sent */
	int pulse_ms_ra, pulse_ms_dec;
	long pulses;

	/* Lock position: where guiding is holding the star. Only valid while
	 * guiding -- looping and calibrating have none. */
	int lock_valid;
	double lock_x, lock_y;
	int search_region; /* px either side of the star that Find searches */

	/* image scale: PHD2's GetPixelScale(), 206.265 * um / mm */
	double focal_length_mm;
	double pixel_um;
	unsigned short peak; /* raw peak of the guide star (PHD2 star profile) */
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

/*
 * Drops the calibration and starts guiding, which recalibrates first. PHD2's
 * equivalent is "Clear calibration" followed by Guide. Only meaningful with a
 * mount connected.
 */
void guide_loop_calibrate(void);

/* Forgets the calibration without starting anything. */
void guide_loop_clear_calibration(void);

/* Dec guide mode: off, auto, north only, south only. A calibration made with
 * Dec off covers RA alone, so turning Dec back on makes the next Guide
 * recalibrate (PHD2's Scope::IsCalibrated rule). */
void guide_loop_set_dec_mode(guide_dec_mode_t mode);

/* Stops acquiring. The last state stays readable. */
void guide_loop_stop(void);

/* "Next star": re-runs detection and advances to the next candidate. */
void guide_loop_reselect(void);

/* PHD2's Auto-select: re-runs detection and takes the best candidate. */
void guide_loop_autoselect(void);

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

/*
 * What to draw over a preview: the star and lock position as they were for
 * the frame the preview was made from, in full-frame pixels. Captured in the
 * same locked swap as the pixels, so the overlay cannot belong to a different
 * frame than the image under it -- which fetching /guide/state separately
 * could not guarantee.
 */
typedef struct {
	long frame;
	guide_state_t state; /* PHD2 styles the overlay by guider state */
	int selected;   /* a star is chosen, found on this frame or not */
	int star_found;
	double star_x, star_y;
	int lock_valid;
	double lock_x, lock_y;
	int search_region;
	unsigned int stretch_lo, stretch_hi; /* the preview's display stretch */
	/* GUIDE_CROP x GUIDE_CROP raw pixels around the star, top-left at
	 * (crop_x, crop_y) in frame pixels, row-major. PHD2's star profile uses
	 * the central 21x21 and its zoomed view a 30x30 box around the lock. */
	int crop_valid, crop_x, crop_y;
} guide_preview_meta_t;

#define GUIDE_CROP 41

/* Copies the crop that belongs to the current preview. Returns 0 if none. */
int guide_loop_crop(unsigned short *out, guide_preview_meta_t *meta);

size_t guide_loop_preview(unsigned char *out, size_t cap, int *w, int *h,
                          guide_preview_meta_t *meta);

/* Bytes guide_loop_preview() needs, so a caller can allocate exactly that. */
size_t guide_loop_preview_size(void);

#endif
