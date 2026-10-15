#ifndef GUIDE_MOUNT_H
#define GUIDE_MOUNT_H

#include <stddef.h>

/*
 * The mount, as far as guiding needs one: something that can be told to pulse
 * an axis for a number of milliseconds.
 *
 * Write the pulse driver once and expose it twice (see Guiding/ROADMAP.md):
 * our own guide loop calls guide_mount_pulse() directly, and a later Camera
 * PulseGuide endpoint can call the same function for a PC-side client. The
 * backends behind it are interchangeable:
 *
 *   simulated   moves the simulated star field (guide_source.c). The only
 *               backend that exists today, and the only way to exercise
 *               calibration and closed-loop guiding without a mount and sky.
 *   ST-4        four GPIO lines. Not written yet.
 *   LX200       :Mg<dir><ms># over a UART, e.g. OpenAstroTech. Not written yet.
 *
 * Directions are PHD2's GUIDE_DIRECTION values, so the ported calibration code
 * keeps its exact meaning: NORTH = Dec+, SOUTH = Dec-, EAST = RA-, WEST = RA+.
 */
typedef enum {
	GUIDE_DIR_NONE = -1,
	GUIDE_NORTH = 0,
	GUIDE_SOUTH,
	GUIDE_EAST,
	GUIDE_WEST,
} guide_dir_t;

typedef struct guide_mount {
	const char *name;
	int connected;

	/*
	 * PHD2's CanPulseGuide(): true for a mount that is pulsed through its own
	 * driver (ASCOM PulseGuide, LX200 :Mg), false for ST-4 relays. It only
	 * decides which calibration advisories run -- PHD2 checks for "little or
	 * no east movement" only on ST-4, where a bad guide cable is the usual
	 * cause.
	 */
	int can_pulse_guide;

	void *ctx;

	/* Blocks for the duration of the pulse, the way an ST-4 relay does. The
	 * next exposure must not start until the mount has finished moving, or
	 * the frame measures a star still in motion. Returns 0 on success. */
	int (*pulse)(void *ctx, guide_dir_t dir, int ms, char *err, size_t errlen);
} guide_mount_t;

/* Pulses one axis. A duration of zero or less is a no-op that succeeds, which
 * is what PHD2's MoveAxis does when a limit or the Dec guide mode zeroes it. */
int guide_mount_pulse(guide_mount_t *m, guide_dir_t dir, int ms,
                      char *err, size_t errlen);

/* A mount that is not there: connected = 0. Guiding still measures, but has
 * nothing to calibrate against and nothing to send corrections to. */
void guide_mount_none(guide_mount_t *m);

const char *guide_dir_name(guide_dir_t d);

#endif
