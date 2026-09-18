#ifndef GUIDE_STAR_H
#define GUIDE_STAR_H

#include <stddef.h>
#include <stdint.h>

/*
 * Star detection and centroid tracking, ported from PHD2 (BSD 3-clause,
 * Copyright (c) 2013-2019 Open PHD Guiding development team). See
 * Guiding/README.md for why this subset and not the application.
 *
 * Two operations with very different duty cycles:
 *
 *   guide_star_autofind()  once, to pick a guide star out of a whole frame.
 *                          Expensive; runs a PSF-matched convolution over the
 *                          image in bands so it never holds two float images
 *                          (PHD2's version allocates ~36 MB at our frame size
 *                          and is OOM-killed on this board -- measured).
 *   guide_star_find()      every frame, to re-centroid the known star inside a
 *                          small search region. ~0.1 ms for 12 stars.
 *
 * The image is addressed through explicit strides rather than assumed to be
 * row-major, because alpacad stores frames in ImageBytes wire order
 * (out[x * height + y], X outer) and transposing 6 MB per frame to satisfy a
 * convention would cost more than the guiding itself.
 */

typedef struct {
	const uint16_t *px;
	int width;
	int height;
	/* Element step for +1 in x and +1 in y. Row-major is {1, width};
	 * alpacad's wire order is {height, 1}. */
	int x_stride;
	int y_stride;
	uint16_t max_adu; /* saturation level, 1023 for this 10-bit sensor */
} guide_image_t;

static inline uint16_t guide_px(const guide_image_t *im, int x, int y) {
	return im->px[(size_t)x * im->x_stride + (size_t)y * im->y_stride];
}

/* Mirrors PHD2's Star::FindResult. */
typedef enum {
	GUIDE_STAR_OK = 0,
	GUIDE_STAR_SATURATED,
	GUIDE_STAR_LOWSNR,
	GUIDE_STAR_LOWMASS,
	GUIDE_STAR_LOWHFD,
	GUIDE_STAR_HIHFD,
	GUIDE_STAR_TOO_NEAR_EDGE,
	GUIDE_STAR_ERROR,
} guide_star_result_t;

typedef struct {
	double x, y;   /* centroid, image pixels */
	double mass;   /* background-subtracted flux */
	double snr;
	double hfd;    /* half-flux diameter, pixels */
	uint16_t peak; /* raw peak value */
	guide_star_result_t result;
} guide_star_t;

const char *guide_star_result_str(guide_star_result_t r);

/*
 * Re-centroid a star near (base_x, base_y). PHD2's Star::Find in
 * FIND_CENTROID mode: smoothed peak search, sigma-clipped background estimate
 * in an annulus, threshold at mean + 3 sigma, then a first-moment centroid
 * over the aperture.
 *
 * search_region is PHD2's half-width; 15 (a 31x31 window) is its default.
 * min_hfd/max_hfd reject a hot pixel and a defocused blob respectively; PHD2
 * defaults to 1.5 and 25.0. Returns star->result, also stored in the struct.
 */
guide_star_result_t guide_star_find(const guide_image_t *im, int search_region,
                                    int base_x, int base_y,
                                    double min_hfd, double max_hfd,
                                    guide_star_t *star);

/*
 * Pick guide stars out of a whole frame. Writes up to max_stars entries to
 * out[], brightest first, and returns how many were written.
 *
 * downsample of 2 or 4 cuts the convolution cost roughly with the area and is
 * the right default here -- PHD2's own heuristic picks 1 at our image scale,
 * which is backwards for a memory-constrained device. band_rows trades peak
 * memory against loop overhead; 128 is a good default (2.5 MB of float at full
 * resolution).
 *
 * scratch must be at least guide_star_autofind_scratch(width, height,
 * downsample, band_rows) bytes, so this allocates nothing itself.
 */
size_t guide_star_autofind_scratch(int width, int height, int downsample, int band_rows);

int guide_star_autofind(const guide_image_t *im, int search_region,
                        int downsample, int band_rows,
                        void *scratch, size_t scratch_len,
                        guide_star_t *out, int max_stars);

#endif
