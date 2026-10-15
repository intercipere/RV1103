/*
 * guide_cal_test -- calibration and closed-loop guiding against the simulated
 * mount, with pass/fail checks.
 *
 * The simulated mount is configured with a camera rotation, guide rates,
 * backlash, periodic error and drift that the guider is NOT told. Each
 * scenario runs PHD2's calibration against it, then compares what calibration
 * found with what was configured, then guides and compares the true residual
 * error against the same sky left unguided.
 *
 * This is the closest thing to a night on the sky that runs in daylight. It
 * proves the port is self-consistent and that the signs close the loop; it
 * cannot prove anything about a real mount's mechanics, which is the part that
 * still needs one.
 *
 *   make test         (host)
 *   ./guide_cal_test [-v]
 */
#include "guide_algo.h"
#include "guide_cal.h"
#include "guide_mount.h"
#include "guide_source.h"
#include "guide_star.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define W 2304
#define H 1296
#define SEARCH 15
#define MAX_CAL_FRAMES 400

static int verbose;
static int failures;

static double deg(double r) { return r * 180.0 / M_PI; }

static double angle_diff_deg(double a, double b) {
	return fabs(deg(guide_norm_angle(a - b)));
}

static void check(int ok, const char *what) {
	printf("    %-4s %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		failures++;
}

typedef struct {
	const char *name;
	guide_sim_mount_cfg_t sim;
	guide_dec_mode_t dec_mode;
	int expect_fail;            /* 0, or which failure is expected */
	const char *expect_err;     /* substring of the expected error */
	const char *expect_advisory;
	double angle_tol_deg;
	double rate_tol;            /* fractional */
	int guide_frames;
	double rms_max;             /* closed-loop true RMS, px, must be under this */
} scenario_t;

/* Measures the star near (*x, *y) and moves the estimate there. */
static int measure(guide_source_t *src, double *x, double *y) {
	guide_star_t s;
	if (guide_star_find(&src->image, SEARCH, (int)floor(*x + 0.5),
	                    (int)floor(*y + 0.5), 1.5, 25.0, &s) != GUIDE_STAR_OK)
		return -1;
	*x = s.x;
	*y = s.y;
	return 0;
}

/* RMS of the true star offset from where it started, over n frames, with or
 * without guiding. Returns -1 on a lost star. */
static double run_guiding(guide_source_t *src, guide_mount_t *m, guide_cal_t *c,
                          int n, int closed, double *x, double *y,
                          int *pulses_ra, int *pulses_dec, int *lost) {
	guide_algo_t ra, dec;
	double lock_x, lock_y, tx0, ty0, sum = 0.0;
	int i, count = 0;
	char err[128];

	guide_algo_init(&ra, GUIDE_ALGO_HYSTERESIS, 0.15);
	guide_algo_init(&dec, GUIDE_ALGO_RESIST_SWITCH, 0.15);

	/* Lock where the star is now, the way PHD2 sets the lock position on
	 * entering GUIDING. */
	guide_source_next(src);
	if (measure(src, x, y) != 0)
		return -1;
	lock_x = *x;
	lock_y = *y;
	guide_source_truth(src, &tx0, &ty0);
	*pulses_ra = *pulses_dec = *lost = 0;

	for (i = 0; i < n; i++) {
		double tx, ty, ex, ey;
		guide_move_t mv;

		guide_source_next(src);
		guide_source_truth(src, &tx, &ty);
		ex = tx - tx0;
		ey = ty - ty0;
		sum += ex * ex + ey * ey;
		count++;

		if (measure(src, x, y) != 0) {
			(*lost)++;
			continue;
		}
		if (!closed)
			continue;
		guide_cal_guide_step(c, &ra, &dec, *x - lock_x, *y - lock_y, &mv);
		if (verbose > 1)
			printf("      %3d err %+6.2f %+6.2f  mount %+6.2f %+6.2f  -> %s %4d  %s %4d\n",
			       i, ex, ey, mv.mount_x, mv.mount_y, guide_dir_name(mv.dir_x),
			       mv.ms_x, guide_dir_name(mv.dir_y), mv.ms_y);
		if (mv.ms_x > 0) {
			guide_mount_pulse(m, mv.dir_x, mv.ms_x, err, sizeof(err));
			(*pulses_ra)++;
		}
		if (mv.ms_y > 0) {
			guide_mount_pulse(m, mv.dir_y, mv.ms_y, err, sizeof(err));
			(*pulses_dec)++;
		}
	}
	return count ? sqrt(sum / count) : 0.0;
}

static void run(const scenario_t *sc) {
	guide_source_t src;
	guide_mount_t mount;
	guide_cal_t cal;
	double x, y, tx, ty;
	int frames, rc = GUIDE_CAL_CONTINUE, lost_cal = 0;
	char err[128];
	double a = sc->sim.angle_deg * M_PI / 180.0;
	double want_x = guide_norm_angle(a + M_PI);
	double want_y = guide_norm_angle(a + (sc->sim.dec_flip ? -M_PI / 2 : M_PI / 2));
	double angle_tol = sc->angle_tol_deg;

	printf("\n%s\n", sc->name);
	if (guide_source_open_sim(&src, NULL, W, H, 6) != 0) {
		printf("    FAIL cannot open sim: %s\n", src.err);
		failures++;
		return;
	}
	guide_source_sim_configure(&src, &sc->sim);
	guide_source_sim_mount(&src, &mount);
	guide_cal_init(&cal);
	cal.dec_mode = sc->dec_mode;

	/* Seed the search on the brightest injected star's true position. Star
	 * selection is autofind's job and is tested separately; this test is
	 * about what happens after a star is chosen. */
	guide_source_next(&src);
	guide_source_truth(&src, &tx, &ty);
	x = src.stars[0].x + tx;
	y = src.stars[0].y + ty;
	if (measure(&src, &x, &y) != 0) {
		printf("    FAIL star not found at start\n");
		failures++;
		guide_source_close(&src);
		return;
	}
	guide_cal_begin(&cal, &mount, x, y);

	for (frames = 0; frames < MAX_CAL_FRAMES; frames++) {
		guide_cal_state_t before = cal.state;
		guide_source_next(&src);
		if (measure(&src, &x, &y) != 0) {
			/* PHD2: "Star lost during calibration... blundering on" */
			lost_cal++;
			continue;
		}
		rc = guide_cal_update(&cal, &mount, x, y);
		if (verbose)
			printf("      %-8s %-34s -> %s %d ms\n", guide_cal_state_name(before),
			       cal.status, guide_dir_name(cal.move_dir), cal.move_ms);
		if (rc != GUIDE_CAL_CONTINUE)
			break;
		guide_mount_pulse(&mount, cal.move_dir, cal.move_ms, err, sizeof(err));
	}

	if (sc->expect_fail) {
		char what[200];
		snprintf(what, sizeof(what), "fails with \"%s\" (got \"%s\")",
		         sc->expect_err, rc == GUIDE_CAL_FAILED ? cal.err : "no failure");
		check(rc == GUIDE_CAL_FAILED && strstr(cal.err, sc->expect_err), what);
		check(!cal.calibrated, "calibration left cleared after failure");
		guide_source_close(&src);
		return;
	}

	{
		char what[320];
		snprintf(what, sizeof(what), "completes (%d frames, %ld pulses, %d lost)%s%s",
		         frames + 1, src.sim_pulses, lost_cal,
		         rc == GUIDE_CAL_FAILED ? ": " : "",
		         rc == GUIDE_CAL_FAILED ? cal.err : "");
		check(rc == GUIDE_CAL_DONE && cal.calibrated, what);
		if (rc != GUIDE_CAL_DONE) {
			guide_source_close(&src);
			return;
		}
	}

	printf("         RA  angle %7.2f deg (sim %7.2f)  rate %.5f px/ms (sim %.5f)  %d steps\n",
	       deg(cal.cal.x_angle), deg(want_x), cal.cal.x_rate, sc->sim.ra_rate,
	       cal.cal.ra_steps);
	if (cal.cal.y_rate != GUIDE_CAL_RATE_UNCALIBRATED)
		printf("         Dec angle %7.2f deg (sim %7.2f)  rate %.5f px/ms (sim %.5f)  %d steps\n",
		       deg(cal.cal.y_angle), deg(want_y), cal.cal.y_rate, sc->sim.dec_rate,
		       cal.cal.dec_steps);
	else
		printf("         Dec not calibrated (Dec guide mode off)\n");
	if (cal.advisory[0])
		printf("         advisory: %s\n", cal.advisory);

	/*
	 * Periodic error keeps running while each leg is measured, and pushes the
	 * star sideways across it -- PHD2 has the same exposure, which is why it
	 * advises calibrating with short legs well away from the pole. The worst
	 * case is a full peak-to-peak PE swing across one leg, so that sets the
	 * tolerance rather than a number tuned until the test passed. Measured:
	 * the -100 deg case is 6.7 deg off with PE on and 0.09 deg with PE off,
	 * with nothing else changed.
	 */
	if (sc->sim.pe_amplitude > 0.0) {
		double pe_tol = deg(atan(2.0 * sc->sim.pe_amplitude / cal.distance_px)) + 1.0;
		if (pe_tol > angle_tol)
			angle_tol = pe_tol;
	}
	{
		char what[160];
		double ea = angle_diff_deg(cal.cal.x_angle, want_x);
		double er = fabs(cal.cal.x_rate / sc->sim.ra_rate - 1.0);
		snprintf(what, sizeof(what), "RA angle within %.1f deg (%.2f), rate within %.0f%% (%.1f%%)",
		         angle_tol, ea, sc->rate_tol * 100, er * 100);
		check(ea <= angle_tol && er <= sc->rate_tol, what);
	}
	if (sc->dec_mode != GUIDE_DEC_NONE) {
		char what[160];
		double ea = angle_diff_deg(cal.cal.y_angle, want_y);
		double er = fabs(cal.cal.y_rate / sc->sim.dec_rate - 1.0);
		snprintf(what, sizeof(what), "Dec angle within %.1f deg (%.2f), rate within %.0f%% (%.1f%%)",
		         angle_tol, ea, sc->rate_tol * 100, er * 100);
		check(ea <= angle_tol && er <= sc->rate_tol, what);
	} else {
		check(cal.cal.y_rate == GUIDE_CAL_RATE_UNCALIBRATED, "Dec left uncalibrated");
	}
	if (sc->expect_advisory) {
		char what[200];
		snprintf(what, sizeof(what), "advisory mentions \"%s\"", sc->expect_advisory);
		check(strstr(cal.advisory, sc->expect_advisory) != NULL, what);
	}

	/* Calibration should leave the star near where it began: west then
	 * east, north then south, nudge. */
	{
		double tx1, ty1, back;
		char what[160];
		guide_source_truth(&src, &tx1, &ty1);
		back = hypot(tx1 - tx, ty1 - ty);
		snprintf(what, sizeof(what), "star returned near its start (%.2f px away)", back);
		check(back < 6.0, what);
	}

	if (sc->guide_frames > 0) {
		guide_source_t open_src;
		guide_mount_t open_mount;
		double rms_closed, rms_open, ox, oy, ot_x, ot_y;
		int pr, pd, lost, opr, opd, olost;
		char what[200];

		/* Open loop on an identical sky from the same frame number, for the
		 * comparison. A second sim source costs another frame of memory,
		 * which is fine on the host this test runs on. */
		guide_source_open_sim(&open_src, NULL, W, H, 6);
		guide_source_sim_configure(&open_src, &sc->sim);
		guide_source_sim_mount(&open_src, &open_mount);
		open_src.frame = src.frame;
		guide_source_next(&open_src);
		guide_source_truth(&open_src, &ot_x, &ot_y);
		ox = open_src.stars[0].x + ot_x;
		oy = open_src.stars[0].y + ot_y;
		measure(&open_src, &ox, &oy);
		open_src.frame--;

		rms_closed = run_guiding(&src, &mount, &cal, sc->guide_frames, 1,
		                         &x, &y, &pr, &pd, &lost);
		rms_open = run_guiding(&open_src, &open_mount, &cal, sc->guide_frames, 0,
		                       &ox, &oy, &opr, &opd, &olost);
		printf("         guiding %d frames: true RMS %.3f px guided vs %.3f px unguided"
		       "  (%d RA / %d Dec pulses, %d lost)\n",
		       sc->guide_frames, rms_closed, rms_open, pr, pd, lost);
		/* With nothing moving the sky (the ideal case) there is nothing to
		 * beat; guiding must then simply not make it worse. */
		snprintf(what, sizeof(what), "guided RMS under %.2f px and %s unguided",
		         sc->rms_max, rms_open > 1e-6 ? "below" : "no worse than");
		check(rms_closed >= 0 && rms_closed < sc->rms_max &&
		      (rms_open > 1e-6 ? rms_closed < rms_open : rms_closed <= rms_open + 1e-9),
		      what);
		check(lost == 0, "no star lost while guiding");
		guide_source_close(&open_src);
	}
	guide_source_close(&src);
}

/* PHD2's TestTransforms, in spirit: with orthogonal axes the inverse transform
 * undoes the forward one, at every rotation and with either Dec sense. */
static void test_transforms(void) {
	int flip, i, bad = 0;
	double worst = 0.0;
	printf("\ntransforms\n");
	for (flip = 0; flip < 2; flip++) {
		for (i = 0; i < 360; i += 15) {
			guide_cal_t c;
			guide_calibration_t k;
			double xa = i * M_PI / 180.0, cx, cy, mx, my, bx, by;
			guide_cal_init(&c);
			memset(&k, 0, sizeof(k));
			k.x_angle = guide_norm_angle(xa);
			k.y_angle = guide_norm_angle(xa + (flip ? -M_PI / 2 : M_PI / 2));
			k.x_rate = k.y_rate = 0.0025;
			guide_cal_set(&c, &k);
			for (cx = -3.0; cx <= 3.0; cx += 1.5) {
				for (cy = -3.0; cy <= 3.0; cy += 1.5) {
					double e;
					guide_cal_camera_to_mount(&c, cx, cy, &mx, &my);
					guide_cal_mount_to_camera(&c, mx, my, &bx, &by);
					e = hypot(bx - cx, by - cy);
					if (e > worst)
						worst = e;
					if (e > 1e-9)
						bad++;
				}
			}
			/* An offset along x_angle is pure +RA, which must ask for WEST. */
			guide_cal_camera_to_mount(&c, cos(k.x_angle), sin(k.x_angle), &mx, &my);
			if (fabs(mx - 1.0) > 1e-9 || fabs(my) > 1e-9)
				bad++;
		}
	}
	{
		char what[120];
		snprintf(what, sizeof(what), "camera->mount->camera round trip, 48 calibrations (worst %.1e px)", worst);
		check(bad == 0, what);
	}
}

int main(int argc, char **argv) {
	scenario_t sc[12];
	int n = 0, i;
	guide_sim_mount_cfg_t base;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) verbose = 1;
		else if (!strcmp(argv[i], "-vv")) verbose = 2;
	}

	test_transforms();

	guide_sim_mount_defaults(&base);

#define ADD(nm) (memset(&sc[n], 0, sizeof(sc[n])), sc[n].name = (nm), \
                 sc[n].sim = base, sc[n].dec_mode = GUIDE_DEC_AUTO, \
                 sc[n].angle_tol_deg = 1.0, sc[n].rate_tol = 0.03, \
                 sc[n].guide_frames = 200, sc[n].rms_max = 0.5, &sc[n++])

	{
		scenario_t *s = ADD("ideal mount: no rotation, no PE, no drift, no seeing, no backlash");
		s->sim.angle_deg = 0;
		s->sim.pe_amplitude = 0;
		s->sim.dec_drift = 0;
		s->sim.seeing = 0;
		s->sim.dec_backlash_ms = 0;
		s->angle_tol_deg = 0.2;
		s->rate_tol = 0.01;
		s->rms_max = 0.05;
	}
	{
		/* Defaults: 30 deg, 300 ms backlash, 1.6 px PE, drift, seeing. PE
		 * keeps running during calibration and bends the measured legs,
		 * exactly as it does on sky, hence the looser tolerance. */
		scenario_t *s = ADD("default sim: 30 deg, PE 1.6 px, 300 ms Dec backlash, drift, 0.1 px seeing");
		s->angle_tol_deg = 5.0;
		s->rate_tol = 0.10;
	}
	{
		scenario_t *s = ADD("rotated 135 deg with Dec mirrored (camera flipped / other side of pier)");
		s->sim.angle_deg = 135;
		s->sim.dec_flip = 1;
		s->angle_tol_deg = 5.0;
		s->rate_tol = 0.10;
	}
	{
		scenario_t *s = ADD("rotated -100 deg, 1500 ms Dec backlash, unequal rates");
		s->sim.angle_deg = -100;
		s->sim.dec_backlash_ms = 1500;
		s->sim.dec_rate = 0.0018;
		s->angle_tol_deg = 5.0;
		s->rate_tol = 0.10;
	}
	{
		scenario_t *s = ADD("Dec guide mode off: RA only, Dec drift left alone");
		s->dec_mode = GUIDE_DEC_NONE;
		s->sim.dec_drift = 0;
		s->angle_tol_deg = 5.0;
		s->rate_tol = 0.10;
	}
	{
		scenario_t *s = ADD("fast guide rate: too few calibration steps");
		s->sim.ra_rate = s->sim.dec_rate = 0.012;
		s->sim.pe_amplitude = 0;
		s->sim.seeing = 0;
		s->angle_tol_deg = 1.0;
		s->rate_tol = 0.25; /* 2 steps quantise the rate badly -- that is the point */
		s->expect_advisory = "few guide steps";
		s->guide_frames = 0;
	}
	{
		scenario_t *s = ADD("RA does not move (dead guide cable)");
		s->sim.ra_rate = 0;
		s->expect_fail = 1;
		s->expect_err = "RA Calibration Failed";
	}
	{
		scenario_t *s = ADD("Dec does not move (dead Dec motor)");
		s->sim.dec_rate = 0;
		s->sim.dec_drift = 0;
		s->expect_fail = 1;
		s->expect_err = "Backlash Clearing Failed";
	}
#undef ADD

	for (i = 0; i < n; i++)
		run(&sc[i]);

	printf("\n%s: %d check(s) failed\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
