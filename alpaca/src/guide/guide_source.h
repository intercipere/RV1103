#ifndef GUIDE_SOURCE_H
#define GUIDE_SOURCE_H

#include "guide_mount.h"
#include "guide_star.h"

/*
 * Where guide frames come from.
 *
 * Two sources, deliberately interchangeable so the guide loop above them cannot
 * tell which it is running on:
 *
 *   GUIDE_SRC_CAMERA     live frames from the camera, fetched from the running
 *                        alpacad over HTTP on localhost. Going through the
 *                        Alpaca API rather than opening /dev/video0 directly is
 *                        what makes this work at all while the daemon is
 *                        serving PHD2 -- the V4L2 device has a single owner and
 *                        alpacad already is it.
 *
 *                        MEMORY WARNING, measured on the RV1103 (32 MB):
 *                        alpacad retains a whole 5.97 MB frame after every
 *                        exposure (VmRSS 12.5 -> 18.4 MB, measured) and holds
 *                        two transiently while capturing the next one. A
 *                        separate process that also holds a frame does not fit
 *                        beside it: doing this repeatedly OOM-killed alpacad
 *                        twice during development. Occasional single frames are
 *                        fine; a continuous loop is not. This is exactly why
 *                        the real guide loop belongs INSIDE alpacad, sharing
 *                        the frame it already has, rather than being a separate
 *                        process pulling frames over HTTP. On the production
 *                        256 MB part the constraint disappears.
 *   GUIDE_SRC_SIM        a synthetic star field that drifts the way a mount
 *                        does, laid over a real frame from this sensor so the
 *                        noise, pedestal and hot pixels are genuine. Star
 *                        positions are known exactly, so the centroid can be
 *                        scored rather than eyeballed.
 *
 * The simulated source is not a stand-in for the real one. It is the only way
 * to exercise the guide loop in daylight, indoors, or with no mount attached,
 * and the only way to get ground truth at all -- a real star field tells you
 * where the star appears, never where it actually was.
 */

typedef enum {
	GUIDE_SRC_CAMERA = 0, /* live, via the Alpaca HTTP API (host-side testing) */
	GUIDE_SRC_SIM,        /* simulated star field over a real noise frame */
	GUIDE_SRC_V4L2,       /* live, straight off V4L2 (inside alpacad only) */
} guide_source_kind_t;

#define GUIDE_SIM_MAX_STARS 12

/* Half-width of the patch saved around each star. Must exceed the star's own
 * drawn radius (8). The patch is saved wherever the star is drawn each frame,
 * so it does not need to cover the motion between frames -- calibration moves
 * stars 25 px and more, which a fixed patch at the start position could not. */
#define GUIDE_SIM_PATCH_R 10
#define GUIDE_SIM_PATCH_W (2 * GUIDE_SIM_PATCH_R + 1)

typedef struct {
	double x, y;   /* true position in the base frame, before any motion */
	double peak;
	/* Pristine copy of the pixels this star was last drawn over, and where.
	 * Keeping these rather than a whole second frame is what lets the
	 * simulated source run on the board beside alpacad: 12 stars cost ~10 kB,
	 * a second frame would cost 6 MB and the two together once got alpacad
	 * OOM-killed. */
	uint16_t patch[GUIDE_SIM_PATCH_W * GUIDE_SIM_PATCH_W];
	int patch_x, patch_y, has_patch;
} guide_sim_star_t;

/*
 * The simulated mount: what the sky does, and what a guide pulse does to it.
 * Stars move in mount axes -- RA from periodic error and pulses, Dec from
 * polar-alignment drift and pulses through a backlash deadband -- and those
 * axes are then rotated (and optionally mirrored) into the camera frame. The
 * guider is told none of this; calibration has to discover the angle, the
 * rates and the mirror on its own, and the test checks what it found.
 */
typedef struct {
	double angle_deg;       /* direction a WEST pulse moves stars, camera frame */
	int dec_flip;           /* Dec axis at angle-90 instead of angle+90 */
	double ra_rate;         /* px per ms of pulse */
	double dec_rate;
	double dec_backlash_ms; /* pulse time eaten on every Dec reversal */
	double pe_amplitude;    /* RA periodic error, px */
	double pe_period;       /* frames */
	double dec_drift;       /* px per frame */
	double seeing;          /* px rms, per frame, both axes */
	unsigned int seed;
} guide_sim_mount_cfg_t;

struct guide_source;
typedef int (*guide_source_next_fn)(struct guide_source *);

typedef struct guide_source {
	guide_source_kind_t kind;
	guide_source_next_fn next; /* set by whichever open() was used */

	int width, height;
	uint16_t max_adu;
	uint16_t *pixels;      /* owned; x-outer, like alpacad's frames */
	guide_image_t image;   /* view onto pixels, ready to pass to guide_star_* */

	/* camera */
	char host[64];
	int port;
	double exposure_s;

	/* sim */
	guide_sim_star_t stars[GUIDE_SIM_MAX_STARS];
	int nstars;
	long frame;
	double drift_x, drift_y; /* true offset applied to the last frame produced */
	guide_sim_mount_cfg_t sim;
	double sim_ra, sim_dec;  /* where pulses have put the mount, px, mount axes */
	double sim_dec_slack;    /* backlash still to take up in the current direction */
	guide_dir_t sim_dec_last;
	long sim_pulses;

	/* v4l2 (in-daemon only) */
	const void *desc;      /* const sensor_desc_t *, opaque here */
	double exposure_rows;

	char err[256];
} guide_source_t;

/*
 * Opens a camera source. base_path may be NULL; if given, one frame is fetched
 * at open time and cached, which is what makes a later switch to the simulated
 * source able to use this sensor's real noise.
 */
int guide_source_open_camera(guide_source_t *src, const char *host, int port,
                             double exposure_s);

/*
 * Opens a simulated source. base_path is a raw uint16 frame from this sensor
 * (see grab_frame.py) used as the noise floor; pass NULL for a flat synthetic
 * pedestal, which is less honest and mainly useful when no frame is at hand.
 */
int guide_source_open_sim(guide_source_t *src, const char *base_path,
                          int width, int height, int nstars);

/* The simulated mount's defaults: a 30 degree camera rotation, 0.0025 px/ms
 * (a 750 ms calibration step moves 1.9 px), 300 ms of Dec backlash, 1.6 px of
 * RA periodic error over 35 frames, slow Dec drift and 0.1 px seeing. */
void guide_sim_mount_defaults(guide_sim_mount_cfg_t *cfg);

/* Replaces the simulated mount's behaviour and resets it to the start. */
void guide_source_sim_configure(guide_source_t *src, const guide_sim_mount_cfg_t *cfg);

/*
 * Binds a mount backend to a simulated source: pulses sent to `m` move this
 * source's stars on the next frame. The source must outlive the binding.
 */
void guide_source_sim_mount(guide_source_t *src, guide_mount_t *m);

/*
 * Opens the in-daemon V4L2 source. Only available inside alpacad, which already
 * owns the streaming device -- this shares that capture path under
 * v4l2_exposure_lock() rather than opening a second one, because there is only
 * one. desc is the sensor_desc_t alpacad detected.
 *
 * This is the source the real guide loop uses. GUIDE_SRC_CAMERA exists for
 * host-side testing against a running daemon and cannot be used from inside it.
 */
int guide_source_open_v4l2(guide_source_t *src, const void *desc,
                           double exposure_s);

/* Changes the exposure of a V4L2 or camera source, in seconds. */
void guide_source_set_exposure(guide_source_t *src, double exposure_s);

/* Produces the next frame into src->image. Returns 0 on success. */
int guide_source_next(guide_source_t *src);

/*
 * Releases the frame buffer without closing the source, for a caller that has
 * finished with the pixels and will ask for another frame later.
 *
 * Worth doing rather than holding the buffer: a frame is ~6 MB, and on the
 * RV1103 that is most of the headroom left once alpacad's own retained frame
 * and the V4L2 mmap pool are accounted for. Holding one between iterations for
 * no reason is what made the guide loop and an Alpaca client unable to run at
 * the same time. Sources that regenerate their pixels each frame (the simulated
 * one keeps a base image) ignore this.
 */
void guide_source_release(guide_source_t *src);

/*
 * True offset of the frame just produced, relative to the star positions the
 * source was created with. Only meaningful for GUIDE_SRC_SIM; returns 0 for a
 * camera source, where no such truth exists.
 */
void guide_source_truth(const guide_source_t *src, double *dx, double *dy);

const char *guide_source_name(const guide_source_t *src);
void guide_source_close(guide_source_t *src);

#endif
