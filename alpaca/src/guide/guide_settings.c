#include "guide_settings.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
	const char *key;  /* PHD2's config key, verbatim */
	int is_int;
	double def, min, max;
	double value;
} setting_t;

/*
 * The settings this device implements. Keys, types and ranges are PHD2's;
 * where the default differs from PHD2's, the reason is given.
 */
static setting_t g_table[] = {
	/* PHD2: myframe.cpp, Profile "/frame/focalLength", int mm. PHD2's default
	 * is "unknown" until the profile wizard asks; this camera is built into a
	 * known 182 mm guide scope, so that is the default. */
	{ "/frame/focalLength", 1, 182, 0, 10000, 182 },
};
#define NSETTINGS (sizeof(g_table) / sizeof(g_table[0]))

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_path[256];
static unsigned int g_gen;

static setting_t *find(const char *key) {
	size_t i;
	for (i = 0; i < NSETTINGS; i++)
		if (!strcmp(g_table[i].key, key))
			return &g_table[i];
	return NULL;
}

static int parse(const setting_t *s, const char *value, double *out, char *err, size_t errlen) {
	char *end;
	double v;
	errno = 0;
	v = strtod(value, &end);
	if (errno || end == value || *end) {
		snprintf(err, errlen, "%s: not a number", s->key);
		return -1;
	}
	if (s->is_int && v != (double)(long)v) {
		snprintf(err, errlen, "%s: must be a whole number", s->key);
		return -1;
	}
	if (v < s->min || v > s->max) {
		snprintf(err, errlen, "%s: must be %g..%g", s->key, s->min, s->max);
		return -1;
	}
	*out = v;
	return 0;
}

/* Caller holds g_lock. Written to a temp file, fsync'd and renamed, so a
 * power cut (this board is bus-powered) leaves the old file or the new one,
 * never half of either. */
static void save_locked(void) {
	char tmp[300];
	FILE *f;
	size_t i;
	if (!g_path[0])
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
	f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "[settings] cannot write %s: %s\n", tmp, strerror(errno));
		return;
	}
	for (i = 0; i < NSETTINGS; i++)
		if (g_table[i].value != g_table[i].def)
			fprintf(f, "%s=%.17g\n", g_table[i].key, g_table[i].value);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	if (rename(tmp, g_path) != 0)
		fprintf(stderr, "[settings] rename %s: %s\n", g_path, strerror(errno));
}

void guide_settings_init(const char *path) {
	FILE *f;
	char line[512];

	pthread_mutex_lock(&g_lock);
	snprintf(g_path, sizeof(g_path), "%s", path ? path : "");
	f = g_path[0] ? fopen(g_path, "r") : NULL;
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '='), *nl = strchr(line, '\n');
		setting_t *s;
		double v;
		char err[128];
		if (nl) *nl = '\0';
		if (!eq) continue;
		*eq = '\0';
		s = find(line);
		/* An unknown or invalid line is dropped, not fatal: a setting may
		 * have been removed, or a newer build may have written it. */
		if (s && parse(s, eq + 1, &v, err, sizeof(err)) == 0)
			s->value = v;
		else
			fprintf(stderr, "[settings] ignoring %s\n", line);
	}
	if (f)
		fclose(f);
	g_gen++;
	pthread_mutex_unlock(&g_lock);
}

double guide_settings_get(const char *key) {
	setting_t *s;
	double v = 0.0;
	pthread_mutex_lock(&g_lock);
	s = find(key);
	if (s)
		v = s->value;
	pthread_mutex_unlock(&g_lock);
	if (!s)
		fprintf(stderr, "[settings] unknown key %s\n", key);
	return v;
}

int guide_settings_set(const char *key, const char *value, char *err, size_t errlen) {
	setting_t *s;
	double v;
	pthread_mutex_lock(&g_lock);
	s = find(key);
	if (!s) {
		pthread_mutex_unlock(&g_lock);
		snprintf(err, errlen, "%s: not an implemented setting", key);
		return -1;
	}
	if (parse(s, value, &v, err, errlen) != 0) {
		pthread_mutex_unlock(&g_lock);
		return -1;
	}
	if (v != s->value) {
		s->value = v;
		g_gen++;
		save_locked();
	}
	pthread_mutex_unlock(&g_lock);
	return 0;
}

size_t guide_settings_json(char *buf, size_t cap) {
	size_t i, n = 0;
	pthread_mutex_lock(&g_lock);
	n += (size_t)snprintf(buf + n, cap - n, "{");
	for (i = 0; i < NSETTINGS && n < cap; i++)
		n += (size_t)snprintf(buf + n, cap - n,
		    "%s\"%s\":{\"value\":%.17g,\"default\":%.17g,\"min\":%.17g,\"max\":%.17g,\"int\":%s}",
		    i ? "," : "", g_table[i].key, g_table[i].value, g_table[i].def,
		    g_table[i].min, g_table[i].max, g_table[i].is_int ? "true" : "false");
	if (n < cap)
		n += (size_t)snprintf(buf + n, cap - n, "}");
	pthread_mutex_unlock(&g_lock);
	return n < cap ? n : cap;
}

unsigned int guide_settings_generation(void) {
	unsigned int g;
	pthread_mutex_lock(&g_lock);
	g = g_gen;
	pthread_mutex_unlock(&g_lock);
	return g;
}
