/*
 * psf_bench -- measures the CPU cost of PHD2's star-detection primitives on the
 * actual target, so the on-device guiding feasibility question is answered with
 * numbers instead of estimates.
 *
 * Three costs matter, and they have very different duty cycles:
 *
 *   once per session   Median3 + float conversion + psf_conv over the whole
 *                      frame.  This is PHD2's Star::AutoFind, which picks the
 *                      guide star(s).  Expensive, but run once.
 *   once per frame     a centroid over a 31x31 search region per tracked star
 *                      (PHD2 DEFAULT_SEARCH_REGION = 15, MAX_LIST_SIZE = 12).
 *                      This is the guiding inner loop.
 *   once per frame     RAW10 unpack, but only over the search regions rather
 *                      than the whole frame -- guiding never needs the rest.
 *
 * psf_conv is transcribed from PHD2's src/star.cpp (BSD 3-clause, Copyright (c)
 * 2013-2019 Open PHD Guiding development team) so the measurement reflects the
 * real operation count rather than an approximation of it.
 *
 * Build with the project toolchain; run on the board.  No arguments.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define WIDTH  2304
#define HEIGHT 1296
#define NPIX   ((size_t)WIDTH * HEIGHT)

#define SEARCH_REGION 15                       /* PHD2 DEFAULT_SEARCH_REGION */
#define WINDOW (2 * SEARCH_REGION + 1)         /* 31x31 */
#define MAX_STARS 12                           /* PHD2 MAX_LIST_SIZE */

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Fills with plausible guide-frame content: a low noisy background plus a few
 * Gaussian stars. The values matter only inasmuch as they must not be constant
 * -- a flat buffer would let the compiler and the branch predictor flatter the
 * result. */
static void synth_frame(unsigned short *img) {
	unsigned int seed = 12345;
	for (size_t i = 0; i < NPIX; i++) {
		seed = seed * 1103515245u + 12345u;
		img[i] = 90 + ((seed >> 16) & 0x1f);
	}
	for (int s = 0; s < 40; s++) {
		int cx = 100 + (s * 53) % (WIDTH - 200);
		int cy = 100 + (s * 97) % (HEIGHT - 200);
		double peak = 300 + (s % 7) * 90;
		for (int dy = -6; dy <= 6; dy++) {
			for (int dx = -6; dx <= 6; dx++) {
				double r2 = dx * dx + dy * dy;
				double v = peak * exp(-r2 / 4.0);
				size_t idx = (size_t)(cy + dy) * WIDTH + (cx + dx);
				double acc = img[idx] + v;
				img[idx] = acc > 1023 ? 1023 : (unsigned short)acc;
			}
		}
	}
}

/* 3x3 median, as PHD2 runs over the whole frame before AutoFind. This is a
 * straightforward transcription, not PHD2's own: PHD2's median9() is a partial
 * insertion network and is meaningfully faster, so treat this figure as an
 * upper bound on the median step rather than as PHD2's cost. */
static void median3(unsigned short *dst, const unsigned short *src) {
	for (int y = 1; y < HEIGHT - 1; y++) {
		for (int x = 1; x < WIDTH - 1; x++) {
			unsigned short a[9];
			int n = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
					a[n++] = src[(size_t)(y + dy) * WIDTH + x + dx];
			/* partial selection sort to the median -- what PHD2 does */
			for (int i = 0; i < 5; i++) {
				int m = i;
				for (int j = i + 1; j < 9; j++)
					if (a[j] < a[m]) m = j;
				unsigned short t = a[i]; a[i] = a[m]; a[m] = t;
			}
			dst[(size_t)y * WIDTH + x] = a[4];
		}
	}
}

/* Transcribed from PHD2 src/star.cpp psf_conv(). */
static void psf_conv_rows(float *dst, const float *src, int width, int height) {
	const double PSF[] = { 0.906, 0.584, 0.365, .117, .049, -0.05, -.064, -.074, -.094 };
	const int psf_size = 4;

	memset(dst, 0, (size_t)width * height * sizeof(float));

	for (int y = psf_size; y < height - psf_size; y++) {
		for (int x = psf_size; x < width - psf_size; x++) {
			float A, B1, B2, C1, C2, C3, D1, D2, D3;
#define PX(dx, dy) *(src + width * (y + (dy)) + x + (dx))
			A = PX(0, 0);
			B1 = PX(0, -1) + PX(0, 1) + PX(1, 0) + PX(-1, 0);
			B2 = PX(-1, -1) + PX(1, -1) + PX(-1, 1) + PX(1, 1);
			C1 = PX(0, -2) + PX(-2, 0) + PX(2, 0) + PX(0, 2);
			C2 = PX(-1, -2) + PX(1, -2) + PX(-2, -1) + PX(2, -1) + PX(-2, 1) + PX(2, 1) + PX(-1, 2) + PX(1, 2);
			C3 = PX(-2, -2) + PX(2, -2) + PX(-2, 2) + PX(2, 2);
			D1 = PX(0, -3) + PX(-3, 0) + PX(3, 0) + PX(0, 3);
			D2 = PX(-1, -3) + PX(1, -3) + PX(-3, -1) + PX(3, -1) + PX(-3, 1) + PX(3, 1) + PX(-1, 3) + PX(1, 3);
			D3 = PX(-4, -2) + PX(-3, -2) + PX(3, -2) + PX(4, -2) + PX(-4, -1) + PX(4, -1) + PX(-4, 0) + PX(4, 0) +
			     PX(-4, 1) + PX(4, 1) + PX(-4, 2) + PX(-3, 2) + PX(3, 2) + PX(4, 2);
#undef PX
			const float *uptr;
			int i;
			uptr = src + width * (y - 4) + (x - 4);
			for (i = 0; i < 9; i++) D3 += *uptr++;
			uptr = src + width * (y - 3) + (x - 4);
			for (i = 0; i < 3; i++) D3 += *uptr++;
			uptr += 3;
			for (i = 0; i < 3; i++) D3 += *uptr++;
			uptr = src + width * (y + 3) + (x - 4);
			for (i = 0; i < 3; i++) D3 += *uptr++;
			uptr += 3;
			for (i = 0; i < 3; i++) D3 += *uptr++;
			uptr = src + width * (y + 4) + (x - 4);
			for (i = 0; i < 9; i++) D3 += *uptr++;

			double mean = (A + B1 + B2 + C1 + C2 + C3 + D1 + D2 + D3) / 81.0;
			double fit = PSF[0] * (A - mean) + PSF[1] * (B1 - 4.0 * mean) + PSF[2] * (B2 - 4.0 * mean) +
			             PSF[3] * (C1 - 4.0 * mean) + PSF[4] * (C2 - 8.0 * mean) + PSF[5] * (C3 - 4.0 * mean) +
			             PSF[6] * (D1 - 4.0 * mean) + PSF[7] * (D2 - 8.0 * mean) + PSF[8] * (D3 - 44.0 * mean);
			dst[(size_t)width * y + x] = (float)fit;
		}
	}
}

/* The guiding inner loop: background-subtracted centre of mass over one search
 * region, the same shape as PHD2's Star::Find in FIND_CENTROID mode. */
static double centroid(const unsigned short *img, int cx, int cy, double *out_x, double *out_y) {
	unsigned long sum = 0;
	unsigned short lo = 65535;
	int x0 = cx - SEARCH_REGION, y0 = cy - SEARCH_REGION;

	for (int y = 0; y < WINDOW; y++)
		for (int x = 0; x < WINDOW; x++) {
			unsigned short v = img[(size_t)(y0 + y) * WIDTH + x0 + x];
			if (v < lo) lo = v;
		}
	double mx = 0, my = 0;
	for (int y = 0; y < WINDOW; y++)
		for (int x = 0; x < WINDOW; x++) {
			unsigned long v = img[(size_t)(y0 + y) * WIDTH + x0 + x] - lo;
			sum += v;
			mx += (double)v * x;
			my += (double)v * y;
		}
	if (sum == 0) return 0;
	*out_x = x0 + mx / sum;
	*out_y = y0 + my / sum;
	return (double)sum;
}

/* RAW10 'bits_lsb' unpack, restricted to the rows a search region spans. The
 * full-frame version of this is the ~88 ms alpacad already pays; the point here
 * is how little of it guiding actually needs. */
static void unpack_region(const unsigned char *packed, size_t stride, unsigned short *out,
                          int x0, int y0, int w, int h) {
	for (int y = 0; y < h; y++) {
		const unsigned char *row = packed + (size_t)(y0 + y) * stride;
		for (int x = 0; x < w; x++) {
			size_t bit = (size_t)(x0 + x) * 10;
			size_t byte = bit >> 3;
			int off = bit & 7;
			unsigned int acc = row[byte] | (row[byte + 1] << 8);
			out[y * w + x] = (acc >> off) & 0x3ff;
		}
	}
}


/* Full-frame RAW10 unpack, as alpacad already does per exposure. Measured here
 * because a live web preview needs the whole frame, where guiding does not. */
static void unpack_full(const unsigned char *packed, size_t stride, unsigned short *out) {
	for (int y = 0; y < HEIGHT; y++) {
		const unsigned char *row = packed + (size_t)y * stride;
		unsigned short *o = out + (size_t)y * WIDTH;
		for (int x = 0; x < WIDTH; x++) {
			size_t bit = (size_t)x * 10;
			size_t byte = bit >> 3;
			int off = bit & 7;
			unsigned int acc = row[byte] | (row[byte + 1] << 8);
			o[x] = (acc >> off) & 0x3ff;
		}
	}
}

/* Box-downsample 10-bit to an 8-bit grayscale preview -- the buffer a browser
 * can render directly via canvas putImageData, with no encoder involved. */
static void preview_8bit(unsigned char *dst, const unsigned short *src, int f) {
	int dw = WIDTH / f, dh = HEIGHT / f;
	int shift = 2; /* 10-bit -> 8-bit */
	for (int y = 0; y < dh; y++)
		for (int x = 0; x < dw; x++) {
			unsigned int acc = 0;
			for (int j = 0; j < f; j++)
				for (int i = 0; i < f; i++)
					acc += src[(size_t)(y * f + j) * WIDTH + x * f + i];
			acc /= (unsigned)(f * f);
			dst[(size_t)y * dw + x] = (unsigned char)(acc >> shift);
		}
}

/* Peak resident set, so the memory claim is measured rather than arithmetic. */
static long peak_rss_kb(void) {
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long kb = -1;
	if (!f) return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "VmHWM: %ld kB", &kb) == 1) break;
	fclose(f);
	return kb;
}

/* psf_conv over a horizontal band, so the full-resolution pass can run without
 * ever holding two whole float images. Bands overlap by psf_size (4) rows on
 * each side because the stencil reaches that far. */
static void psf_conv_banded(const unsigned short *src, int width, int height, int band_rows) {
	const int pad = 4;
	float *fsrc = malloc((size_t)width * (band_rows + 2 * pad) * sizeof(float));
	float *fdst = malloc((size_t)width * (band_rows + 2 * pad) * sizeof(float));
	if (!fsrc || !fdst) { fprintf(stderr, "band alloc failed\n"); exit(1); }

	for (int y0 = 0; y0 < height; y0 += band_rows) {
		int top = y0 - pad < 0 ? 0 : y0 - pad;
		int bot = y0 + band_rows + pad > height ? height : y0 + band_rows + pad;
		int rows = bot - top;
		for (int y = 0; y < rows; y++)
			for (int x = 0; x < width; x++)
				fsrc[(size_t)y * width + x] = src[(size_t)(top + y) * width + x];
		psf_conv_rows(fdst, fsrc, width, rows);
	}
	free(fsrc); free(fdst);
}

/* Box-average downsample in place into a smaller u16 buffer. */
static void downsample_u16(unsigned short *dst, const unsigned short *src, int f) {
	int dw = WIDTH / f, dh = HEIGHT / f;
	for (int y = 0; y < dh; y++)
		for (int x = 0; x < dw; x++) {
			unsigned int acc = 0;
			for (int j = 0; j < f; j++)
				for (int i = 0; i < f; i++)
					acc += src[(size_t)(y * f + j) * WIDTH + x * f + i];
			dst[(size_t)y * dw + x] = acc / (f * f);
		}
}

int main(int argc, char **argv) {
	int downsample = argc > 1 ? atoi(argv[1]) : 1;
	int band_rows = argc > 2 ? atoi(argv[2]) : 128;

	printf("psf_bench: %dx%d (%.2f Mpx), downsample=%d band_rows=%d\n\n",
	       WIDTH, HEIGHT, NPIX / 1e6, downsample, band_rows);

	unsigned short *img = malloc(NPIX * sizeof(unsigned short));
	unsigned short *med = malloc(NPIX * sizeof(unsigned short));
	if (!img || !med) { fprintf(stderr, "alloc of 2x %.1f MB failed\n", NPIX * 2 / 1e6); return 1; }
	synth_frame(img);

	printf("--- per frame (the guiding inner loop) ---\n");

	double t0 = now_s();
	int reps = 200;
	double cxo, cyo, mass = 0;
	for (int r = 0; r < reps; r++)
		for (int s = 0; s < MAX_STARS; s++) {
			int cx = 200 + (s * 137) % (WIDTH - 400);
			int cy = 200 + (s * 211) % (HEIGHT - 400);
			mass += centroid(img, cx, cy, &cxo, &cyo);
		}
	double t1 = now_s();
	printf("centroid  %2d stars x %dx%d : %8.3f ms/frame\n",
	       MAX_STARS, WINDOW, WINDOW, (t1 - t0) * 1000.0 / reps);

	size_t stride = (size_t)WIDTH * 10 / 8;
	unsigned char *packed = malloc(stride * HEIGHT);
	unsigned short *win = malloc((size_t)WINDOW * WINDOW * sizeof(unsigned short));
	if (!packed || !win) { fprintf(stderr, "alloc of packed frame failed\n"); return 1; }
	memset(packed, 0xa5, stride * HEIGHT);
	t0 = now_s();
	for (int r = 0; r < reps; r++)
		for (int s = 0; s < MAX_STARS; s++)
			unpack_region(packed, stride, win, 200 + s * 37, 200 + s * 53, WINDOW, WINDOW);
	t1 = now_s();
	printf("unpack    %2d windows only  : %8.3f ms/frame  (cf. ~88 ms full frame)\n",
	       MAX_STARS, (t1 - t0) * 1000.0 / reps);
	free(win);

	printf("\n--- live web preview (only while someone is watching) ---\n");
	{
		unsigned short *full = malloc(NPIX * sizeof(unsigned short));
		int f = 4, dw = WIDTH / f, dh = HEIGHT / f;
		unsigned char *prev = malloc((size_t)dw * dh);
		if (!full || !prev) { fprintf(stderr, "preview alloc failed\n"); return 1; }
		t0 = now_s();
		unpack_full(packed, stride, full);
		t1 = now_s();
		printf("unpack    full frame       : %8.1f ms\n", (t1 - t0) * 1000.0);
		t0 = now_s();
		preview_8bit(prev, full, f);
		t1 = now_s();
		printf("preview   %dx -> %dx%d 8bit : %8.1f ms  (%.0f kB on the wire)\n",
		       f, dw, dh, (t1 - t0) * 1000.0, (double)dw * dh / 1024.0);
		free(full); free(prev);
	}
	free(packed);

	printf("\n--- once per session (star selection) ---\n");

	t0 = now_s();
	median3(med, img);
	t1 = now_s();
	printf("median3   full frame       : %8.1f ms\n", (t1 - t0) * 1000.0);

	/* The u16 source is all AutoFind still needs; drop the original before
	 * allocating anything float-sized. */
	free(img);

	int dw = WIDTH / downsample, dh = HEIGHT / downsample;
	if (downsample > 1) {
		t0 = now_s();
		downsample_u16(med, med, downsample);
		t1 = now_s();
		printf("downsample %dx -> %dx%d    : %8.1f ms\n", downsample, dw, dh, (t1 - t0) * 1000.0);
	}
	if (band_rows > dh) band_rows = dh;
	t0 = now_s();
	psf_conv_banded(med, dw, dh, band_rows);
	t1 = now_s();
	printf("psf_conv  banded %3d rows  : %8.1f ms  (peak float %.1f MB)\n",
	       band_rows, (t1 - t0) * 1000.0,
	       (double)dw * (band_rows + 8) * 4 * 2 / 1e6);

	printf("\npeak RSS: %ld kB\n", peak_rss_kb());
	printf("(consumed %.0f)\n", mass);
	free(med);
	return 0;
}
