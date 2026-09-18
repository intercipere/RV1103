#ifndef GUIDE_ALGO_H
#define GUIDE_ALGO_H

/*
 * Guide algorithms, ported from PHD2 (BSD 3-clause, Copyright (c) 2013-2019
 * Open PHD Guiding development team).
 *
 * These take a measured axis error in pixels and return the correction to
 * apply, also in pixels; converting that to a pulse duration is the mount's
 * business, not the algorithm's. PHD2 runs one instance per axis, and the two
 * axes are routinely given different algorithms and parameters -- Dec has no
 * periodic error to chase but does have backlash, so it is usually guided more
 * gently than RA.
 *
 * The whole point of taking these from PHD2 rather than writing a PID loop is
 * that they encode a decade of real-sky behaviour: hysteresis damps the
 * overcorrection that a naive proportional response produces on a mount with
 * any backlash, min_move suppresses chasing seeing noise, and resist-switch
 * refuses to reverse direction on a single sample.
 */

typedef enum {
	GUIDE_ALGO_HYSTERESIS = 0,
	GUIDE_ALGO_LOWPASS,
	GUIDE_ALGO_RESIST_SWITCH,
	GUIDE_ALGO_IDENTITY,
} guide_algo_kind_t;

#define GUIDE_ALGO_HISTORY 10

typedef struct {
	guide_algo_kind_t kind;

	/* min_move is shared by every algorithm: an error smaller than this is
	 * seeing, not drift, and correcting it injects noise into the mount. */
	double min_move;

	/* hysteresis */
	double hysteresis;   /* 0..1, weight of the previous correction */
	double aggression;   /* 0..1+, scales the result */
	double last_move;

	/* lowpass: slope of a linear fit through recent history */
	double slope_weight;

	/* resist switch */
	int current_side;      /* -1, 0, +1: the direction currently allowed */
	int fast_switch;       /* force a switch on a large excursion */

	/* Fixed-length sliding window, pre-filled with zeros exactly as PHD2's
	 * reset() does -- the algorithms read the whole window from the first
	 * call, so an empty one would behave differently for the first ten
	 * frames. */
	double history[GUIDE_ALGO_HISTORY];
} guide_algo_t;

/* Sets PHD2's defaults for the given algorithm. */
void guide_algo_init(guide_algo_t *a, guide_algo_kind_t kind, double min_move);

/* One guiding step: input is the measured error in pixels, return value is the
 * correction to apply in pixels. */
double guide_algo_result(guide_algo_t *a, double input);

void guide_algo_reset(guide_algo_t *a);

const char *guide_algo_name(guide_algo_kind_t kind);

#endif
