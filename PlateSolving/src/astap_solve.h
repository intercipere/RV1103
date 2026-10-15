/*
 * Hinted plate solving. Port of ASTAP's solver core (unit_star_align.pas,
 * unit_astrometric_solving.pas).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Ported from ASTAP, Copyright (C) 2017-2026 by Han Kleijn, www.hnsky.org,
 * https://sourceforge.net/p/astap-program/ (hg changeset 4ea2c2f5).
 *
 * Input is a star list the caller has already extracted (on the device,
 * PHD2's star finder in alpaca/src/guide/), brightest first. Star detection,
 * binning and ASTAP's autoFOV search are not ported: a hinted solve knows
 * its image scale.
 *
 * All angles in radians unless a name says otherwise. Pixel coordinates are
 * 0-based, (0,0) at the first pixel, in whatever orientation the caller
 * uses: the fit recovers any rotation and mirror, so nothing here assumes
 * y-up or y-down. Use astap_pixel_to_radec() rather than interpreting
 * crota/cdelt signs, which follow ASTAP's FITS conventions.
 */
#ifndef ASTAP_SOLVE_H
#define ASTAP_SOLVE_H

typedef struct {
	const char *db_dir;     /* directory holding <db_name>_RRCC.1476 */
	const char *db_name;    /* e.g. "d05", or our own catalogue's name */
	int width, height;      /* image size, pixels */
	double ra_hint, dec_hint;
	double fov_deg;         /* image HEIGHT in degrees, as ASTAP's FOV */
	double radius_deg;      /* search radius around the hint. ASTAP default 20 */
	double quad_tolerance;  /* ASTAP default 0.007 */
	int max_stars;          /* ASTAP default 500 */
	int db_density;         /* catalogue's stars per deg^2 cap, 0 = none */
} astap_params_t;

/* Defaults as ASTAP's settings form ships them. */
void astap_params_default(astap_params_t *p);

typedef struct {
	int solved;
	/* Image centre and FITS-style WCS, as ASTAP writes them. */
	double ra0, dec0;
	double cdelt1, cdelt2;  /* arcsec per pixel; cdelt1 < 0 if mirrored */
	double crota1, crota2;
	int flipped;
	/* The raw solution: pixel (x,y) -> standard coordinates in arcsec
	 * around the tangent point tan_ra/tan_dec:
	 *   X = sx[0]*x + sx[1]*y + sx[2],  Y = sy[0]*x + sy[1]*y + sy[2] */
	double tan_ra, tan_dec;
	double sx[3], sy[3];
	/* Diagnostics */
	int stars_image;        /* stars used from the image */
	int quads_image;
	int nr_references;      /* matching quads kept */
	int nr_references2;     /* matching quads before the outlier filter */
	int search_steps;       /* spiral positions tried */
	double resid_rms;       /* fit residual of the matched quad centres, arcsec */
	double resid_max;
	double mag_limit;       /* faintest catalogue magnitude read */
	double search_offset;   /* hint to solution, radians */
	const char *error;      /* why it failed, NULL if solved */
} astap_solution_t;

/* stars_x/stars_y: n stars, brightest first. Returns 1 if solved. */
int astap_solve(const astap_params_t *p, const double *stars_x, const double *stars_y,
                int n, astap_solution_t *sol);

void astap_pixel_to_radec(const astap_solution_t *sol, double x, double y,
                          double *ra, double *dec);

/* Angular separation (Meeus 16.1), exposed for tests. */
double astap_ang_sep(double ra1, double dec1, double ra2, double dec2);

#endif
