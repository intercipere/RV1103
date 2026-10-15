/*
 * Host test for the ASTAP port: a synthetic sky written in the .1476 format,
 * synthetic star lists taken from it at a known pointing, rotation, scale and
 * mirror, and the solver asked to recover them from a hint that is off by up
 * to a few degrees.
 *
 * Nothing the solver computes is used to build the truth: the projection
 * here is an independent gnomonic implementation, and the check compares
 * astap_pixel_to_radec() against it at the image centre and corners, which is
 * independent of any axis or sign convention.
 *
 * The sky's star counts follow Gaia DR3 (PlateSolving/README.md, "Star
 * density"): 56.3 stars/deg^2 brighter than V 12 all-sky, ~2.6x per
 * magnitude. -d scales that: 0.35 is the galactic-pole density, 2.3 the
 * plane.
 *
 *   solve_test                  the default matrix, exit 1 on failure
 *   solve_test -d 0.35 -c 12 -m 11 -n 50 -o 5   one configuration
 *   ... -k catdir -w cases.txt  also keep the catalogue and write the cases,
 *                               for solve_bench on the board
 */
#include "astap_db.h"
#include "astap_solve.h"
#include "catalog.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ARCSEC (M_PI / 180 / 3600)
#define WIDTH 2304
#define HEIGHT 1296
#define SCALE 2.78 /* arcsec/px: SC3336 at 182 mm */

static unsigned long long rng_state;
static double urand(void) { /* xorshift64*, (0,1] */
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return ((rng_state * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0) + 1e-17;
}
static double nrand(void) { return sqrt(-2 * log(urand())) * cos(2 * M_PI * urand()); }

static double now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- sky */

static cat_star_t *sky;
static size_t sky_n;

static void make_sky(double density_factor, double mag_max, unsigned long long seed) {
	const double k = 0.418;                  /* log10 counts per magnitude, Gaia V 9..12 */
	double per_deg2 = 56.3 * density_factor * pow(10, k * (mag_max - 12));
	size_t i;
	sky_n = (size_t)(41252.96 * per_deg2);
	sky = realloc(sky, sky_n * sizeof(cat_star_t));
	rng_state = seed;
	for (i = 0; i < sky_n; i++) {
		double z = 2 * urand() - 1;
		sky[i].ra = 2 * M_PI * urand();
		if (sky[i].ra >= 2 * M_PI) sky[i].ra = 0;
		sky[i].dec = asin(z);
		sky[i].mag = mag_max + log10(urand()) / k;
		if (sky[i].mag < -1.5) sky[i].mag = -1.5;
	}
}

/* ---- truth projection, independent of astap_solve.c */

typedef struct {
	double ra0, dec0, rot, scale; /* scale arcsec/px */
	int mirror;
} truth_t;

static int sky_to_pixel(const truth_t *t, double ra, double dec, double *x, double *y) {
	double D = sin(dec) * sin(t->dec0) + cos(dec) * cos(t->dec0) * cos(ra - t->ra0);
	double xi, eta, u, v;
	if (D <= 0.1) return 0;
	xi = cos(dec) * sin(ra - t->ra0) / D;                                        /* east */
	eta = (sin(dec) * cos(t->dec0) - cos(dec) * sin(t->dec0) * cos(ra - t->ra0)) / D; /* north */
	u = xi / ARCSEC / t->scale;
	v = eta / ARCSEC / t->scale;
	if (t->mirror) u = -u;
	*x = (WIDTH - 1) / 2.0 + cos(t->rot) * u - sin(t->rot) * v;
	*y = (HEIGHT - 1) / 2.0 - (sin(t->rot) * u + cos(t->rot) * v);
	return 1;
}

static void pixel_to_sky(const truth_t *t, double x, double y, double *ra, double *dec) {
	double dx = x - (WIDTH - 1) / 2.0, dy = (HEIGHT - 1) / 2.0 - y;
	double u = cos(t->rot) * dx + sin(t->rot) * dy, v = -sin(t->rot) * dx + cos(t->rot) * dy;
	double xi, eta, den;
	if (t->mirror) u = -u;
	xi = u * t->scale * ARCSEC;
	eta = v * t->scale * ARCSEC;
	den = cos(t->dec0) - eta * sin(t->dec0);
	*ra = fmod(t->ra0 + atan2(xi, den) + 4 * M_PI, 2 * M_PI);
	*dec = atan((sin(t->dec0) + eta * cos(t->dec0)) / sqrt(xi * xi + den * den));
}

/* Move distance d from ra/dec towards position angle pa. */
static void offset(double ra, double dec, double d, double pa, double *ra2, double *dec2) {
	*dec2 = asin(sin(dec) * cos(d) + cos(dec) * sin(d) * cos(pa));
	*ra2 = ra + atan2(sin(pa) * sin(d) * cos(dec), cos(d) - sin(dec) * sin(*dec2));
	*ra2 = fmod(*ra2 + 4 * M_PI, 2 * M_PI);
}

/* ---- synthetic detections */

typedef struct { double x, y, flux; } det_t;
static int cmp_flux(const void *a, const void *b) {
	double fa = ((const det_t *)a)->flux, fb = ((const det_t *)b)->flux;
	return fa < fb ? 1 : fa > fb ? -1 : 0;
}

/* Stars the camera would detect: a soft limit at mag_limit, 5% lost
 * (blends, edges), 0.15 mag of brightness scatter (V vs a mono sensor's
 * passband), 0.25 px centroid noise, and 5% spurious detections. */
static int detect(const truth_t *t, double mag_limit, det_t *out, int cap) {
	double r = sqrt(pow(WIDTH * t->scale, 2) + pow(HEIGHT * t->scale, 2)) * ARCSEC;
	int n = 0, nfalse, i;
	size_t s;
	for (s = 0; s < sky_n && n < cap; s++) {
		double x, y;
		if (fabs(sky[s].dec - t->dec0) > r) continue;
		if (astap_ang_sep(sky[s].ra, sky[s].dec, t->ra0, t->dec0) > r) continue;
		if (!sky_to_pixel(t, sky[s].ra, sky[s].dec, &x, &y)) continue;
		if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) continue;
		if (urand() > 1 / (1 + exp((sky[s].mag - mag_limit) / 0.15))) continue;
		if (urand() < 0.05) continue;
		out[n].x = x + 0.25 * nrand();
		out[n].y = y + 0.25 * nrand();
		out[n].flux = pow(10, -0.4 * (sky[s].mag + 0.15 * nrand()));
		n++;
	}
	nfalse = n / 20;
	for (i = 0; i < nfalse && n < cap; i++, n++) {
		out[n].x = urand() * WIDTH;
		out[n].y = urand() * HEIGHT;
		out[n].flux = pow(10, -0.4 * (mag_limit - 1.5 * urand()));
	}
	qsort(out, n, sizeof(det_t), cmp_flux);
	return n;
}

/* ---- one configuration */

#define MAX_TRIALS 2000
typedef struct {
	int trials, solved, wrong, poor_fit;
	int trials10, solved10; /* fields with >= 10 detected stars */
	int failed20;           /* failures with >= 20 detected stars */
	double centre[MAX_TRIALS], corner[MAX_TRIALS]; /* errors of solved trials, arcsec */
	double total_ms, max_ms;
	int max_steps, min_stars;
} stats_t;

static double pct(const double *v, int n, double q) {
	double *t = malloc((n ? n : 1) * sizeof(double)), r;
	int i;
	if (!n) { free(t); return 0; }
	memcpy(t, v, n * sizeof(double));
	for (i = 1; i < n; i++) { /* insertion sort, n is small */
		double x = t[i];
		int j = i - 1;
		while (j >= 0 && t[j] > x) { t[j + 1] = t[j]; j--; }
		t[j + 1] = x;
	}
	r = t[(int)(q * (n - 1) + 0.5)];
	free(t);
	return r;
}

static const char *tmpdir;
static const char *catdir;   /* -k: keep the catalogue here */
static FILE *casefile;       /* -w: cases for solve_bench on the board */

static void run(double density, double cat_limit, double img_limit, int trials,
                double max_offset_deg, unsigned long long seed, stats_t *st, int verbose) {
	static det_t det[20000];
	static double xs[20000], ys[20000];
	int i;

	make_sky(density, cat_limit, seed);
	if (cat_write(catdir ? catdir : tmpdir, "syn", "synthetic test sky", sky, sky_n, cat_limit) < 0) {
		fprintf(stderr, "cannot write catalogue to %s\n", catdir ? catdir : tmpdir);
		exit(2);
	}
	/* cat_write sorted the sky by tile; detection only needs it unsorted-agnostic */

	memset(st, 0, sizeof(*st));
	st->min_stars = 1 << 30;
	rng_state = seed * 7919 + 1;
	for (i = 0; i < trials; i++) {
		truth_t t;
		astap_params_t p;
		astap_solution_t sol;
		double t0, ms, err = 0, err_centre = 0, scale_err;
		int n, k, j;

		t.ra0 = 2 * M_PI * urand();
		t.dec0 = asin(2 * urand() - 1);
		if (i == 0) t.dec0 = 89.6 * M_PI / 180; /* field containing the pole */
		if (i == 1) t.dec0 = -89.2 * M_PI / 180;
		if (i == 2) t.ra0 = 0.2 * M_PI / 180;   /* RA wrap */
		t.rot = 2 * M_PI * urand();
		t.scale = SCALE;
		t.mirror = i & 1;

		n = detect(&t, img_limit, det, 20000);
		for (k = 0; k < n; k++) { xs[k] = det[k].x; ys[k] = det[k].y; }
		if (n < st->min_stars) st->min_stars = n;

		astap_params_default(&p);
		p.db_dir = catdir ? catdir : tmpdir;
		p.db_name = "syn";
		p.width = WIDTH;
		p.height = HEIGHT;
		offset(t.ra0, t.dec0, max_offset_deg * M_PI / 180 * sqrt(urand()), 2 * M_PI * urand(),
		       &p.ra_hint, &p.dec_hint);
		p.fov_deg = HEIGHT * SCALE / 3600 * (1 + 0.03 * (2 * urand() - 1)); /* +-3% scale hint */

		if (casefile) { /* hint, fov, the truth at centre + corners, then the stars */
			const double px[5] = { (WIDTH - 1) / 2.0, 0, WIDTH - 1, 0, WIDTH - 1 };
			const double py[5] = { (HEIGHT - 1) / 2.0, 0, 0, HEIGHT - 1, HEIGHT - 1 };
			fprintf(casefile, "case %d %d %.12f %.12f %.9f", i, n, p.ra_hint, p.dec_hint, p.fov_deg);
			for (j = 0; j < 5; j++) {
				double ra_t, dec_t;
				pixel_to_sky(&t, px[j], py[j], &ra_t, &dec_t);
				fprintf(casefile, " %.12f %.12f", ra_t, dec_t);
			}
			fprintf(casefile, "\n");
			for (k = 0; k < n; k++) fprintf(casefile, "%.3f %.3f\n", xs[k], ys[k]);
		}

		t0 = now_ms();
		astap_solve(&p, xs, ys, n, &sol);
		ms = now_ms() - t0;
		st->trials++;
		st->total_ms += ms;
		if (ms > st->max_ms) st->max_ms = ms;

		if (n >= 10) st->trials10++;
		if (!sol.solved) {
			if (n >= 20) st->failed20++;
			if (verbose)
				printf("  trial %2d: FAIL  %3d stars  ra %7.3f dec %+7.3f  %s\n", i, n,
				       t.ra0 * 180 / M_PI, t.dec0 * 180 / M_PI, sol.error);
			continue;
		}
		/* the solution's sky position of the centre and corners against the truth */
		{
			const double px[5] = { (WIDTH - 1) / 2.0, 0, WIDTH - 1, 0, WIDTH - 1 };
			const double py[5] = { (HEIGHT - 1) / 2.0, 0, 0, HEIGHT - 1, HEIGHT - 1 };
			for (j = 0; j < 5; j++) {
				double ra_s, dec_s, ra_t, dec_t, e;
				astap_pixel_to_radec(&sol, px[j], py[j], &ra_s, &dec_s);
				pixel_to_sky(&t, px[j], py[j], &ra_t, &dec_t);
				e = astap_ang_sep(ra_s, dec_s, ra_t, dec_t) / ARCSEC;
				if (j == 0) err_centre = e;
				if (e > err) err = e;
			}
		}
		scale_err = fabs(fabs(sol.cdelt2) / SCALE - 1);
		/* A misidentified field puts the centre degrees off; a solve with a
		 * few false quad matches in its fit is merely imprecise. */
		if (err_centre > 120) {
			st->wrong++;
			if (verbose)
				printf("  trial %2d: WRONG %3d stars  error %.0f\" (centre %.0f\")  %d/%d refs  %d quads  scale %.4f  resid rms %.1f\" max %.1f\"\n",
				       i, n, err, err_centre, sol.nr_references, sol.nr_references2, sol.quads_image,
				       sol.cdelt2 / SCALE, sol.resid_rms, sol.resid_max);
			continue;
		}
		if (n >= 10) st->solved10++;
		st->centre[st->solved] = err_centre;
		st->corner[st->solved] = err;
		st->solved++;
		if (err > 30) {
			st->poor_fit++;
			if (verbose > 1)
				printf("  trial %2d: poor fit %3d stars  corner %.0f\" centre %.0f\"  %d refs  scale %.4f  resid rms %.1f\" max %.1f\"\n",
				       i, n, err, err_centre, sol.nr_references, sol.cdelt2 / SCALE,
				       sol.resid_rms, sol.resid_max);
		}
		(void)scale_err;
		if (sol.search_steps > st->max_steps) st->max_steps = sol.search_steps;
		if (verbose > 1)
			printf("  trial %2d: ok %3d stars %3d quads %3d refs  err %.2f\"  %d steps  %.1f ms  mag %.1f\n",
			       i, sol.stars_image, sol.quads_image, sol.nr_references, err,
			       sol.search_steps, ms, sol.mag_limit);
	}
}

static void print_stats(double d, double c, double m, double o, const stats_t *s) {
	printf("density %.2fx  cat V<%.1f  image V<%.1f  hint<=%.0f deg | %3d/%3d solved, %d wrong, min %3d stars"
	       " | centre err p50 %.1f\" p95 %.1f\" max %.1f\" | corner p95 %.1f\", %d poor (>30\")"
	       " | %.1f ms avg, %.0f max, %d steps max | >=10 stars: %d/%d\n",
	       d, c, m, o, s->solved, s->trials, s->wrong, s->min_stars,
	       pct(s->centre, s->solved, 0.5), pct(s->centre, s->solved, 0.95), pct(s->centre, s->solved, 1),
	       pct(s->corner, s->solved, 0.95), s->poor_fit,
	       s->total_ms / s->trials, s->max_ms, s->max_steps, s->solved10, s->trials10);
}

/* Reader round trip: what cat_write stores, astap_db_read returns. */
static int roundtrip_test(void) {
	cat_star_t st[4] = {
		{ 1.0, 0.3, 5.0 }, { 1.0001, 0.3002, 5.04 }, { 1.002, 0.2999, 8.3 }, { 1.001, 0.301, 11.9 },
	};
	cat_star_t want[4];
	astap_db_t db;
	double ra, dec, mag, worst = 0;
	int area, got = 0, i;
	double sE, sW, sN, sS;
	memcpy(want, st, sizeof(st));
	if (cat_write(tmpdir, "rt", "roundtrip", st, 4, 12) != 4) return 1;
	astap_db_area_and_boundaries(1.0, 0.3, &area, &sE, &sW, &sN, &sS);
	astap_db_init(&db, tmpdir, "rt");
	if (astap_db_open(&db, 0.3, area) < 0) return 1;
	while (astap_db_read(&db, 1.0, 0.3, 0.1, &ra, &dec, &mag)) {
		/* stars come back bright to faint, i.e. in want[] order */
		double e = astap_ang_sep(ra, dec, want[got].ra, want[got].dec) / ARCSEC;
		if (e > worst) worst = e;
		if (fabs(mag / 10 - round(want[got].mag * 10) / 10) > 1e-9) {
			printf("roundtrip: star %d magnitude %.1f, want %.1f\n", got, mag / 10, want[got].mag);
			return 1;
		}
		got++;
	}
	astap_db_close(&db);
	printf("roundtrip: %d/4 stars back, worst position error %.3f\" (format resolution 0.077\")\n",
	       got, worst);
	for (i = 0; i < 4; i++) (void)i;
	return got != 4 || worst > 0.1;
}

int main(int argc, char **argv) {
	char dir[] = "/tmp/solve_test_XXXXXX";
	double d = -1, c = 12, m = 11, o = 5;
	int n = 100, opt, fail = 0, verbose = 1;
	stats_t s;

	while ((opt = getopt(argc, argv, "d:c:m:n:o:vk:w:")) != -1) {
		switch (opt) {
		case 'd': d = atof(optarg); break;
		case 'c': c = atof(optarg); break;
		case 'm': m = atof(optarg); break;
		case 'n': n = atoi(optarg); if (n > MAX_TRIALS) n = MAX_TRIALS; break;
		case 'o': o = atof(optarg); break;
		case 'v': verbose = 2; break;
		case 'k': catdir = optarg; mkdir(catdir, 0755); break;
		case 'w': casefile = fopen(optarg, "w"); if (!casefile) { perror(optarg); return 2; } break;
		default: return 2;
		}
	}
	tmpdir = mkdtemp(dir);
	if (!tmpdir) { perror("mkdtemp"); return 2; }

	if (d > 0) {
		run(d, c, m, n, o, 1, &s, verbose);
		print_stats(d, c, m, o, &s);
		fail = s.wrong > 0;
	} else {
		/* The default matrix. Pass criteria: never a wrong solution; 95% of
		 * the centres within 5"; >= 95% solved where 10+ stars are detected,
		 * and no failure at all with 20+. The last two rows are too shallow
		 * for the pole on purpose: they document the limit, and still must
		 * never return a wrong answer. */
		static const double cfg[][4] = {
			/* density, catalogue limit, image limit, hint error */
			{ 1.00, 12, 11.0, 5 },
			{ 0.35, 12, 11.0, 5 },  /* galactic pole */
			{ 2.30, 12, 11.0, 5 },  /* galactic plane */
			{ 0.35, 12, 11.5, 5 },
			{ 1.00, 11, 10.5, 5 },  /* the smaller V<11 catalogue */
			{ 0.35, 11, 10.5, 2 },
		};
		size_t i;
		fail |= roundtrip_test();
		for (i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
			run(cfg[i][0], cfg[i][1], cfg[i][2], n, cfg[i][3], 1 + i, &s, verbose);
			print_stats(cfg[i][0], cfg[i][1], cfg[i][2], cfg[i][3], &s);
			if (s.wrong > 0 || s.failed20 > 0 || pct(s.centre, s.solved, 0.95) > 5 ||
			    (s.trials10 && s.solved10 < 0.95 * s.trials10))
				fail = 1;
		}
		printf(fail ? "FAIL\n" : "PASS\n");
	}
	{
		char cmd[128];
		snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
		if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", tmpdir);
	}
	if (casefile) fclose(casefile);
	free(sky);
	return fail;
}
