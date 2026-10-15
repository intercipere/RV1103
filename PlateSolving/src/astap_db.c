/*
 * Reader for ASTAP's .1476 star database format. Port of unit_star_database.pas.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Ported from ASTAP, Copyright (C) 2017-2026 by Han Kleijn, www.hnsky.org,
 * https://sourceforge.net/p/astap-program/ (hg changeset 4ea2c2f5).
 *
 * Deviations from the Pascal, none of which change results:
 * - area_and_boundaries1476 is one 36-branch if/else chain in ASTAP, one
 *   branch per ring with the cell count and the running area offset written
 *   out. Here it is the same arithmetic driven by the ring table.
 * - open_database grows its cache to the file size and then fills it in
 *   blocks as records are read; here the tile is read whole on open. A tile
 *   of our V<12 catalogue is ~10-30 kB.
 * - The .290 and wide-field (.001) formats are not ported.
 */
#include "astap_db.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const int astap_db_ring_cells[ASTAP_DB_RINGS] = {
	1, 3, 9, 15, 21, 27, 33, 38, 43, 48, 52, 56, 60, 63, 65, 67, 68, 69,
	69, 68, 67, 65, 63, 60, 56, 52, 48, 43, 38, 33, 27, 21, 15, 9, 3, 1,
};

#define D2R (M_PI / 180)
const double astap_db_dec_boundaries[ASTAP_DB_RINGS + 1] = {
	-90 * D2R, -87.42857143 * D2R, -82.28571429 * D2R, -77.14285714 * D2R,
	-72 * D2R, -66.85714286 * D2R, -61.71428571 * D2R, -56.57142857 * D2R,
	-51.42857143 * D2R, -46.28571429 * D2R, -41.14285714 * D2R, -36 * D2R,
	-30.85714286 * D2R, -25.71428571 * D2R, -20.57142857 * D2R, -15.42857143 * D2R,
	-10.28571429 * D2R, -5.142857143 * D2R,
	0 * D2R,
	5.142857143 * D2R, 10.28571429 * D2R, 15.42857143 * D2R, 20.57142857 * D2R,
	25.71428571 * D2R, 30.85714286 * D2R, 36 * D2R, 41.14285714 * D2R,
	46.28571429 * D2R, 51.42857143 * D2R, 56.57142857 * D2R, 61.71428571 * D2R,
	66.85714286 * D2R, 72 * D2R, 77.14285714 * D2R, 82.28571429 * D2R,
	87.42857143 * D2R, 90 * D2R,
};

void astap_db_init(astap_db_t *db, const char *dir, const char *name) {
	memset(db, 0, sizeof(*db));
	snprintf(db->prefix, sizeof(db->prefix), "%s/%s", dir, name);
	db->old_area = 9999999;
}

void astap_db_close(astap_db_t *db) {
	free(db->cache);
	db->cache = NULL;
	db->cache_cap = 0;
	db->open = 0;
}

void astap_db_area_name(int area, char out[5]) {
	int ring = 0, base = 0;
	while (ring < ASTAP_DB_RINGS - 1 && area > base + astap_db_ring_cells[ring]) {
		base += astap_db_ring_cells[ring];
		ring++;
	}
	snprintf(out, 5, "%02d%02d", ring + 1, area - base);
}

static double frac_part(double x) { return x - trunc(x); }

void astap_db_area_and_boundaries(double ra1, double dec1, int *area_nr,
                                  double *spaceE, double *spaceW,
                                  double *spaceN, double *spaceS) {
	const double *b = astap_db_dec_boundaries;
	double cos_dec1 = cos(dec1);
	int r, base, n;
	double rot;

	if (dec1 > b[35]) { /* celestial north pole area */
		*area_nr = 1476;
		*spaceS = dec1 - b[35];
		*spaceN = b[36] - b[35]; /* minimum, could go beyond the pole */
		*spaceW = M_PI * 2;
		*spaceE = M_PI * 2;
		return;
	}
	/* rings 34 down to 1: the first ring whose lower boundary is below dec1 */
	base = 0;
	for (r = 0; r < 35; r++)
		base += astap_db_ring_cells[r];
	for (r = 34; r >= 1; r--) {
		base -= astap_db_ring_cells[r];
		if (dec1 > b[r]) {
			n = astap_db_ring_cells[r];
			rot = ra1 * n / (2 * M_PI);
			*area_nr = base + 1 + (int)trunc(rot);
			*spaceS = dec1 - b[r];
			*spaceN = b[r + 1] - dec1;
			*spaceW = (M_PI * 2 / n) * frac_part(rot) * cos_dec1; /* RA decreases west */
			*spaceE = (M_PI * 2 / n) * (1 - frac_part(rot)) * cos_dec1;
			return;
		}
	}
	*area_nr = 1; /* celestial south pole area */
	*spaceS = b[1] - b[0]; /* minimum, could go beyond the pole */
	*spaceN = b[1] - dec1;
	*spaceW = M_PI * 2;
	*spaceE = M_PI * 2;
}

static double dmin(double a, double b) { return a < b ? a : b; }

void astap_db_find_areas(double ra1, double dec1, double fov, int area[4], double frac[4]) {
	double ra_cornerWN, ra_cornerEN, ra_cornerWS, ra_cornerES, dec_cornerN, dec_cornerS;
	double fov_half, sE, sW, sN, sS;
	int i, j;

	fov = dmin(fov, 5.142857 * M_PI / 180); /* should never happen but this crop is preferred */
	fov_half = fov / 2;

	dec_cornerN = dec1 + fov_half; /* above +pi/2 doesn't matter since it is all area 1476 */
	dec_cornerS = dec1 - fov_half;
	ra_cornerWN = ra1 - fov_half / cos(dec_cornerN); if (ra_cornerWN < 0) ra_cornerWN += 2 * M_PI;
	ra_cornerEN = ra1 + fov_half / cos(dec_cornerN); if (ra_cornerEN >= 2 * M_PI) ra_cornerEN -= 2 * M_PI;
	ra_cornerWS = ra1 - fov_half / cos(dec_cornerS); if (ra_cornerWS < 0) ra_cornerWS += 2 * M_PI;
	ra_cornerES = ra1 + fov_half / cos(dec_cornerS); if (ra_cornerES >= 2 * M_PI) ra_cornerES -= 2 * M_PI;

	astap_db_area_and_boundaries(ra_cornerEN, dec_cornerN, &area[0], &sE, &sW, &sN, &sS);
	frac[0] = dmin(sW, fov) * dmin(sS, fov) / (fov * fov);
	astap_db_area_and_boundaries(ra_cornerWN, dec_cornerN, &area[1], &sE, &sW, &sN, &sS);
	frac[1] = dmin(sE, fov) * dmin(sS, fov) / (fov * fov);
	astap_db_area_and_boundaries(ra_cornerES, dec_cornerS, &area[2], &sE, &sW, &sN, &sS);
	frac[2] = dmin(sW, fov) * dmin(sN, fov) / (fov * fov);
	astap_db_area_and_boundaries(ra_cornerWS, dec_cornerS, &area[3], &sE, &sW, &sN, &sS);
	frac[3] = dmin(sE, fov) * dmin(sN, fov) / (fov * fov);

	/* same order as ASTAP: 2-1, 3-1, 4-1, 3-2, 4-2, 4-3 */
	for (i = 0; i < 4; i++)
		for (j = i + 1; j < 4; j++)
			if (area[j] == area[i]) { area[j] = 0; frac[j] = 0; }
	for (i = 0; i < 4; i++)
		if (frac[i] < 0.01) { area[i] = 0; frac[i] = 0; } /* too small, ignore */
}

int astap_db_open(astap_db_t *db, double telescope_dec, int area) {
	db->cos_telescope_dec = cos(telescope_dec); /* here to save CPU time */

	if (area != db->old_area || !db->open) {
		char name[5], path[600];
		unsigned char header[ASTAP_DB_HEADER];
		FILE *f;
		long size;

		db->open = 0;
		astap_db_area_name(area, name);
		snprintf(path, sizeof(path), "%s_%s.1476", db->prefix, name);
		f = fopen(path, "rb");
		if (!f)
			return -1;
		if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
		    fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < ASTAP_DB_HEADER ||
		    fseek(f, ASTAP_DB_HEADER, SEEK_SET) != 0) {
			fclose(f);
			return -1;
		}
		db->record_size = header[109] == ' ' ? 11 : header[109];
		db->version = header[108];
		if (db->record_size != 5 && db->record_size != 6) {
			fclose(f); /* only 5 and 6 are ported, as in ASTAP's stripped reader */
			return -1;
		}
		db->cache_size = (size_t)size - ASTAP_DB_HEADER;
		if (db->cache_size > db->cache_cap) {
			unsigned char *p = realloc(db->cache, db->cache_size);
			if (!p) { fclose(f); return -1; }
			db->cache = p;
			db->cache_cap = db->cache_size;
		}
		if (fread(db->cache, 1, db->cache_size, f) != db->cache_size) {
			fclose(f);
			return -1;
		}
		fclose(f);
		db->open = 1;
		db->old_area = area;
	}
	db->cache_position = 0;
	return 0;
}

int astap_db_read(astap_db_t *db, double telescope_ra, double telescope_dec,
                  double field_diameter, double *ra2, double *dec2, double *mag2) {
	double delta_ra;
	double ra = 0, dec = 0;
	int header_record;

	do {
		do {
			const unsigned char *p;
			long ra_raw;

			if (db->cache_position + (size_t)db->record_size > db->cache_size)
				return 0; /* no more data in this file */
			p = db->cache + db->cache_position;
			db->cache_position += db->record_size;

			header_record = 0;
			ra_raw = p[0] | (p[1] << 8) | ((long)p[2] << 16);
			if (ra_raw == 0xFFFFFF) { /* special magnitude record */
				db->mag2 = p[4] - 16; /* magnitude shifted 16 to make Sirius positive */
				db->dec9_storage = p[3] - 128;
				header_record = 1;
			} else {
				ra = ra_raw * (M_PI * 2 / ((256 * 256 * 256) - 1));
				dec = ((db->dec9_storage * 65536L) + (p[4] << 8) + p[3]) *
				      (M_PI * 0.5 / ((128 * 256 * 256) - 1));
			}
		} while (header_record);
		delta_ra = fabs(ra - telescope_ra);
		if (delta_ra > M_PI) delta_ra = M_PI * 2 - delta_ra;
	} while (!(delta_ra * db->cos_telescope_dec < field_diameter / 2 &&
	           fabs(dec - telescope_dec) < field_diameter / 2));

	*ra2 = ra;
	*dec2 = dec;
	*mag2 = db->mag2;
	return 1;
}
