/*
 * Writes star catalogues in ASTAP's .1476 tile format (see astap_db.h for
 * the layout), so astap_db.c can read them. This is the host-side tool that
 * will turn a licensed source catalogue into the on-device one; the tests use
 * it with a synthetic sky.
 *
 * Written from the format description, not ported: there is no writer in
 * ASTAP's solver source.
 */
#ifndef CATALOG_H
#define CATALOG_H

#include <stddef.h>

typedef struct {
	double ra, dec; /* radians, J2000 */
	double mag;
} cat_star_t;

/* Sorts stars in place (by tile, magnitude, Dec) and writes all 1476 tiles,
 * empty ones included, to dir/name_RRCC.1476. Stars fainter than mag_limit
 * are dropped. Returns the number of stars written, or -1 on error. */
long cat_write(const char *dir, const char *name, const char *description,
               cat_star_t *stars, size_t n, double mag_limit);

#endif
