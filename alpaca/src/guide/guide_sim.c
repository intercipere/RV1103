/*
 * guide_sim -- runs the ported guiding pipeline end to end, against either the
 * live camera or a simulated star field.
 *
 *   guide_sim --source camera [--host H] [--port N] [--exposure S] [--frames N]
 *   guide_sim --source sim --base <frame.u16> --width W --height H [--stars N]
 *
 * The two sources are interchangeable: the loop below cannot tell which it is
 * running on. What differs is what can be checked.
 *
 *   camera     real frames, real everything. But there is no ground truth --
 *              a real frame tells you where a star appears, never where it
 *              actually was, so the centroid can only be checked for
 *              self-consistency (does it stay put on a static scene?).
 *   sim        stars at exactly known sub-pixel positions, drifting the way a
 *              mount does, drawn over a REAL frame from this sensor so the
 *              noise, pedestal and hot pixels are genuine. This is the only
 *              configuration that can score the centroid against truth, and
 *              the only one that works in daylight or with no mount attached.
 *
 * Neither replaces the other. Use sim to know the maths is right; use camera to
 * know the maths survives this sensor.
 */

#include "guide_star.h"
#include "guide_algo.h"
#include "guide_source.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static long peak_rss_kb(void) {
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long kb = -1;
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "VmHWM: %ld kB", &kb) == 1)
			break;
	fclose(f);
	return kb;
}

static void usage(const char *prog) {
	fprintf(stderr,
	        "usage: %s --source camera|sim [options]\n"
	        "  --source  camera|sim     frame source (default sim)\n"
	        "  --base    <frame.u16>    sim: real frame used as the noise floor\n"
	        "  --width/--height N       sim: frame geometry (default 2304x1296)\n"
	        "  --stars   N              sim: how many stars to inject (default 6)\n"
	        "  --host    H              camera: alpacad address (default 127.0.0.1)\n"
	        "  --port    N              camera: alpacad port (default 11111)\n"
	        "  --exposure S             camera: seconds (default 0.5)\n"
	        "  --frames  N              frames to track (default 20)\n"
	        "  --downsample N           autofind downsample (default 4)\n"
	        "  --band    N              autofind band rows (default 128)\n",
	        prog);
}

int main(int argc, char **argv) {
	guide_source_t src;
	guide_source_kind_t kind = GUIDE_SRC_SIM;
	const char *base_path = NULL, *host = "127.0.0.1";
	int port = 11111, width = 2304, height = 1296, nstars = 6;
	int nframes = 20, downsample = 4, band = 128;
	double exposure = 0.5;
	int i, n, frame, lost = 0, err_n = 0;
	int lock_x, lock_y;
	double lock_tx = 0, lock_ty = 0;
	double err_sum = 0.0, err_max = 0.0, find_total = 0.0, acquire_total = 0.0;
	guide_star_t found[12];
	void *scratch;
	size_t scratch_len;
	guide_algo_t algo_ra, algo_dec;
	double t0, t1;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--source") && i + 1 < argc) {
			const char *s = argv[++i];
			if (!strcmp(s, "camera")) kind = GUIDE_SRC_CAMERA;
			else if (!strcmp(s, "sim")) kind = GUIDE_SRC_SIM;
			else { fprintf(stderr, "unknown source '%s'\n", s); return 2; }
		}
		else if (!strcmp(argv[i], "--base") && i + 1 < argc) base_path = argv[++i];
		else if (!strcmp(argv[i], "--width") && i + 1 < argc) width = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--height") && i + 1 < argc) height = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--stars") && i + 1 < argc) nstars = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
		else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--exposure") && i + 1 < argc) exposure = atof(argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) nframes = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--downsample") && i + 1 < argc) downsample = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--band") && i + 1 < argc) band = atoi(argv[++i]);
		else { usage(argv[0]); return 2; }
	}

	if (kind == GUIDE_SRC_CAMERA) {
		if (guide_source_open_camera(&src, host, port, exposure) != 0) {
			fprintf(stderr, "camera source: %s\n", src.err);
			return 1;
		}
	} else {
		if (guide_source_open_sim(&src, base_path, width, height, nstars) != 0) {
			fprintf(stderr, "sim source: %s\n", src.err);
			return 1;
		}
	}

	printf("source: %s", guide_source_name(&src));
	if (kind == GUIDE_SRC_CAMERA)
		printf(" (%s:%d, %.4f s exposure)", host, port, exposure);
	else
		printf(" (%d stars over %s)", src.nstars,
		       base_path ? base_path : "a synthetic pedestal");
	printf("\nframe: %dx%d, maxADU %u\n", src.width, src.height, src.max_adu);

	scratch_len = guide_star_autofind_scratch(src.width, src.height, downsample, band);
	scratch = malloc(scratch_len);
	if (!scratch) {
		fprintf(stderr, "cannot allocate %.2f MB of scratch\n", scratch_len / 1e6);
		return 1;
	}
	printf("autofind scratch: %.2f MB (downsample=%d band=%d)\n",
	       scratch_len / 1e6, downsample, band);

	/* --- acquire one frame and pick a star ---------------------------- */
	t0 = now_s();
	if (guide_source_next(&src) != 0) {
		fprintf(stderr, "\nframe acquire failed: %s\n", src.err);
		return 1;
	}
	t1 = now_s();
	printf("first frame: %.0f ms\n", (t1 - t0) * 1000.0);

	t0 = now_s();
	n = guide_star_autofind(&src.image, 15, downsample, band, scratch, scratch_len,
	                        found, (int)(sizeof(found) / sizeof(found[0])));
	t1 = now_s();
	printf("\nautofind: %d star(s) in %.0f ms\n", n, (t1 - t0) * 1000.0);
	for (i = 0; i < n; i++)
		printf("  [%d] %8.3f %8.3f  SNR %6.1f  HFD %5.2f  mass %8.0f\n",
		       i, found[i].x, found[i].y, found[i].snr, found[i].hfd, found[i].mass);

	if (n == 0) {
		printf("\nno stars found -- nothing to track\n");
		printf("peak RSS: %ld kB\n", peak_rss_kb());
		return 0;
	}

	/* Which star to track. On the simulated source it must be one of ours: a
	 * real base frame contains real bright objects too, and locking onto one
	 * would score the centroid against a truth position that does not
	 * describe it. */
	{
		int pick = 0;
		if (kind == GUIDE_SRC_SIM) {
			double best = 1e9, dxs, dys;
			int bi = -1, bj = 0, j;
			guide_source_truth(&src, &dxs, &dys);
			for (i = 0; i < n; i++) {
				for (j = 0; j < src.nstars; j++) {
					double dx = found[i].x - (src.stars[j].x + dxs);
					double dy = found[i].y - (src.stars[j].y + dys);
					double d = sqrt(dx * dx + dy * dy);
					if (d < best) { best = d; bi = i; bj = j; }
				}
			}
			if (bi < 0 || best > 3.0) {
				printf("\nnone of the injected stars was found -- cannot score\n");
				printf("peak RSS: %ld kB\n", peak_rss_kb());
				return 1;
			}
			pick = bi;
			lock_tx = src.stars[bj].x;
			lock_ty = src.stars[bj].y;
		} else {
			lock_tx = found[0].x;
			lock_ty = found[0].y;
		}
		lock_x = (int)(found[pick].x + 0.5);
		lock_y = (int)(found[pick].y + 0.5);
	}

	guide_algo_init(&algo_ra, GUIDE_ALGO_HYSTERESIS, 0.15);
	guide_algo_init(&algo_dec, GUIDE_ALGO_RESIST_SWITCH, 0.15);

	printf("\ntracking %d frames from (%.2f, %.2f)   [%s / %s]\n",
	       nframes, lock_tx, lock_ty,
	       guide_algo_name(algo_ra.kind), guide_algo_name(algo_dec.kind));
	if (kind == GUIDE_SRC_SIM)
		printf("  %-5s %9s %9s %9s %9s %9s\n",
		       "frame", "true dx", "meas dx", "true dy", "meas dy", "|err| px");
	else
		printf("  %-5s %9s %9s %7s %6s %9s\n",
		       "frame", "meas dx", "meas dy", "SNR", "HFD", "acquire");

	for (frame = 1; frame <= nframes; frame++) {
		double dx = 0, dy = 0, mdx, mdy, cra, cdec, acq;
		guide_star_t s;
		guide_star_result_t r;

		t0 = now_s();
		if (guide_source_next(&src) != 0) {
			printf("  %-5d  acquire failed: %s\n", frame, src.err);
			lost++;
			continue;
		}
		t1 = now_s();
		acq = (t1 - t0) * 1000.0;
		acquire_total += t1 - t0;
		guide_source_truth(&src, &dx, &dy);

		t0 = now_s();
		r = guide_star_find(&src.image, 15, lock_x, lock_y, 1.5, 25.0, &s);
		t1 = now_s();
		find_total += t1 - t0;

		if (r != GUIDE_STAR_OK) {
			printf("  %-5d  star lost: %s\n", frame, guide_star_result_str(s.result));
			lost++;
			continue;
		}

		mdx = s.x - lock_tx;
		mdy = s.y - lock_ty;
		cra = guide_algo_result(&algo_ra, -mdx);
		cdec = guide_algo_result(&algo_dec, -mdy);

		if (kind == GUIDE_SRC_SIM) {
			double e = sqrt((mdx - dx) * (mdx - dx) + (mdy - dy) * (mdy - dy));
			err_sum += e;
			err_n++;
			if (e > err_max)
				err_max = e;
			printf("  %-5d %9.3f %9.3f %9.3f %9.3f %9.3f   -> RA %+6.3f  Dec %+6.3f\n",
			       frame, dx, mdx, dy, mdy, e, cra, cdec);
		} else {
			err_sum += sqrt(mdx * mdx + mdy * mdy);
			err_n++;
			printf("  %-5d %9.3f %9.3f %7.1f %6.2f %7.0f ms   -> RA %+6.3f  Dec %+6.3f\n",
			       frame, mdx, mdy, s.snr, s.hfd, acq, cra, cdec);
		}
	}

	if (kind == GUIDE_SRC_SIM)
		printf("\ncentroid error vs ground truth: mean %.4f px, worst %.4f px"
		       " over %d frames (%d lost)\n",
		       err_n ? err_sum / err_n : 0.0, err_max, err_n, lost);
	else
		printf("\nno ground truth on a live camera. Star wandered a mean %.4f px"
		       " from its first position over %d frames (%d lost)"
		       " -- on a static scene that is the measurement noise floor,\n"
		       "   which is the only centroid check a real frame can give.\n",
		       err_n ? err_sum / err_n : 0.0, err_n, lost);

	printf("guide_star_find: %.3f ms per frame\n",
	       nframes ? find_total * 1000.0 / nframes : 0.0);
	printf("frame acquire:   %.1f ms per frame\n",
	       nframes ? acquire_total * 1000.0 / nframes : 0.0);
	printf("peak RSS: %ld kB\n", peak_rss_kb());

	free(scratch);
	guide_source_close(&src);
	return 0;
}
