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
 * The sky is divided into 1476 tiles of ~5.14 deg, one file each, named
 * <name>_RRCC.1476 (ring RR 01..36, cell CC). Each file is a 110-byte header
 * (byte 108 version, byte 109 record size) then 5- or 6-byte records sorted
 * bright to faint in 0.1 mag steps. Within a magnitude step the stars are
 * sorted in Dec; a header record (RA = 0xFFFFFF) precedes every run sharing
 * one magnitude and one Dec high byte. See the comments in
 * unit_star_database.pas for the full description.
 */
#ifndef ASTAP_DB_H
#define ASTAP_DB_H

#include <stddef.h>

#define ASTAP_DB_TILES 1476
#define ASTAP_DB_RINGS 36
#define ASTAP_DB_HEADER 110

/* ASTAP: "warning FOV should be less the database tiles dimensions" */
#define ASTAP_DB_MAX_FOV_DEG 5.142857143

typedef struct {
	char prefix[512];     /* "<dir>/<name>", files are prefix_RRCC.1476 */
	int open;
	int old_area;
	unsigned char *cache; /* the whole tile, minus the header */
	size_t cache_size;
	size_t cache_cap;
	size_t cache_position;
	int record_size;
	int version;
	int dec9_storage;
	double mag2;          /* magnitude*10 of the stars being read */
	double cos_telescope_dec;
} astap_db_t;

/* Number of cells in each declination ring, south to north. */
extern const int astap_db_ring_cells[ASTAP_DB_RINGS];
/* Ring boundaries in radians, [0] = -90 deg ... [36] = +90 deg. */
extern const double astap_db_dec_boundaries[ASTAP_DB_RINGS + 1];

void astap_db_init(astap_db_t *db, const char *dir, const char *name);
void astap_db_close(astap_db_t *db);

/* File name part "RRCC" for an area number 1..1476. */
void astap_db_area_name(int area, char out[5]);

/* area_and_boundaries1476 */
void astap_db_area_and_boundaries(double ra1, double dec1, int *area_nr,
                                  double *spaceE, double *spaceW,
                                  double *spaceN, double *spaceS);

/* find_areas (1476 branch): up to four tiles covering a square field of
 * fov radians centred on ra1/dec1, with the fraction each contributes.
 * An area of 0 means unused. */
void astap_db_find_areas(double ra1, double dec1, double fov, int area[4], double frac[4]);

/* open_database. Returns 0 on success, -1 if the file cannot be read. */
int astap_db_open(astap_db_t *db, double telescope_dec, int area);

/* readdatabase1476: next star of the open tile inside the square window of
 * field_diameter radians around telescope_ra/dec. Returns 1 and the star, or
 * 0 when the tile is exhausted. mag2 is magnitude*10. */
int astap_db_read(astap_db_t *db, double telescope_ra, double telescope_dec,
                  double field_diameter, double *ra2, double *dec2, double *mag2);

#endif
