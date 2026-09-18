#include "guide_star.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Ported from PHD2 src/star.cpp (BSD 3-clause, Copyright (c) 2013-2019 Open
 * PHD Guiding development team). The numeric constants below -- annulus radii,
 * the 3-sigma threshold, the 9 clipping iterations, the 0.5 e-/ADU nominal
 * gain, the 0.1 detection threshold -- are PHD2's and are deliberately left
 * alone: they are what a decade of real guiding tuned.
 */

const char *guide_star_result_str(guide_star_result_t r) {
	switch (r) {
	case GUIDE_STAR_OK:            return "ok";
	case GUIDE_STAR_SATURATED:     return "saturated";
	case GUIDE_STAR_LOWSNR:        return "low SNR";
	case GUIDE_STAR_LOWMASS:       return "low mass";
	case GUIDE_STAR_LOWHFD:        return "HFD too small";
	case GUIDE_STAR_HIHFD:         return "HFD too large";
	case GUIDE_STAR_TOO_NEAR_EDGE: return "too near edge";
	default:                       return "error";
	}
}

/* ------------------------------------------------------------------ HFD -- */

/* One aperture pixel, for the half-flux computation. The aperture is a disc of
 * radius APERTURE so the count is bounded; no allocation is needed. */
#define ANNULUS_INNER 7
#define ANNULUS_OUTER 12
#define APERTURE      ANNULUS_INNER
#define MAX_APERTURE_PX ((2 * APERTURE + 1) * (2 * APERTURE + 1))

typedef struct {
	double r2;
	double m;
	int x, y;
} r2m_t;

static int r2m_cmp(const void *a, const void *b) {
	double ra = ((const r2m_t *)a)->r2, rb = ((const r2m_t *)b)->r2;
	return ra < rb ? -1 : (ra > rb ? 1 : 0);
}

/* Half-flux radius: sort aperture pixels by radius from the centroid, walk
 * outward until half the mass is enclosed, interpolate. PHD2's hfr(). */
static double hfr(r2m_t *vec, int n, double cx, double cy, double mass) {
	int i;
	double r20 = 0, r21 = 0, m0 = 0, m1 = 0, halfm;

	if (n == 1)
		return 0.25; /* single pixel over threshold -- a hot pixel */

	for (i = 0; i < n; i++) {
		double dx = (double)vec[i].x - cx;
		double dy = (double)vec[i].y - cy;
		vec[i].r2 = dx * dx + dy * dy;
	}
	qsort(vec, (size_t)n, sizeof(*vec), r2m_cmp);

	halfm = 0.5 * mass;
	for (i = 0; i < n; i++) {
		r20 = r21;
		m0 = m1;
		r21 = vec[i].r2;
		m1 += vec[i].m;
		if (m1 > halfm)
			break;
	}

	if (m1 > m0) {
		double r0 = sqrt(r20), r1 = sqrt(r21);
		double s = (r1 - r0) / (m1 - m0);
		return r0 + s * (halfm - m0);
	}
	return 0.25;
}

/* ----------------------------------------------------------------- find -- */

guide_star_result_t guide_star_find(const guide_image_t *im, int search_region,
                                    int base_x, int base_y,
                                    double min_hfd, double max_hfd,
                                    guide_star_t *star) {
	int minx = 0, miny = 0, maxx = im->width - 1, maxy = im->height - 1;
	int start_x, end_x, start_y, end_y;
	int peak_x = 0, peak_y = 0, x, y, iter;
	unsigned int peak_val = 0;
	uint16_t max3[3] = { 0, 0, 0 };
	unsigned int nbg = 0;
	double mean_bg = 0.0, prev_mean_bg = 0.0, sigma2_bg = 0.0, sigma_bg = 0.0;
	double cx = 0.0, cy = 0.0, mass = 0.0;
	unsigned int n = 0;
	uint16_t thresh;
	r2m_t hfrvec[MAX_APERTURE_PX];
	int nhfr = 0;
	const double gain = 0.5;    /* electrons per ADU, nominal (PHD2) */
	const double LOW_SNR = 3.0;
	const int A2 = ANNULUS_INNER * ANNULUS_INNER;
	const int B2 = ANNULUS_OUTER * ANNULUS_OUTER;

	memset(star, 0, sizeof(*star));
	star->x = base_x;
	star->y = base_y;

	start_x = base_x - search_region; if (start_x < minx) start_x = minx;
	end_x   = base_x + search_region; if (end_x   > maxx) end_x   = maxx;
	start_y = base_y - search_region; if (start_y < miny) start_y = miny;
	end_y   = base_y + search_region; if (end_y   > maxy) end_y   = maxy;

	if (end_x <= start_x || end_y <= start_y) {
		star->result = GUIDE_STAR_ERROR;
		return star->result;
	}

	/* Peak search over a 3x3 smoothing kernel (1 2 1 / 2 4 2 / 1 2 1), so a
	 * single hot pixel does not win against a real star. Also tracks the
	 * three largest raw values, for the saturation test. */
	for (y = start_y + 1; y <= end_y - 1; y++) {
		for (x = start_x + 1; x <= end_x - 1; x++) {
			uint16_t p = guide_px(im, x, y);
			unsigned int val =
			    4u * (unsigned int)p +
			    guide_px(im, x - 1, y - 1) + guide_px(im, x + 1, y - 1) +
			    guide_px(im, x - 1, y + 1) + guide_px(im, x + 1, y + 1) +
			    2u * guide_px(im, x, y - 1) + 2u * guide_px(im, x - 1, y) +
			    2u * guide_px(im, x + 1, y) + 2u * guide_px(im, x, y + 1);

			if (val > peak_val) {
				peak_val = val;
				peak_x = x;
				peak_y = y;
			}
			if (p > max3[0]) { uint16_t t = max3[0]; max3[0] = p; p = t; }
			if (p > max3[1]) { uint16_t t = max3[1]; max3[1] = p; p = t; }
			if (p > max3[2]) { uint16_t t = max3[2]; max3[2] = p; p = t; }
		}
	}
	star->peak = max3[0];
	peak_val /= 16;

	/* Background mean and sigma in an annulus around the peak, re-estimated
	 * with 2-sigma clipping so a neighbouring star in the annulus does not
	 * drag the background up. */
	start_x = peak_x - ANNULUS_OUTER; if (start_x < minx) start_x = minx;
	end_x   = peak_x + ANNULUS_OUTER; if (end_x   > maxx) end_x   = maxx;
	start_y = peak_y - ANNULUS_OUTER; if (start_y < miny) start_y = miny;
	end_y   = peak_y + ANNULUS_OUTER; if (end_y   > maxy) end_y   = maxy;

	for (iter = 0; iter < 9; iter++) {
		double sum = 0.0, a = 0.0, q = 0.0;
		nbg = 0;

		for (y = start_y; y <= end_y; y++) {
			int dy = y - peak_y, dy2 = dy * dy;
			for (x = start_x; x <= end_x; x++) {
				int dx = x - peak_x;
				int r2 = dx * dx + dy2;
				double val, a0, k;

				if (r2 <= A2 || r2 > B2)
					continue;
				val = (double)guide_px(im, x, y);
				if (iter > 0 && (val < mean_bg - 2.0 * sigma_bg ||
				                 val > mean_bg + 2.0 * sigma_bg))
					continue;

				sum += val;
				++nbg;
				k = (double)nbg;
				a0 = a;
				a += (val - a) / k;      /* Welford: running mean */
				q += (val - a0) * (val - a);
			}
		}

		if (nbg < 10)
			break; /* only reachable after the first iteration */

		prev_mean_bg = mean_bg;
		mean_bg = sum / (double)nbg;
		sigma2_bg = q / (double)(nbg - 1);
		sigma_bg = sqrt(sigma2_bg);

		if (iter > 0 && fabs(mean_bg - prev_mean_bg) < 0.5)
			break;
	}

	/* First-moment centroid over pixels above threshold inside the aperture. */
	thresh = (uint16_t)(mean_bg + 3.0 * sigma_bg + 0.5);

	start_x = peak_x - APERTURE; if (start_x < minx) start_x = minx;
	end_x   = peak_x + APERTURE; if (end_x   > maxx) end_x   = maxx;
	start_y = peak_y - APERTURE; if (start_y < miny) start_y = miny;
	end_y   = peak_y + APERTURE; if (end_y   > maxy) end_y   = maxy;

	for (y = start_y; y <= end_y; y++) {
		int dy = y - peak_y, dy2 = dy * dy;
		if (dy2 > A2)
			continue;
		for (x = start_x; x <= end_x; x++) {
			int dx = x - peak_x;
			uint16_t val;
			double d;

			if (dx * dx + dy2 > A2)
				continue;
			val = guide_px(im, x, y);
			if (val < thresh)
				continue;

			d = (double)val - mean_bg;
			cx += dx * d;
			cy += dy * d;
			mass += d;
			++n;

			if (nhfr < MAX_APERTURE_PX) {
				hfrvec[nhfr].x = x;
				hfrvec[nhfr].y = y;
				hfrvec[nhfr].m = d;
				nhfr++;
			}
		}
	}

	star->mass = mass;

	/* SNR per Simonetti (2004), as PHD2 uses it. */
	star->snr = n > 0 ? mass / sqrt(mass / gain +
	                                sigma2_bg * (double)n *
	                                    (1.0 + 1.0 / (double)(nbg ? nbg : 1)))
	                  : 0.0;

	/* A handful of scattered pixels over threshold can pass the SNR test;
	 * require the smoothed peak to clear the threshold too. */
	if (peak_val <= thresh && star->snr >= LOW_SNR)
		star->snr = LOW_SNR - 0.1;

	if (mass < 10.0) {
		star->result = GUIDE_STAR_LOWMASS;
		return star->result;
	}
	if (star->snr < LOW_SNR) {
		star->result = GUIDE_STAR_LOWSNR;
		return star->result;
	}

	star->x = peak_x + cx / mass;
	star->y = peak_y + cy / mass;
	star->hfd = 2.0 * hfr(hfrvec, nhfr, star->x, star->y, mass);

	if (star->hfd < min_hfd) {
		star->result = GUIDE_STAR_LOWHFD;
		return star->result;
	}
	if (star->hfd > max_hfd) {
		star->result = GUIDE_STAR_HIHFD;
		return star->result;
	}
	if (max3[0] >= im->max_adu && max3[2] >= im->max_adu) {
		/* Three pixels at full scale: the core is clipped and the
		 * centroid is no longer trustworthy. */
		star->result = GUIDE_STAR_SATURATED;
		return star->result;
	}

	star->result = GUIDE_STAR_OK;
	return star->result;
}

/* ------------------------------------------------------------- autofind -- */

/*
 * PHD2's AutoFind, restructured for this device.
 *
 * PHD2 median-filters the frame, converts it to float, downsamples, and runs a
 * PSF-matched convolution, holding two whole float images at once. At
 * 2304x1296 that peaks at ~35.8 MB and the kernel OOM-kills it here (measured;
 * see Guiding/README.md). The work is identical, but done in horizontal bands
 * that are gathered and downsampled straight out of the source image, so no
 * full-size intermediate exists at any point: peak scratch is two float bands.
 *
 * It is also a single pass. PHD2 computes the convolution's global mean and
 * stdev over the finished image, then rescans it for local maxima scoring
 * h = (val - local_mean) / global_stdev. Since global_stdev is one scalar
 * applied uniformly, it cannot change the *ranking* of candidates -- so the
 * top-N list is collected by (val - local_mean) as the bands are produced and
 * the threshold is applied once at the end. Same result, one pass.
 */

#define CONV_RADIUS 4                     /* psf_conv stencil reach */
#define LOCAL_R     7                     /* local-mean window half-width */
#define MAXIMA_R    4                     /* local-maximum neighbourhood */
#define BAND_PAD    (CONV_RADIUS + LOCAL_R)
#define TOP_N       100
#define MERGE_DIST2 25                    /* merge peaks closer than 5 px */

typedef struct {
	int x, y;      /* coordinates in the original image */
	double h;      /* val - local_mean; scaled by global stdev at the end */
} peak_t;

size_t guide_star_autofind_scratch(int width, int height, int downsample, int band_rows) {
	int dw = width / downsample;
	int rows = band_rows + 2 * BAND_PAD;
	(void)height;
	if (dw < 1) dw = 1;
	return (size_t)dw * rows * sizeof(float) * 2;
}

/* psf_conv from PHD2 src/star.cpp, restricted to a band. Writes dst rows
 * [CONV_RADIUS, rows - CONV_RADIUS). */
static void psf_conv_band(float *dst, const float *src, int width, int rows) {
	static const double PSF[] = { 0.906, 0.584, 0.365, .117, .049,
	                              -0.05, -.064, -.074, -.094 };
	int x, y, i;

	memset(dst, 0, (size_t)width * rows * sizeof(float));

	for (y = CONV_RADIUS; y < rows - CONV_RADIUS; y++) {
		for (x = CONV_RADIUS; x < width - CONV_RADIUS; x++) {
			float A, B1, B2, C1, C2, C3, D1, D2, D3;
			const float *uptr;
			double mean, fit;

#define PX(dx, dy) *(src + (size_t)width * (y + (dy)) + x + (dx))
			A = PX(0, 0);
			B1 = PX(0, -1) + PX(0, 1) + PX(1, 0) + PX(-1, 0);
			B2 = PX(-1, -1) + PX(1, -1) + PX(-1, 1) + PX(1, 1);
			C1 = PX(0, -2) + PX(-2, 0) + PX(2, 0) + PX(0, 2);
			C2 = PX(-1, -2) + PX(1, -2) + PX(-2, -1) + PX(2, -1) +
			     PX(-2, 1) + PX(2, 1) + PX(-1, 2) + PX(1, 2);
			C3 = PX(-2, -2) + PX(2, -2) + PX(-2, 2) + PX(2, 2);
			D1 = PX(0, -3) + PX(-3, 0) + PX(3, 0) + PX(0, 3);
			D2 = PX(-1, -3) + PX(1, -3) + PX(-3, -1) + PX(3, -1) +
			     PX(-3, 1) + PX(3, 1) + PX(-1, 3) + PX(1, 3);
			D3 = PX(-4, -2) + PX(-3, -2) + PX(3, -2) + PX(4, -2) +
			     PX(-4, -1) + PX(4, -1) + PX(-4, 0) + PX(4, 0) +
			     PX(-4, 1) + PX(4, 1) + PX(-4, 2) + PX(-3, 2) +
			     PX(3, 2) + PX(4, 2);
#undef PX
			uptr = src + (size_t)width * (y - 4) + (x - 4);
			for (i = 0; i < 9; i++) D3 += *uptr++;
			uptr = src + (size_t)width * (y - 3) + (x - 4);
			for (i = 0; i < 3; i++) D3 += *uptr++;
			uptr += 3;
			for (i = 0; i < 3; i++) D3 += *uptr++;
			uptr = src + (size_t)width * (y + 3) + (x - 4);
			for (i = 0; i < 3; i++) D3 += *uptr++;
			uptr += 3;
			for (i = 0; i < 3; i++) D3 += *uptr++;
			uptr = src + (size_t)width * (y + 4) + (x - 4);
			for (i = 0; i < 9; i++) D3 += *uptr++;

			mean = (A + B1 + B2 + C1 + C2 + C3 + D1 + D2 + D3) / 81.0;
			fit = PSF[0] * (A - mean) + PSF[1] * (B1 - 4.0 * mean) +
			      PSF[2] * (B2 - 4.0 * mean) + PSF[3] * (C1 - 4.0 * mean) +
			      PSF[4] * (C2 - 8.0 * mean) + PSF[5] * (C3 - 4.0 * mean) +
			      PSF[6] * (D1 - 4.0 * mean) + PSF[7] * (D2 - 8.0 * mean) +
			      PSF[8] * (D3 - 44.0 * mean);
			dst[(size_t)width * y + x] = (float)fit;
		}
	}
}

/* Box-downsample rows [sy0, sy0+rows) of the source into a float band, with a
 * 3-pixel median along x to knock out hot pixels -- PHD2 runs a full 3x3
 * Median3 over the frame first, which at our size is a 755 ms pass over 6 MB
 * purely to protect the convolution. Downsampling already averages f*f pixels;
 * the median across the three horizontal neighbours catches the isolated hot
 * pixel that averaging alone would smear into a false peak. */
static void gather_band(float *dst, const guide_image_t *im,
                        int dw, int sy0, int rows, int f) {
	int y, x, j, i;

	for (y = 0; y < rows; y++) {
		int syb = (sy0 + y) * f;
		for (x = 0; x < dw; x++) {
			unsigned int acc = 0;
			int sxb = x * f;
			if (syb < 0 || syb + f > im->height || sxb + f > im->width) {
				dst[(size_t)y * dw + x] = 0.0f;
				continue;
			}
			for (j = 0; j < f; j++) {
				for (i = 0; i < f; i++) {
					int sx = sxb + i;
					uint16_t a, b, c, m;
					if (sx < 1 || sx >= im->width - 1) {
						acc += guide_px(im, sx, syb + j);
						continue;
					}
					a = guide_px(im, sx - 1, syb + j);
					b = guide_px(im, sx,     syb + j);
					c = guide_px(im, sx + 1, syb + j);
					/* median of three */
					m = a > b ? (b > c ? b : (a > c ? c : a))
					          : (a > c ? a : (b > c ? c : b));
					acc += m;
				}
			}
			dst[(size_t)y * dw + x] = (float)acc / (float)(f * f);
		}
	}
}

/* Insert into a descending top-N list kept by h. */
static int peak_insert(peak_t *list, int n, const peak_t *p) {
	int i;
	if (n == TOP_N && p->h <= list[n - 1].h)
		return n;
	i = n < TOP_N ? n : TOP_N - 1;
	for (; i > 0 && list[i - 1].h < p->h; i--)
		list[i] = list[i - 1];
	list[i] = *p;
	return n < TOP_N ? n + 1 : TOP_N;
}

int guide_star_autofind(const guide_image_t *im, int search_region,
                        int downsample, int band_rows,
                        void *scratch, size_t scratch_len,
                        guide_star_t *out, int max_stars) {
	int dw, dh, f = downsample;
	int rows_total, y0, nfound = 0, npk = 0, i, k;
	float *fsrc, *fdst;
	peak_t peaks[TOP_N];
	double gsum = 0.0, gq = 0.0, ga = 0.0, gstdev;
	unsigned long gn = 0;
	size_t need;

	if (f < 1) f = 1;
	if (band_rows < 1) band_rows = 1;
	dw = im->width / f;
	dh = im->height / f;
	rows_total = band_rows + 2 * BAND_PAD;

	need = guide_star_autofind_scratch(im->width, im->height, f, band_rows);
	if (scratch_len < need || dw <= 2 * BAND_PAD)
		return 0;

	fsrc = (float *)scratch;
	fdst = fsrc + (size_t)dw * rows_total;

	for (y0 = 0; y0 < dh; y0 += band_rows) {
		int top = y0 - BAND_PAD;
		int rows = rows_total;
		int y, x;

		if (top + rows > dh + BAND_PAD)
			rows = dh + BAND_PAD - top;
		if (rows <= 2 * CONV_RADIUS)
			break;

		gather_band(fsrc, im, dw, top, rows, f);
		psf_conv_band(fdst, fsrc, dw, rows);

		/* Detect only in the interior of the band; the pad rows exist so
		 * every detected pixel has a full local window available. */
		for (y = BAND_PAD; y < rows - BAND_PAD; y++) {
			int gy = top + y;
			if (gy < BAND_PAD || gy >= dh - BAND_PAD)
				continue;
			for (x = BAND_PAD; x < dw - BAND_PAD; x++) {
				float val = fdst[(size_t)y * dw + x];
				int j, ii, ismax;
				double lsum = 0.0, lmean, d;
				peak_t p;

				/* global stats, accumulated as bands are produced */
				gn++;
				d = (double)val - ga;
				ga += d / (double)gn;
				gq += d * ((double)val - ga);
				gsum += val;

				if (val <= 0.0f)
					continue;
				ismax = 1;
				for (j = -MAXIMA_R; j <= MAXIMA_R && ismax; j++)
					for (ii = -MAXIMA_R; ii <= MAXIMA_R; ii++) {
						if (!ii && !j)
							continue;
						if (fdst[(size_t)(y + j) * dw + (x + ii)] > val) {
							ismax = 0;
							break;
						}
					}
				if (!ismax)
					continue;

				for (j = -LOCAL_R; j <= LOCAL_R; j++)
					for (ii = -LOCAL_R; ii <= LOCAL_R; ii++)
						lsum += fdst[(size_t)(y + j) * dw + (x + ii)];
				lmean = lsum / (double)((2 * LOCAL_R + 1) * (2 * LOCAL_R + 1));

				p.x = x * f + f / 2;
				p.y = gy * f + f / 2;
				p.h = (double)val - lmean;
				npk = peak_insert(peaks, npk, &p);
			}
		}
	}

	if (gn < 2)
		return 0;
	gstdev = sqrt(gq / (double)(gn - 1));
	if (gstdev <= 0.0)
		return 0;

	/* Merge candidates that are within a few pixels of each other -- one star
	 * can produce two adjacent local maxima. Keeps the brighter. */
	for (i = 0; i < npk; i++) {
		for (k = i + 1; k < npk; ) {
			int dx = peaks[i].x - peaks[k].x;
			int dy = peaks[i].y - peaks[k].y;
			if (dx * dx + dy * dy < MERGE_DIST2 * f * f) {
				memmove(&peaks[k], &peaks[k + 1],
				        (size_t)(npk - k - 1) * sizeof(peaks[0]));
				npk--;
			} else {
				k++;
			}
		}
	}

	/* Validate each candidate with the same centroid routine that will track
	 * it every frame, so a star that cannot be tracked is never selected.
	 *
	 * The merge above works on raw peak positions, which is not enough:
	 * guide_star_find() searches +/- search_region for its own peak, so two
	 * candidates up to 2 * search_region apart can re-centroid onto the same
	 * star and be reported twice. Dedupe on the validated centroid as well,
	 * at the same distance the peak merge uses. */
	for (i = 0; i < npk && nfound < max_stars; i++) {
		guide_star_t s;
		int edge = search_region + ANNULUS_OUTER;
		int dup = 0;

		if (peaks[i].h / gstdev < 0.1)
			continue;
		if (peaks[i].x < edge || peaks[i].x >= im->width - edge ||
		    peaks[i].y < edge || peaks[i].y >= im->height - edge)
			continue;
		if (guide_star_find(im, search_region, peaks[i].x, peaks[i].y,
		                    1.5, 25.0, &s) != GUIDE_STAR_OK)
			continue;

		for (k = 0; k < nfound; k++) {
			double ddx = out[k].x - s.x, ddy = out[k].y - s.y;
			if (ddx * ddx + ddy * ddy < (double)MERGE_DIST2) {
				dup = 1;
				break;
			}
		}
		if (dup)
			continue;

		out[nfound++] = s;
	}
	return nfound;
}
