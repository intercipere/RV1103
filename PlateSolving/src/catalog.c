/*
 * Writes star catalogues in ASTAP's .1476 tile format. See catalog.h.
 */
#include "catalog.h"
#include "astap_db.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	int area;
	int m10;      /* magnitude * 10, rounded: the reader's 0.1 mag groups */
	long dec_raw; /* 24-bit two's complement Dec */
	long ra_raw;  /* 24-bit RA, never 0xFFFFFF (the header marker) */
} rec_t;

static int cmp_rec(const void *a, const void *b) {
	const rec_t *x = a, *y = b;
	if (x->area != y->area) return x->area - y->area;
	if (x->m10 != y->m10) return x->m10 - y->m10;
	return (x->dec_raw > y->dec_raw) - (x->dec_raw < y->dec_raw);
}

static int write_tile(const char *dir, const char *name, const char *description,
                      int area, const rec_t *r, size_t n) {
	char fname[5], path[600];
	unsigned char header[ASTAP_DB_HEADER];
	FILE *f;
	size_t i;
	int cur_m10 = 9999, cur_dec9 = 9999;

	astap_db_area_name(area, fname);
	snprintf(path, sizeof(path), "%s/%s_%s.1476", dir, name, fname);
	f = fopen(path, "wb");
	if (!f) return -1;

	memset(header, ' ', sizeof(header));
	memcpy(header, description, strlen(description) < 100 ? strlen(description) : 100);
	header[108] = 1; /* version */
	header[109] = 5; /* record size */
	fwrite(header, 1, sizeof(header), f);

	for (i = 0; i < n; i++) {
		unsigned char rec[5];
		int dec9 = (int)(r[i].dec_raw >> 16); /* arithmetic shift: -128..127 */
		if (r[i].m10 != cur_m10 || dec9 != cur_dec9) {
			/* header record: RA 0xFFFFFF, Dec high byte + 128, magnitude*10 + 16 */
			rec[0] = rec[1] = rec[2] = 0xFF;
			rec[3] = (unsigned char)(dec9 + 128);
			rec[4] = (unsigned char)(r[i].m10 + 16);
			fwrite(rec, 1, 5, f);
			cur_m10 = r[i].m10;
			cur_dec9 = dec9;
		}
		rec[0] = r[i].ra_raw & 0xFF;
		rec[1] = (r[i].ra_raw >> 8) & 0xFF;
		rec[2] = (r[i].ra_raw >> 16) & 0xFF;
		rec[3] = r[i].dec_raw & 0xFF;
		rec[4] = (r[i].dec_raw >> 8) & 0xFF;
		fwrite(rec, 1, 5, f);
	}
	return fclose(f) == 0 ? 0 : -1;
}

long cat_write(const char *dir, const char *name, const char *description,
               cat_star_t *stars, size_t n, double mag_limit) {
	rec_t *r = malloc((n ? n : 1) * sizeof(rec_t));
	size_t i, m = 0, start;
	int area;

	if (!r) return -1;
	for (i = 0; i < n; i++) {
		double sE, sW, sN, sS, ra = stars[i].ra;
		int m10 = (int)lround(stars[i].mag * 10);
		if (stars[i].mag > mag_limit) continue;
		if (m10 < -16) m10 = -16;
		if (m10 > 239) continue; /* not representable */
		ra = fmod(ra, 2 * M_PI);
		if (ra < 0) ra += 2 * M_PI;
		astap_db_area_and_boundaries(ra, stars[i].dec, &r[m].area, &sE, &sW, &sN, &sS);
		r[m].m10 = m10;
		r[m].ra_raw = lround(ra / (2 * M_PI) * 16777215.0);
		if (r[m].ra_raw >= 0xFFFFFF) r[m].ra_raw = 0; /* 2*pi is 0 */
		r[m].dec_raw = lround(stars[i].dec / (M_PI / 2) * (128.0 * 65536 - 1));
		m++;
	}
	qsort(r, m, sizeof(rec_t), cmp_rec);

	start = 0;
	for (area = 1; area <= ASTAP_DB_TILES; area++) {
		size_t end = start;
		while (end < m && r[end].area == area) end++;
		if (write_tile(dir, name, description, area, r + start, end - start) < 0) {
			free(r);
			return -1;
		}
		start = end;
	}
	free(r);
	return (long)m;
}
