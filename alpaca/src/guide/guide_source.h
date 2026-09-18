#ifndef GUIDE_SOURCE_H
#define GUIDE_SOURCE_H

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
 * radius (8) plus the largest drift the simulator applies (~2), so restoring
 * the patch fully erases the previous frame's star. */
#define GUIDE_SIM_PATCH_R 16
#define GUIDE_SIM_PATCH_W (2 * GUIDE_SIM_PATCH_R + 1)

typedef struct {
	double x, y;   /* true position in the base frame, before drift */
	double peak;
	/* Pristine copy of the pixels this star is drawn over. Keeping these
	 * rather than a whole second frame is what lets the simulated source run
	 * on the board beside alpacad: 12 stars cost ~25 kB, a second frame would
	 * cost 6 MB and the two together once got alpacad OOM-killed. */
	uint16_t patch[GUIDE_SIM_PATCH_W * GUIDE_SIM_PATCH_W];
} guide_sim_star_t;

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
