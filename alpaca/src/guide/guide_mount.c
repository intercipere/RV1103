#include "guide_mount.h"

#include <stdio.h>
#include <string.h>

int guide_mount_pulse(guide_mount_t *m, guide_dir_t dir, int ms,
                      char *err, size_t errlen) {
	if (ms <= 0 || dir == GUIDE_DIR_NONE)
		return 0;
	if (!m || !m->connected || !m->pulse) {
		snprintf(err, errlen, "no mount connected");
		return -1;
	}
	return m->pulse(m->ctx, dir, ms, err, errlen);
}

void guide_mount_none(guide_mount_t *m) {
	memset(m, 0, sizeof(*m));
	m->name = "none";
}

const char *guide_dir_name(guide_dir_t d) {
	switch (d) {
	case GUIDE_NORTH: return "north";
	case GUIDE_SOUTH: return "south";
	case GUIDE_EAST:  return "east";
	case GUIDE_WEST:  return "west";
	default:          return "none";
	}
}
