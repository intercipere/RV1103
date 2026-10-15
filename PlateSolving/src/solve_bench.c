/*
 * On-board benchmark: solves the cases solve_test wrote on the host (-k/-w)
 * against the catalogue it wrote, and reports time, accuracy and memory.
 * The board cannot generate the synthetic sky itself (~56 MB).
 *
 *   solve_bench <catalogue dir> <cases.txt>
 */
#include "astap_solve.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARCSEC (M_PI / 180 / 3600)
#define WIDTH 2304
#define HEIGHT 1296
#define MAXSTARS 20000

static double now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void print_vm(const char *when) {
	char line[128];
	FILE *f = fopen("/proc/self/status", "r");
	if (!f) return;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "VmHWM", 5) || !strncmp(line, "VmRSS", 5))
			printf("%s %s", when, line);
	fclose(f);
}

int main(int argc, char **argv) {
	static double xs[MAXSTARS], ys[MAXSTARS];
	const double px[5] = { (WIDTH - 1) / 2.0, 0, WIDTH - 1, 0, WIDTH - 1 };
	const double py[5] = { (HEIGHT - 1) / 2.0, 0, 0, HEIGHT - 1, HEIGHT - 1 };
	char line[512];
	FILE *f;
	int cases = 0, solved = 0, wrong = 0, poor = 0, steps_max = 0;
	double total = 0, worst = 0, centre_max = 0;

	if (argc != 3) { fprintf(stderr, "usage: %s catdir cases.txt\n", argv[0]); return 2; }
	f = fopen(argv[2], "r");
	if (!f) { perror(argv[2]); return 2; }
	print_vm("start");

	while (fgets(line, sizeof(line), f)) {
		astap_params_t p;
		astap_solution_t sol;
		double truth[10], t0, ms, err = 0, err_c = 0;
		int id, n, k;
		if (sscanf(line, "case %d %d %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf",
		           &id, &n, &p.ra_hint, &p.dec_hint, &p.fov_deg, &truth[0], &truth[1], &truth[2],
		           &truth[3], &truth[4], &truth[5], &truth[6], &truth[7], &truth[8], &truth[9]) != 15)
			continue;
		{
			double ra_h = p.ra_hint, dec_h = p.dec_hint, fov = p.fov_deg;
			astap_params_default(&p);
			p.ra_hint = ra_h; p.dec_hint = dec_h; p.fov_deg = fov;
		}
		p.db_dir = argv[1];
		p.db_name = "syn";
		p.width = WIDTH;
		p.height = HEIGHT;
		for (k = 0; k < n && fgets(line, sizeof(line), f); k++)
			if (k < MAXSTARS) sscanf(line, "%lf %lf", &xs[k], &ys[k]);
		if (n > MAXSTARS) n = MAXSTARS;

		t0 = now_ms();
		astap_solve(&p, xs, ys, n, &sol);
		ms = now_ms() - t0;
		total += ms;
		if (ms > worst) worst = ms;
		cases++;
		if (!sol.solved) {
			printf("case %3d: FAIL  %3d stars  %.0f ms  %s\n", id, n, ms, sol.error);
			continue;
		}
		for (k = 0; k < 5; k++) {
			double ra, dec, e;
			astap_pixel_to_radec(&sol, px[k], py[k], &ra, &dec);
			e = astap_ang_sep(ra, dec, truth[2 * k], truth[2 * k + 1]) / ARCSEC;
			if (k == 0) err_c = e;
			if (e > err) err = e;
		}
		if (err_c > 120) { wrong++; printf("case %3d: WRONG centre %.0f\"\n", id, err_c); continue; }
		solved++;
		if (err > 30) poor++;
		if (err_c > centre_max) centre_max = err_c;
		if (sol.search_steps > steps_max) steps_max = sol.search_steps;
		printf("case %3d: ok %3d stars  %3d refs  %3d steps  centre %.2f\"  corners %.1f\"  %.0f ms\n",
		       id, n, sol.nr_references, sol.search_steps, err_c, err, ms);
	}
	fclose(f);
	printf("%d/%d solved, %d wrong, %d poor (>30\" at a corner), centre err max %.1f\" | %.0f ms avg, %.0f ms max, %d steps max\n",
	       solved, cases, wrong, poor, centre_max, cases ? total / cases : 0, worst, steps_max);
	print_vm("end");
	return wrong > 0;
}
