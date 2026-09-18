#include "guide_algo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Ported from PHD2 src/guide_algorithm_{hysteresis,lowpass,resistswitch}.cpp
 * (BSD 3-clause, Copyright (c) 2013-2019 Open PHD Guiding development team).
 * Default parameter values are PHD2's. */

const char *guide_algo_name(guide_algo_kind_t kind) {
	switch (kind) {
	case GUIDE_ALGO_HYSTERESIS:    return "hysteresis";
	case GUIDE_ALGO_LOWPASS:       return "lowpass";
	case GUIDE_ALGO_RESIST_SWITCH: return "resist switch";
	default:                       return "identity";
	}
}

void guide_algo_reset(guide_algo_t *a) {
	a->last_move = 0.0;
	a->current_side = 0;
	memset(a->history, 0, sizeof(a->history));
}

void guide_algo_init(guide_algo_t *a, guide_algo_kind_t kind, double min_move) {
	memset(a, 0, sizeof(*a));
	a->kind = kind;
	a->min_move = min_move;

	/* PHD2 keeps these per algorithm, not globally -- aggression in
	 * particular differs: hysteresis damps to 0.7 because it is already
	 * blending in the previous move, while resist-switch applies the full
	 * correction once it has decided to move at all. Using one value for both
	 * quietly under-corrects on the resist-switch axis. */
	switch (kind) {
	case GUIDE_ALGO_HYSTERESIS:
		a->hysteresis = 0.10; /* DefaultHysteresis */
		a->aggression = 0.70; /* DefaultAggression */
		break;
	case GUIDE_ALGO_LOWPASS:
		a->slope_weight = 5.0; /* DefaultSlopeWeight */
		break;
	case GUIDE_ALGO_RESIST_SWITCH:
		a->aggression = 1.00; /* DefaultAggression */
		a->fast_switch = 1;   /* PHD2 defaults fastSwitch to true */
		break;
	default:
		a->aggression = 1.00;
		break;
	}
	guide_algo_reset(a);
}

/* Slide the window left and append, which is PHD2's Add()+RemoveAt(0). The
 * window is always full, never growing. */
static void history_push(guide_algo_t *a, double v) {
	memmove(&a->history[0], &a->history[1],
	        sizeof(a->history[0]) * (GUIDE_ALGO_HISTORY - 1));
	a->history[GUIDE_ALGO_HISTORY - 1] = v;
}

static int sgn(double x) { return x > 0.0 ? 1 : (x < 0.0 ? -1 : 0); }

double guide_algo_result(guide_algo_t *a, double input) {
	double out = 0.0;

	switch (a->kind) {
	case GUIDE_ALGO_HYSTERESIS:
		/* Blend this error with the previous correction, then scale. The
		 * blend is what keeps a mount with backlash from oscillating. */
		out = (1.0 - a->hysteresis) * input + a->hysteresis * a->last_move;
		out *= a->aggression;
		if (fabs(input) < a->min_move)
			out = 0.0;
		a->last_move = out;
		break;

	case GUIDE_ALGO_LOWPASS: {
		/* Correct the *trend* rather than the sample: fit a line through
		 * recent history and act on its slope, so seeing noise averages
		 * out and only real drift survives. */
		double median, slope = 0.0;
		double sorted[GUIDE_ALGO_HISTORY];
		const int n = GUIDE_ALGO_HISTORY;
		int i, j;

		history_push(a, input);
		memcpy(sorted, a->history, sizeof(sorted));
		for (i = 1; i < n; i++) {
			double key = sorted[i];
			for (j = i - 1; j >= 0 && sorted[j] > key; j--)
				sorted[j + 1] = sorted[j];
			sorted[j + 1] = key;
		}
		median = n & 1 ? sorted[n / 2]
		               : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);

		{
			/* least-squares slope against sample index -- PHD2 fits against
			 * its own monotonic time base, which is the same thing for a
			 * uniformly sampled window. */
			double sx = 0, sy = 0, sxy = 0, sxx = 0, dn = (double)n;
			for (i = 0; i < n; i++) {
				sx += i;
				sy += a->history[i];
				sxy += (double)i * a->history[i];
				sxx += (double)i * i;
			}
			if (dn * sxx - sx * sx != 0.0)
				slope = (dn * sxy - sx * sy) / (dn * sxx - sx * sx);
		}

		out = median + a->slope_weight * slope;
		if (fabs(input) < a->min_move)
			out = 0.0;
		else if (fabs(out) > fabs(input))
			out = input; /* never correct further than the measurement */
		break;
	}

	case GUIDE_ALGO_RESIST_SWITCH: {
		/*
		 * Ported from PHD2 guide_algorithm_resistswitch.cpp. An earlier
		 * version here was a hand-written reversal counter that shared only
		 * the name -- this is the actual algorithm.
		 *
		 * It will not reverse direction on one sample, or even on several,
		 * unless the recent history agrees on the new direction *and* the
		 * error is getting worse rather than better. On a Dec axis with
		 * backlash a spurious reversal costs a whole unwind, so the bar is
		 * deliberately high.
		 */
		int dec_history = 0, i;
		double oldest = 0.0, newest = 0.0;

		history_push(a, input);

		if (fabs(input) < a->min_move) {
			out = 0.0;
			break;
		}

		/* A big excursion is real, not noise: drop the accumulated history
		 * and let the new direction take effect immediately. */
		if (a->fast_switch) {
			double thresh = 3.0 * a->min_move;
			if (sgn(input) != a->current_side && fabs(input) > thresh) {
				a->current_side = 0;
				for (i = 0; i < GUIDE_ALGO_HISTORY - 3; i++)
					a->history[i] = 0.0;
				for (; i < GUIDE_ALGO_HISTORY; i++)
					a->history[i] = input;
			}
		}

		for (i = 0; i < GUIDE_ALGO_HISTORY; i++)
			if (fabs(a->history[i]) > a->min_move)
				dec_history += sgn(a->history[i]);

		if (a->current_side == 0 ||
		    sgn(a->current_side) == -sgn(dec_history)) {
			if (abs(dec_history) < 3) {
				out = 0.0; /* not a compelling enough consensus */
				break;
			}
			for (i = 0; i < 3; i++) {
				oldest += a->history[i];
				newest += a->history[GUIDE_ALGO_HISTORY - (i + 1)];
			}
			if (fabs(newest) <= fabs(oldest)) {
				out = 0.0; /* drifting back on its own; leave it alone */
				break;
			}
			a->current_side = sgn(dec_history);
		}

		if (a->current_side != sgn(input)) {
			out = 0.0; /* overshot -- veto rather than chase it back */
			break;
		}

		out = input * a->aggression;
		break;
	}

	default:
		out = fabs(input) < a->min_move ? 0.0 : input;
		break;
	}

	return out;
}
