/*
 * Hinted plate solving. Port of ASTAP's solver core.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Ported from ASTAP, Copyright (C) 2017-2026 by Han Kleijn, www.hnsky.org,
 * https://sourceforge.net/p/astap-program/ (hg changeset 4ea2c2f5):
 *   unit_star_align.pas         lsq_fit, QuickSort_starlist, find_many_quads,
 *                               find_quads, find_fit, find_fit_using_hash,
 *                               find_offset_and_rotation
 *   unit_astrometric_solving.pas  position_angle, equatorial_standard,
 *                               standard_equatorial, read_stars, solve_image
 *   astap_main.pas              fnmodulo, ang_sep, SMedian
 * Constants and control flow are kept as ASTAP has them; Pascal's Round
 * (banker's rounding) is rint() here.
 *
 * Deviations, each because the Pascal leans on something outside the solver:
 * - solve_image's star detection, binning, cropping, autoFOV loop, SIP fit,
 *   FITS header and GUI reporting are not ported. The caller supplies stars.
 * - The 2025 second-pass filter ("keep only stars visible in the image
 *   according the first solution") projects catalogue stars into the image
 *   with ASTAP's rotate() and image-axis conventions. Those conventions are
 *   ASTAP's own find_stars output; ours come from PHD2's star finder. Here
 *   the same filter is done by inverting the first pass's own fit, which is
 *   exact for any pixel convention.
 * - Hash buckets are linked lists and hash bins a counting sort instead of
 *   resizable Pascal arrays. Bucket membership is identical; only the order
 *   of equivalent matches differs, which changes nothing but float rounding.
 */
#include "astap_solve.h"
#include "astap_db.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* quad_star_distances[0..7, i] in the Pascal: d1, d2/d1 .. d6/d1, xt, yt */
typedef struct { double v[8]; } astap_quad_t;

typedef struct {
	astap_quad_t *q;
	int n, cap;
} quadlist_t;

typedef struct {
	double *x, *y;     /* standard coordinates (arcsec) or pixels */
	double *ra, *dec;  /* catalogue stars only, for the second-pass filter */
	int n, cap;
} starlist_t;

typedef struct {
	quadlist_t quads1;   /* database, quad_star_distances1 */
	quadlist_t quads2;   /* image,    quad_star_distances2 */
	int nr_references, nr_references2;
	double sx[3], sy[3]; /* solution_vectorX/Y */
	/* scratch for find_fit */
	int *match;          /* pairs i,j */
	int match_cap;
	double *A[3], *bx, *by;
	int A_cap;
} ctx_t;

/* ---------------------------------------------------------- helpers */

static double fnmodulo(double x, double range) {
	double r;
	if (x >= 0 && x < range) /* avoid division */
		return x;
	r = x - range * trunc(x / range);
	if (r < 0) r += range;
	return r;
}

double astap_ang_sep(double ra1, double dec1, double ra2, double dec2) {
	double c = sin(dec1) * sin(dec2) + cos(dec1) * cos(dec2) * cos(ra1 - ra2);
	if (c > 1.0) c = 1.0;
	if (c < -1.0) c = -1.0;
	return acos(c);
}

static int cmp_double(const void *a, const void *b) {
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

/* SMedian: sorts list in place. Odd lengths above 3 average the middle three. */
static double smedian(double *list, int leng) {
	int mid;
	if (leng == 0) return NAN;
	if (leng == 1) return list[0];
	qsort(list, leng, sizeof(double), cmp_double);
	mid = (leng - 1) / 2;
	if (leng & 1) {
		if (leng <= 3) return list[mid];
		return (list[mid - 1] + list[mid] + list[mid + 1]) / 3;
	}
	return (list[mid] + list[mid + 1]) / 2;
}

/* Position angle of ra1/dec1 seen from ra0/dec0, north through east (Meeus 46.5). */
static double position_angle(double ra1, double dec1, double ra0, double dec0) {
	double sdr = sin(ra1 - ra0), cdr = cos(ra1 - ra0);
	return atan2(cos(dec1) * sdr, sin(dec1) * cos(dec0) - cos(dec1) * sin(dec0) * cdr);
}

/* Equatorial -> standard coordinates around ra0/dec0, in units of cdelt arcsec. */
static void equatorial_standard(double ra0, double dec0, double ra, double dec, double cdelt,
                                double *xx, double *yy) {
	double sin_dec0 = sin(dec0), cos_dec0 = cos(dec0);
	double sin_dec = sin(dec), cos_dec = cos(dec);
	double sin_dra = sin(ra - ra0), cos_dra = cos(ra - ra0);
	double dv = (cos_dec0 * cos_dec * cos_dra + sin_dec0 * sin_dec) * cdelt / (3600 * 180 / M_PI);
	*xx = -cos_dec * sin_dra / dv;
	*yy = -(sin_dec0 * cos_dec * cos_dra - cos_dec0 * sin_dec) / dv;
}

static void standard_equatorial(double ra0, double dec0, double x, double y, double cdelt,
                                double *ra, double *dec) {
	double sin_dec0 = sin(dec0), cos_dec0 = cos(dec0), delta;
	x = x * cdelt / (3600 * 180 / M_PI);
	y = y * cdelt / (3600 * 180 / M_PI);
	delta = cos_dec0 - y * sin_dec0;
	*ra = ra0 + atan2(-x, delta); /* atan2 is required for images containing the pole */
	*dec = atan((sin_dec0 + y * cos_dec0) / sqrt(x * x + delta * delta));
	if (*ra > M_PI * 2) *ra -= M_PI * 2;
	if (*ra < 0) *ra += M_PI * 2;
}

static int quads_reserve(quadlist_t *l, int n) {
	if (n > l->cap) {
		astap_quad_t *p = realloc(l->q, (size_t)n * sizeof(astap_quad_t));
		if (!p) return -1;
		l->q = p;
		l->cap = n;
	}
	return 0;
}

static int stars_reserve(starlist_t *s, int n) {
	if (n > s->cap) {
		double *x = realloc(s->x, (size_t)n * sizeof(double)); if (!x) return -1; s->x = x;
		x = realloc(s->y, (size_t)n * sizeof(double)); if (!x) return -1; s->y = x;
		x = realloc(s->ra, (size_t)n * sizeof(double)); if (!x) return -1; s->ra = x;
		x = realloc(s->dec, (size_t)n * sizeof(double)); if (!x) return -1; s->dec = x;
		s->cap = n;
	}
	return 0;
}

static void stars_free(starlist_t *s) {
	free(s->x); free(s->y); free(s->ra); free(s->dec);
	memset(s, 0, sizeof(*s));
}

/* ---------------------------------------------------------- unit_star_align */

/*
 * lsq_fit: least-squares solution of A x = b by Givens rotations
 * (Montenbruck & Pfleger). A is 3 columns of n equations. A and b are
 * copied, as the Pascal does by value.
 */
static int lsq_fit(double *const A[3], const double *b_in, int n, double x[3]) {
	const double tiny = 1E-10;
	double *t[3], *b, p, q, h;
	int i, j, k, ok = 1;

	t[0] = malloc((size_t)n * 4 * sizeof(double));
	if (!t[0]) return 0;
	t[1] = t[0] + n;
	t[2] = t[1] + n;
	b = t[2] + n;
	for (j = 0; j < 3; j++) memcpy(t[j], A[j], (size_t)n * sizeof(double));
	memcpy(b, b_in, (size_t)n * sizeof(double));

	for (j = 0; j < 3; j++)
		for (i = j + 1; i < n; i++)
			if (t[j][i] != 0) {
				if (fabs(t[j][j]) < tiny * fabs(t[j][i])) {
					p = 0;
					q = 1;
					t[j][j] = -t[j][i];
					t[j][i] = 0;
				} else {
					h = sqrt(t[j][j] * t[j][j] + t[j][i] * t[j][i]);
					if (t[j][j] < 0) h = -h;
					p = t[j][j] / h;
					q = -t[j][i] / h;
					t[j][j] = h;
					t[j][i] = 0;
				}
				for (k = j + 1; k < 3; k++) {
					h = p * t[k][j] - q * t[k][i];
					t[k][i] = q * t[k][j] + p * t[k][i];
					t[k][j] = h;
				}
				h = p * b[j] - q * b[i];
				b[i] = q * b[j] + p * b[i];
				b[j] = h;
			}

	for (i = 0; i < 3; i++) x[i] = 0;
	for (i = 2; i >= 0; i--) { /* back substitution */
		h = b[i];
		for (k = i + 1; k < 3; k++) h -= t[k][i] * x[k];
		if (fabs(t[i][i]) > 1E-30) {
			x[i] = h / t[i][i];
		} else {
			ok = 0; /* prevent a division by zero, forces a failure */
			break;
		}
	}
	free(t[0]);
	return ok;
}

/* QuickSort_starlist: sort on x only. */
static void quicksort_starlist(double *X, double *Y, int iLo, int iHi) {
	int Lo = iLo, Hi = iHi;
	double pivot = X[(Lo + Hi) / 2], tx, ty;
	do {
		while (X[Lo] < pivot) Lo++;
		while (X[Hi] > pivot) Hi--;
		if (Lo <= Hi) {
			tx = X[Lo]; ty = Y[Lo];
			X[Lo] = X[Hi]; Y[Lo] = Y[Hi];
			X[Hi] = tx; Y[Hi] = ty;
			Lo++;
			Hi--;
		}
	} while (Lo <= Hi);
	if (Hi > iLo) quicksort_starlist(X, Y, iLo, Hi);
	if (Lo < iHi) quicksort_starlist(X, Y, Lo, iHi);
}

/* The six pairwise distances of four stars, sorted longest first, stored as
 * d1, d2/d1 .. d6/d1 plus the centre. The bubble sort is ASTAP's. */
static void store_quad(astap_quad_t *q, double dist1, double dist2, double dist3, double dist4,
                       double dist5, double dist6, double xt, double yt) {
	double temp;
#define SW(a, b) if (b > a) { temp = a; a = b; b = temp; }
	SW(dist1, dist2) SW(dist2, dist3) SW(dist3, dist4) SW(dist4, dist5) SW(dist5, dist6)
	SW(dist1, dist2) SW(dist2, dist3) SW(dist3, dist4) SW(dist4, dist5)
	SW(dist1, dist2) SW(dist2, dist3) SW(dist3, dist4)
	SW(dist1, dist2) SW(dist2, dist3)
	SW(dist1, dist2)
#undef SW
	q->v[0] = dist1;
	q->v[1] = dist2 / dist1;
	q->v[2] = dist3 / dist1;
	q->v[3] = dist4 / dist1;
	q->v[4] = dist5 / dist1;
	q->v[5] = dist6 / dist1;
	q->v[6] = xt;
	q->v[7] = yt;
}

/* find_many_quads: every C(mode,4) quad from each star and its mode-1
 * closest neighbours. Combinations in lexicographic order, as ASTAP's tables. */
static int find_many_quads(const double *X, const double *Y, int nrstars, int mode,
                           quadlist_t *out) {
	int num_closest = mode, num_quads_per_group;
	int combos[35][4], nc = 0, a, b, c, d;
	int closest_indices[7];
	double closest_distances[7];
	int i, j, k, q, insert_pos;

	for (a = 0; a < num_closest; a++)
		for (b = a + 1; b < num_closest; b++)
			for (c = b + 1; c < num_closest; c++)
				for (d = c + 1; d < num_closest; d++) {
					combos[nc][0] = a; combos[nc][1] = b;
					combos[nc][2] = c; combos[nc][3] = d;
					nc++;
				}
	/* ASTAP uses only 5 of the C(5,4)=5, all 15 of C(6,4), all 35 of C(7,4) */
	num_quads_per_group = nc;

	out->n = 0;
	if (quads_reserve(out, nrstars * num_quads_per_group) < 0) return -1;

	for (i = 0; i < nrstars; i++) {
		double x1 = X[i], y1 = Y[i];
		closest_distances[0] = 0;
		closest_indices[0] = i;
		for (j = 1; j < num_closest; j++) {
			closest_indices[j] = -1;
			closest_distances[j] = 1E99;
		}
		for (j = 0; j < nrstars; j++) {
			if (i != j) {
				double dx = X[j] - x1, dy = Y[j] - y1;
				double distance = dx * dx + dy * dy;
				if (distance > 1) {
					insert_pos = -1;
					for (k = num_closest - 1; k >= 1; k--) {
						if (distance < closest_distances[k]) insert_pos = k;
						else break;
					}
					if (insert_pos >= 0) {
						for (k = num_closest - 1; k >= insert_pos + 1; k--) {
							closest_distances[k] = closest_distances[k - 1];
							closest_indices[k] = closest_indices[k - 1];
						}
						closest_distances[insert_pos] = distance;
						closest_indices[insert_pos] = j;
					}
				}
			}
		}

		if (closest_indices[num_closest - 1] != -1) {
			for (q = 0; q < num_quads_per_group; q++) {
				int s1 = closest_indices[combos[q][0]], s2 = closest_indices[combos[q][1]];
				int s3 = closest_indices[combos[q][2]], s4 = closest_indices[combos[q][3]];
				double x2 = X[s2], y2 = Y[s2], x3 = X[s3], y3 = Y[s3], x4 = X[s4], y4 = Y[s4];
				double xa = X[s1], ya = Y[s1];
				double xt = (xa + x2 + x3 + x4) * 0.25, yt = (ya + y2 + y3 + y4) * 0.25;
				int identical_quad = 0;

				for (k = 0; k < out->n; k++)
					if (fabs(xt - out->q[k].v[6]) < 1 && fabs(yt - out->q[k].v[7]) < 1) {
						identical_quad = 1;
						break;
					}
				if (!identical_quad) {
#define DST(ax, ay, bx, by) sqrt(((ax) - (bx)) * ((ax) - (bx)) + ((ay) - (by)) * ((ay) - (by)))
					store_quad(&out->q[out->n],
					           DST(xa, ya, x2, y2), DST(xa, ya, x3, y3), DST(xa, ya, x4, y4),
					           DST(x2, y2, x3, y3), DST(x2, y2, x4, y4), DST(x3, y3, x4, y4),
					           xt, yt);
#undef DST
					out->n++;
				}
			}
		}
	}
	return 0;
}

/* find_quads, revised 2026. X/Y are sorted in place when nrstars >= 150. */
static int find_quads(int nrstars_image, double *X, double *Y, int nrstars, quadlist_t *out) {
	const double GRID_INV = 0.2;
	int i, j, bandw, hash_table_len, *head = NULL, *next = NULL;
	int j_index1 = 0, j_index2 = 0, j_index3 = 0;

	if (nrstars_image < 15 && nrstars > 6) return find_many_quads(X, Y, nrstars, 7, out);
	if (nrstars_image < 30 && nrstars > 5) return find_many_quads(X, Y, nrstars, 6, out);
	if (nrstars_image < 60 && nrstars > 4) return find_many_quads(X, Y, nrstars, 5, out);

	out->n = 0;
	if (nrstars < 4) return 0;

	if (nrstars >= 150) {
		quicksort_starlist(X, Y, 0, nrstars - 1);
		bandw = (int)rint(2 * sqrt(nrstars)); /* ~twice the mean star distance */
	} else {
		bandw = nrstars; /* switch off pre-filtering in X */
	}

	hash_table_len = nrstars * 2;
	head = malloc((size_t)hash_table_len * sizeof(int));
	next = malloc((size_t)nrstars * sizeof(int));
	if (!head || !next || quads_reserve(out, nrstars) < 0) {
		free(head); free(next);
		return -1;
	}
	for (i = 0; i < hash_table_len; i++) head[i] = -1;

	for (i = 0; i < nrstars; i++) {
		double distance1 = 1E99, distance2 = 1E99, distance3 = 1E99;
		int Sstart = i - bandw < 0 ? 0 : i - bandw;
		int Send = i + bandw > nrstars - 1 ? nrstars - 1 : i + bandw;
		double x1 = X[i], y1 = Y[i];

		for (j = Sstart; j <= Send; j++) {
			if (j != i) {
				double disty = (Y[j] - y1) * (Y[j] - y1);
				if (disty < distance3) {
					double distance = (X[j] - x1) * (X[j] - x1) + disty;
					if (distance > 1) { /* not an identical star */
						if (distance < distance1) {
							distance3 = distance2; j_index3 = j_index2;
							distance2 = distance1; j_index2 = j_index1;
							distance1 = distance;  j_index1 = j;
						} else if (distance < distance2) {
							distance3 = distance2; j_index3 = j_index2;
							distance2 = distance;  j_index2 = j;
						} else if (distance < distance3) {
							distance3 = distance;  j_index3 = j;
						}
					}
				}
			}
		}

		if (distance3 < 1E99) {
			double x2 = X[j_index1], y2 = Y[j_index1];
			double x3 = X[j_index2], y3 = Y[j_index2];
			double x4 = X[j_index3], y4 = Y[j_index3];
			double xt = (x1 + x2 + x3 + x4) * 0.25, yt = (y1 + y2 + y3 + y4) * 0.25;
			int idx = abs((int)trunc(xt * GRID_INV) * 31 + (int)trunc(yt * GRID_INV)) % hash_table_len;
			int k, identical_quad = 0;

			for (k = head[idx]; k >= 0; k = next[k])
				if (fabs(xt - out->q[k].v[6]) < 1 && fabs(yt - out->q[k].v[7]) < 1) {
					identical_quad = 1;
					break;
				}
			if (!identical_quad) {
				double dx, dy, dist4, dist5, dist6;
				dx = x2 - x3; dy = y2 - y3; dist4 = sqrt(dx * dx + dy * dy);
				dx = x2 - x4; dy = y2 - y4; dist5 = sqrt(dx * dx + dy * dy);
				dx = x3 - x4; dy = y3 - y4; dist6 = sqrt(dx * dx + dy * dy);
				store_quad(&out->q[out->n], sqrt(distance1), sqrt(distance2), sqrt(distance3),
				           dist4, dist5, dist6, xt, yt);
				next[out->n] = head[idx];
				head[idx] = out->n;
				out->n++;
			}
		}
	}
	free(head);
	free(next);
	return 0;
}

static int match_push(ctx_t *c, int i, int j) {
	if (c->nr_references2 >= c->match_cap) {
		int cap = c->nr_references2 + 1000;
		int *p = realloc(c->match, (size_t)cap * 2 * sizeof(int));
		if (!p) return -1;
		c->match = p;
		c->match_cap = cap;
	}
	c->match[2 * c->nr_references2] = i;
	c->match[2 * c->nr_references2 + 1] = j;
	c->nr_references2++;
	return 0;
}

static int matches_ok(const astap_quad_t *a, const astap_quad_t *b, double tol) {
	/* short-circuit chain as ASTAP: ~98.6% of pairs fail the first check */
	return fabs(a->v[1] - b->v[1]) <= tol && fabs(a->v[2] - b->v[2]) <= tol &&
	       fabs(a->v[3] - b->v[3]) <= tol && fabs(a->v[4] - b->v[4]) <= tol &&
	       fabs(a->v[5] - b->v[5]) <= tol;
}

/* The common tail of find_fit and find_fit_using_hash: median scale ratio,
 * outlier removal, equations for lsq_fit. */
static int fit_tail(ctx_t *c, int minimum_count, double quad_tolerance) {
	const quadlist_t *q1 = &c->quads1, *q2 = &c->quads2;
	double *ratios, *sorted, median_ratio;
	int k, n2 = c->nr_references2;

	if (n2 < minimum_count) { c->nr_references = 0; return 0; }

	ratios = malloc((size_t)n2 * 2 * sizeof(double));
	if (!ratios) return 0;
	sorted = ratios + n2;
	for (k = 0; k < n2; k++)
		ratios[k] = q1->q[c->match[2 * k]].v[0] / q2->q[c->match[2 * k + 1]].v[0];
	memcpy(sorted, ratios, (size_t)n2 * sizeof(double));
	median_ratio = smedian(sorted, n2);

	if (n2 > c->A_cap) {
		int j;
		for (j = 0; j < 3; j++) { free(c->A[j]); c->A[j] = malloc((size_t)n2 * sizeof(double)); }
		free(c->bx); c->bx = malloc((size_t)n2 * sizeof(double));
		free(c->by); c->by = malloc((size_t)n2 * sizeof(double));
		if (!c->A[0] || !c->A[1] || !c->A[2] || !c->bx || !c->by) { free(ratios); c->A_cap = 0; return 0; }
		c->A_cap = n2;
	}

	c->nr_references = 0;
	for (k = 0; k < n2; k++) {
		if (fabs(median_ratio - ratios[k]) <= quad_tolerance * median_ratio) {
			const astap_quad_t *img = &q2->q[c->match[2 * k + 1]];
			const astap_quad_t *ref = &q1->q[c->match[2 * k]];
			int r = c->nr_references++;
			c->A[0][r] = img->v[6]; /* centre of the image quad */
			c->A[1][r] = img->v[7];
			c->A[2][r] = 1;
			c->bx[r] = ref->v[6];   /* centre of the database quad */
			c->by[r] = ref->v[7];
		}
	}
	free(ratios);
	return c->nr_references >= 3;
}

/* find_fit: brute force over all quad pairs. */
static int find_fit(ctx_t *c, int minimum_count, double quad_tolerance) {
	int i, j, n1 = c->quads1.n, n2 = c->quads2.n;
	c->nr_references2 = 0;
	if (n1 < minimum_count || n2 < minimum_count) { c->nr_references = 0; return 0; }
	for (i = 0; i < n1; i++)
		for (j = 0; j < n2; j++)
			if (matches_ok(&c->quads1.q[i], &c->quads2.q[j], quad_tolerance))
				if (match_push(c, i, j) < 0) return 0;
	return fit_tail(c, minimum_count, quad_tolerance);
}

/* Bin quads on their first ratio: offsets[bin]..offsets[bin+1] index idx[]. */
static int bin_quads(const quadlist_t *l, double tol, int hash_bins, int *offsets, int *idx) {
	int i, b;
	memset(offsets, 0, (size_t)(hash_bins + 1) * sizeof(int));
	for (i = 0; i < l->n; i++) {
		b = (int)trunc(l->q[i].v[1] / tol);
		if (b < 0) b = 0;
		if (b >= hash_bins) b = hash_bins - 1;
		offsets[b + 1]++;
	}
	for (b = 0; b < hash_bins; b++) offsets[b + 1] += offsets[b];
	{
		int *fill = malloc((size_t)hash_bins * sizeof(int));
		if (!fill) return -1;
		memcpy(fill, offsets, (size_t)hash_bins * sizeof(int));
		for (i = 0; i < l->n; i++) {
			b = (int)trunc(l->q[i].v[1] / tol);
			if (b < 0) b = 0;
			if (b >= hash_bins) b = hash_bins - 1;
			idx[fill[b]++] = i;
		}
		free(fill);
	}
	return 0;
}

/* find_fit_using_hash: compare only quads whose first ratio falls in the same
 * or a neighbouring bin. */
static int find_fit_using_hash(ctx_t *c, int minimum_count, double quad_tolerance) {
	const int NEIGHBOR_BINS = 1;
	int n1 = c->quads1.n, n2 = c->quads2.n, hash_bins, bin, delta, adj, i, j, ok = 0;
	int *off1, *off2, *idx1, *idx2;

	c->nr_references2 = 0;
	if (n1 < minimum_count || n2 < minimum_count) { c->nr_references = 0; return 0; }

	hash_bins = (int)rint(1.0 / quad_tolerance) + 2;
	if (hash_bins < 4) hash_bins = 4;
	if (hash_bins > 10000) hash_bins = 10000;

	off1 = malloc((size_t)(2 * (hash_bins + 1) + n1 + n2) * sizeof(int));
	if (!off1) return 0;
	off2 = off1 + hash_bins + 1;
	idx1 = off2 + hash_bins + 1;
	idx2 = idx1 + n1;
	if (bin_quads(&c->quads1, quad_tolerance, hash_bins, off1, idx1) < 0 ||
	    bin_quads(&c->quads2, quad_tolerance, hash_bins, off2, idx2) < 0)
		goto out;

	for (bin = 0; bin < hash_bins; bin++) {
		if (off1[bin] == off1[bin + 1]) continue;
		for (delta = -NEIGHBOR_BINS; delta <= NEIGHBOR_BINS; delta++) {
			adj = bin + delta;
			if (adj < 0 || adj >= hash_bins) continue;
			if (off2[adj] == off2[adj + 1]) continue;
			for (i = off1[bin]; i < off1[bin + 1]; i++)
				for (j = off2[adj]; j < off2[adj + 1]; j++)
					if (matches_ok(&c->quads1.q[idx1[i]], &c->quads2.q[idx2[j]], quad_tolerance))
						if (match_push(c, idx1[i], idx2[j]) < 0) goto out;
		}
	}
	ok = fit_tail(c, minimum_count, quad_tolerance);
out:
	free(off1);
	return ok;
}

static void reset_solution_vectors(ctx_t *c, double factor) {
	c->sx[0] = factor; c->sx[1] = 0; c->sx[2] = 0;
	c->sy[0] = 0; c->sy[1] = factor; c->sy[2] = 0;
}

static int find_offset_and_rotation(ctx_t *c, int minimum_quads, double tolerance) {
	double xy_sqr_ratio;
	int found;

	if (c->quads1.n < 120) found = find_fit(c, minimum_quads, tolerance); /* brute force */
	else found = find_fit_using_hash(c, minimum_quads, tolerance);
	if (!found) { reset_solution_vectors(c, 0.001); return 0; }

	if (!lsq_fit(c->A, c->bx, c->nr_references, c->sx) ||
	    !lsq_fit(c->A, c->by, c->nr_references, c->sy)) {
		reset_solution_vectors(c, 0.001);
		return 0;
	}
	xy_sqr_ratio = (c->sx[0] * c->sx[0] + c->sx[1] * c->sx[1]) /
	               (0.00000001 + c->sy[0] * c->sy[0] + c->sy[1] * c->sy[1]);
	if (xy_sqr_ratio < 0.9 || xy_sqr_ratio > 1.1) { /* x and y scale differ: wrong */
		reset_solution_vectors(c, 0.001);
		return 0;
	}
	return 1;
}

/* ---------------------------------------------------------- unit_astrometric_solving */

/* read_stars, 1476 branch. The search field is split over up to four tiles
 * in proportion to their share of it; each contributes its brightest stars
 * inside the field. Returns -1 if a tile file cannot be read. */
static int read_stars(astap_db_t *db, double telescope_ra, double telescope_dec, double search_field,
                      int nrstars_required, starlist_t *out, double *mag_limit) {
	int area[4], k, nrstars = 0, req2;
	double frac[4], cum = 0, ra2 = 0, dec2 = 0, mag2 = 0;

	out->n = 0;
	if (stars_reserve(out, nrstars_required > 0 ? nrstars_required : 1) < 0) return -1;
	astap_db_find_areas(telescope_ra, telescope_dec, search_field, area, frac);
	for (k = 0; k < 4; k++) {
		cum += frac[k];
		if (area[k] == 0) continue;
		if (astap_db_open(db, telescope_dec, area[k]) < 0) return -1;
		req2 = (int)trunc(nrstars_required * cum);
		if (req2 > nrstars_required) req2 = nrstars_required;
		while (nrstars < req2 &&
		       astap_db_read(db, telescope_ra, telescope_dec, search_field, &ra2, &dec2, &mag2)) {
			equatorial_standard(telescope_ra, telescope_dec, ra2, dec2, 1,
			                    &out->x[nrstars], &out->y[nrstars]);
			out->ra[nrstars] = ra2;
			out->dec[nrstars] = dec2;
			nrstars++;
		}
	}
	out->n = nrstars;
	*mag_limit = mag2 / 10;
	return 0;
}

void astap_params_default(astap_params_t *p) {
	memset(p, 0, sizeof(*p));
	p->radius_deg = 20;      /* radius_search1 */
	p->quad_tolerance = 0.007; /* quad_tolerance1 */
	p->max_stars = 500;      /* max_stars1 */
}

void astap_pixel_to_radec(const astap_solution_t *s, double x, double y, double *ra, double *dec) {
	standard_equatorial(s->tan_ra, s->tan_dec,
	                    s->sx[0] * x + s->sx[1] * y + s->sx[2],
	                    s->sy[0] * x + s->sy[1] * y + s->sy[2], 1, ra, dec);
}

/* The second-pass filter: keep catalogue stars that land inside the image
 * according to the first pass (see the header comment for why this is not
 * ASTAP's rotate()-based version). */
static void keep_visible(starlist_t *s, double tan_ra, double tan_dec, const double sx[3],
                         const double sy[3], int width, int height) {
	double det = sx[0] * sy[1] - sx[1] * sy[0];
	int i, n = 0;
	if (det == 0) return;
	for (i = 0; i < s->n; i++) {
		double X, Y, xi, yi;
		equatorial_standard(tan_ra, tan_dec, s->ra[i], s->dec[i], 1, &X, &Y);
		X -= sx[2];
		Y -= sy[2];
		xi = (sy[1] * X - sx[1] * Y) / det;
		yi = (-sy[0] * X + sx[0] * Y) / det;
		if (xi > 0 && xi < width && yi > 0 && yi < height) {
			s->x[n] = s->x[i]; s->y[n] = s->y[i];
			s->ra[n] = s->ra[i]; s->dec[n] = s->dec[i];
			n++;
		}
	}
	s->n = n;
}

int astap_solve(const astap_params_t *p, const double *stars_x, const double *stars_y, int n,
                astap_solution_t *sol) {
	const double max_fov = ASTAP_DB_MAX_FOV_DEG;
	ctx_t c;
	astap_db_t db;
	starlist_t img = { 0 }, ref = { 0 };
	double fov_org = p->fov_deg, fov2, oversize, oversize2, radius, search_field, step_size;
	double ra_seed = p->ra_hint, dec_seed = p->dec_hint, ra_database = 0, dec_database = 0;
	double ra_solved = 0, dec_solved = 0, centerX, centerY, flipped_image = 1;
	double crota2_rad = 0, cdelt1_arcsec = 0, cdelt2_arcsec = 0, mag_limit = 0;
	double prev_tan_ra = 0, prev_tan_dec = 0, prev_sx[3] = { 0 }, prev_sy[3] = { 0 };
	int database_density, max_stars, limit, nrstars_image, nrstars_required, nrstars_required2;
	int minimum_quads, max_distance, match_nr, solution = 0;
	int spiral_x, spiral_y, spiral_dx, spiral_dy, spiral_t, count, steps = 0;

	memset(sol, 0, sizeof(*sol));
	memset(&c, 0, sizeof(c));
	astap_db_init(&db, p->db_dir, p->db_name);

	if (fov_org <= 0 || fov_org > max_fov) {
		sol->error = "field of view must be between 0 and one catalogue tile (5.14 deg)";
		return 0;
	}
	fov2 = fov_org; /* no cropping needed below max_fov */

	database_density = p->db_density > 0 ? p->db_density : 9999;
	max_stars = p->max_stars;
	limit = (int)rint(database_density * fov2 * fov2 * p->width / p->height);
	if (limit < max_stars) max_stars = limit; /* database limit for this FOV */

	nrstars_image = n < max_stars ? n : max_stars; /* stars arrive brightest first */
	sol->stars_image = nrstars_image;
	nrstars_required = (int)rint(nrstars_image * ((double)p->height / p->width));

	if (nrstars_image < 5) { /* bare minimum */
		sol->error = "fewer than 5 stars in the image";
		return 0;
	}
	if (stars_reserve(&img, nrstars_image) < 0) { sol->error = "out of memory"; return 0; }
	memcpy(img.x, stars_x, (size_t)nrstars_image * sizeof(double));
	memcpy(img.y, stars_y, (size_t)nrstars_image * sizeof(double));
	img.n = nrstars_image;

	if (find_quads(nrstars_image, img.x, img.y, img.n, &c.quads2) < 0) {
		sol->error = "out of memory";
		goto done;
	}
	sol->quads_image = c.quads2.n;
	if (c.quads2.n < 3) {
		sol->error = "fewer than 3 quads in the image";
		goto done;
	}

	/* Fixed step size; with few stars the database window grows up to 2x
	 * the image height so every image quad has its database counterpart. */
	if (nrstars_image < 35) oversize = 2;
	else if (nrstars_image > 140) oversize = 1;
	else oversize = 2 * sqrt(35.0 / nrstars_image);
	if (oversize > max_fov / fov2) oversize = max_fov / fov2;
	radius = p->radius_deg;
	minimum_quads = 3 + nrstars_image / 140; /* fewer false matches in rich fields */

	search_field = fov2 * (M_PI / 180);
	step_size = search_field;
	max_distance = (int)rint(radius / (fov2 + 0.00001)); /* in steps */

	match_nr = 0;
	do { /* maximum accuracy loop: re-solve centred on the first solution */
		solution = 0;
		count = 0;
		spiral_x = 0; spiral_y = 0;
		spiral_dx = 0; spiral_dy = -1;

		do { /* square spiral [0 0],[1 0],[1 1],[0 1],[-1 1],[-1 0],[-1 -1],... */
			double flip = 0, extra, ra_database_offset;
			if (count != 0) {
				if (spiral_x == spiral_y || (spiral_x < 0 && spiral_x == -spiral_y) ||
				    (spiral_x > 0 && spiral_x == 1 - spiral_y)) {
					spiral_t = spiral_dx;
					spiral_dx = -spiral_dy;
					spiral_dy = spiral_t;
				}
				spiral_x += spiral_dx;
				spiral_y += spiral_dy;
			}

			dec_database = step_size * spiral_y + dec_seed;
			if (dec_database > M_PI / 2) { dec_database = M_PI - dec_database; flip = M_PI; }
			else if (dec_database < -M_PI / 2) { dec_database = -M_PI - dec_database; flip = M_PI; }

			extra = dec_database > 0 ? step_size / 2 : -step_size / 2; /* side furthest from the pole */
			ra_database_offset = step_size * spiral_x / cos(dec_database - extra);
			if (ra_database_offset <= M_PI / 2 + step_size / 2 && ra_database_offset >= -M_PI / 2) {
				double seperation;
				ra_database = fnmodulo(flip + ra_seed + ra_database_offset, 2 * M_PI);
				seperation = astap_ang_sep(ra_database, dec_database, ra_seed, dec_seed);
				if (seperation <= radius * M_PI / 180 + step_size / 2) { /* circular area only */
					if (match_nr == 0) {
						oversize2 = oversize;
					} else {
						double full = sqrt(pow((double)p->width / p->height, 2) + 1);
						oversize2 = oversize > full ? oversize : full;
						if (oversize2 > max_fov / fov2) oversize2 = max_fov / fov2;
					}
					nrstars_required2 = (int)rint(nrstars_required * oversize2 * oversize2);

					if (read_stars(&db, ra_database, dec_database, search_field * oversize2,
					               nrstars_required2, &ref, &mag_limit) < 0) {
						sol->error = "star database file missing or unreadable";
						goto done;
					}
					if (match_nr == 1)
						keep_visible(&ref, prev_tan_ra, prev_tan_dec, prev_sx, prev_sy,
						             p->width, p->height);

					if (find_quads(nrstars_image, ref.x, ref.y, ref.n, &c.quads1) < 0) {
						sol->error = "out of memory";
						goto done;
					}
					solution = find_offset_and_rotation(&c, minimum_quads, p->quad_tolerance);
					steps++;
				}
			}
			count++;
		} while (!solution && spiral_x <= max_distance);

		if (solution) {
			double ra7, dec7, X, Y;
			centerX = (p->width - 1) / 2.0;
			centerY = (p->height - 1) / 2.0;
			X = c.sx[0] * centerX + c.sx[1] * centerY + c.sx[2];
			Y = c.sy[0] * centerX + c.sy[1] * centerY + c.sy[2];
			standard_equatorial(ra_database, dec_database, X, Y, 1, &ra_solved, &dec_solved);
			ra_seed = ra_solved; /* re-seed the max-accuracy second pass */
			dec_seed = dec_solved;

			/* mirrored in one axis (both axes is a 180 deg rotation) */
			flipped_image = c.sx[0] * c.sy[1] - c.sx[1] * c.sy[0] > 0 ? -1 : +1;

			standard_equatorial(ra_database, dec_database,
			                    c.sx[0] * centerX + c.sx[1] * (centerY + 1) + c.sx[2],
			                    c.sy[0] * centerX + c.sy[1] * (centerY + 1) + c.sy[2], 1, &ra7, &dec7);
			crota2_rad = -position_angle(ra7, dec7, ra_solved, dec_solved);
			cdelt1_arcsec = flipped_image * sqrt(c.sx[0] * c.sx[0] + c.sx[1] * c.sx[1]);
			cdelt2_arcsec = sqrt(c.sy[0] * c.sy[0] + c.sy[1] * c.sy[1]);

			prev_tan_ra = ra_database;
			prev_tan_dec = dec_database;
			memcpy(prev_sx, c.sx, sizeof(prev_sx));
			memcpy(prev_sy, c.sy, sizeof(prev_sy));
			match_nr++;
		} else {
			match_nr = 0; /* rarely happens on the second solve */
		}
	} while (solution && match_nr < 2);

	sol->search_steps = steps;
	if (solution) { /* not in ASTAP: residuals, as a quality figure for the caller */
		int k;
		double ss = 0;
		for (k = 0; k < c.nr_references; k++) {
			double ex = c.sx[0] * c.A[0][k] + c.sx[1] * c.A[1][k] + c.sx[2] - c.bx[k];
			double ey = c.sy[0] * c.A[0][k] + c.sy[1] * c.A[1][k] + c.sy[2] - c.by[k];
			double e2 = ex * ex + ey * ey;
			ss += e2;
			if (sqrt(e2) > sol->resid_max) sol->resid_max = sqrt(e2);
		}
		sol->resid_rms = sqrt(ss / c.nr_references);
	}
	sol->nr_references = c.nr_references;
	sol->nr_references2 = c.nr_references2;
	sol->mag_limit = mag_limit;

	if (solution) {
		double ra7, dec7, crota1_rad;
		sol->solved = 1;
		sol->ra0 = ra_solved;
		sol->dec0 = dec_solved;
		sol->tan_ra = ra_database;
		sol->tan_dec = dec_database;
		memcpy(sol->sx, c.sx, sizeof(sol->sx));
		memcpy(sol->sy, c.sy, sizeof(sol->sy));
		sol->cdelt1 = cdelt1_arcsec;
		sol->cdelt2 = cdelt2_arcsec;
		sol->flipped = flipped_image < 0;
		sol->search_offset = astap_ang_sep(p->ra_hint, p->dec_hint, ra_solved, dec_solved);

		standard_equatorial(ra_database, dec_database,
		                    c.sx[0] * (centerX + flipped_image) + c.sx[1] * centerY + c.sx[2],
		                    c.sy[0] * (centerX + flipped_image) + c.sy[1] * centerY + c.sy[2], 1, &ra7, &dec7);
		crota1_rad = M_PI / 2 - position_angle(ra7, dec7, ra_solved, dec_solved);
		if (crota1_rad > M_PI) crota1_rad -= 2 * M_PI;
		sol->crota1 = crota1_rad;
		sol->crota2 = crota2_rad;
	} else if (!sol->error) {
		sol->error = "no match within the search radius";
	}

done:
	free(c.quads1.q);
	free(c.quads2.q);
	free(c.match);
	free(c.A[0]); free(c.A[1]); free(c.A[2]);
	free(c.bx); free(c.by);
	stars_free(&img);
	stars_free(&ref);
	astap_db_close(&db);
	return sol->solved;
}
