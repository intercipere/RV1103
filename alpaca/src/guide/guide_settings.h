#ifndef GUIDE_SETTINGS_H
#define GUIDE_SETTINGS_H

#include <stddef.h>

/*
 * Guiding settings, stored under PHD2's own configuration keys
 * ("/frame/focalLength", "/scope/CalibrationDuration", ...) so the settings
 * registry generated from PHD2's source (see Guiding/UI_PORTING_PLAN.md) maps
 * onto them one to one, and a future multi-profile store needs no migration.
 *
 * Only settings this device actually implements are accepted: each has an
 * entry in the table in guide_settings.c with PHD2's type, range and default.
 * Values are persisted as "key=value" lines; on the board that file is on
 * /userdata, since /tmp is a RAM disk.
 *
 * Thread-safe: the loop thread reads, HTTP threads write.
 */

/* Loads the file at path (missing is fine: defaults apply). */
void guide_settings_init(const char *path);

double guide_settings_get(const char *key);

/* Validates against the table and persists. Returns 0, or -1 with err set
 * (unknown key, not a number, out of range). */
int guide_settings_set(const char *key, const char *value, char *err, size_t errlen);

/* {"<key>":{"value":v,"default":d,"min":lo,"max":hi}, ...} for every
 * implemented setting. Returns bytes written. */
size_t guide_settings_json(char *buf, size_t cap);

/* Bumped on every change, so the loop can pick changes up cheaply. */
unsigned int guide_settings_generation(void);

#endif
