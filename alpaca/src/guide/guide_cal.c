/*
 * Calibration and the camera/mount transforms, ported from PHD2 (BSD 3-clause,
 * Copyright (c) 2013-2019 Open PHD Guiding development team): scope.cpp
 * (BeginCalibration, UpdateCalibrationState, MoveAxis, SanityCheckCalibration)
 * and mount.cpp (SetCalibration, the two Transform* functions, MoveOffset).
 *
 * Structure, constants and the order of every test are PHD2's. Where a line
 * here looks odd -- the post-increment in "steps++ > MAX_CALIBRATION_STEPS",
 * dX/dY/dist being zeroed in one state and read in the next -- it is because
 * PHD2 does it, and changing it would change the behaviour a decade of real
 * mounts has been tuned against. See guide_cal.h for what was left out.
 */
#include "guide_cal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* scope.cpp */
static const int DefaultCalibrationDuration = 750;
static const int DefaultMaxDecDuration = 2500;
static const int DefaultMaxRaDuration = 2500;
static const int MAX_CALIBRATION_STEPS = 60;
static const int CAL_ALERT_MINSTEPS = 4;
static const double CAL_ALERT_ORTHOGONALITY_TOLERANCE = 12.5; /* degrees */
static const int MAX_NUDGES = 3;
static const double NUDGE_TOLERANCE = 2.0;
/* scope.h, blConstants */
enum { BL_BACKLASH_MIN_COUNT = 3, BL_MAX_CLEARING_TIME = 60000, BL_MIN_CLEARING_DISTANCE = 3 };
/* calstep_dialog.h, guider_multistar.cpp */
static const int DEFAULT_DISTANCE = 25;
static const int DEFAULT_SEARCH_REGION = 15;

/* phd.h */
#define ROUND(x) (int)floor((x) + 0.5)
#define DIV_ROUND_UP(x, y) (((x) + (y) - 1) / (y))

/* image_math.h */
double guide_norm_angle(double val) {
	const double start = -M_PI, range = 2.0 * M_PI;
	return val - floor((val - start) / range) * range;
}
static double degrees(double r) { return r * 180.0 / M_PI; }
static double radians(double d) { return d * M_PI / 180.0; }

/* point.h. Note the argument order, which the angle conventions below depend
 * on: a.dX(b) is a.X - b.X, so a.Angle(b) is the direction of the vector FROM
 * b TO a. */
static guide_pt_t pt(double x, double y) { guide_pt_t p = { x, y, 1 }; return p; }
static double pt_dx(guide_pt_t a, guide_pt_t b) { return a.x - b.x; }
static double pt_dy(guide_pt_t a, guide_pt_t b) { return a.y - b.y; }
static double pt_dist(guide_pt_t a, guide_pt_t b) { return hypot(pt_dx(a, b), pt_dy(a, b)); }
static double pt_angle(guide_pt_t a, guide_pt_t b) {
	double dx = pt_dx(a, b), dy = pt_dy(a, b);
	return (dx != 0 || dy != 0) ? atan2(dy, dx) : 0.0;
}

static void schedule(guide_cal_t *c, guide_dir_t dir, int ms) {
	c->move_dir = dir;
	c->move_ms = ms;
}

void guide_cal_clear(guide_cal_t *c) {
	c->calibrated = 0;
	c->state = GUIDE_CAL_CLEARED;
}

void guide_cal_init(guide_cal_t *c) {
	memset(c, 0, sizeof(*c));
	c->duration_ms = DefaultCalibrationDuration;
	c->distance_px = DEFAULT_DISTANCE;
	c->max_ra_ms = DefaultMaxRaDuration;
	c->max_dec_ms = DefaultMaxDecDuration;
	c->max_move_px = DEFAULT_SEARCH_REGION;
	c->fast_recenter = 1;
	c->assume_orthogonal = 0;
	c->dec_mode = GUIDE_DEC_AUTO;
	c->move_dir = GUIDE_DIR_NONE;
	guide_cal_clear(c);
}

void guide_cal_begin(guide_cal_t *c, const guide_mount_t *m, double x, double y) {
	(void)m; /* CheckCalibrationDuration needs guide rates and binning */
	guide_cal_clear(c);
	c->steps = 0;
	c->initial = pt(x, y);
	c->starting.valid = 0;
	c->state = GUIDE_CAL_GO_WEST;
	c->work.ra_steps = 0;
	c->work.dec_steps = 0;
	c->move_dir = GUIDE_DIR_NONE;
	c->move_ms = 0;
	c->status[0] = c->err[0] = c->advisory[0] = '\0';
}

void guide_cal_set(guide_cal_t *c, const guide_calibration_t *cal) {
	c->cal = *cal;
	/* "the angles are more difficult because we have to turn yAngle into a
	 * yError" */
	c->y_angle_error = guide_norm_angle(cal->x_angle - cal->y_angle + M_PI / 2.);
	c->calibrated = 1;
}

int guide_cal_is_calibrated(const guide_cal_t *c) {
	if (!c->calibrated)
		return 0;
	if (c->dec_mode == GUIDE_DEC_NONE)
		return 1;
	return c->cal.y_rate != GUIDE_CAL_RATE_UNCALIBRATED;
}

/* Scope::SanityCheckCalibration, the two checks that need neither declination
 * nor a stored previous calibration. Advisory only, as in PHD2. */
static void sanity_check(guide_cal_t *c) {
	int xs = c->cal.ra_steps, ys = c->cal.dec_steps;
	if (xs < CAL_ALERT_MINSTEPS || (ys < CAL_ALERT_MINSTEPS && ys > 0)) {
		snprintf(c->advisory, sizeof(c->advisory),
		         "Calibration completed but few guide steps were used (RA %d, "
		         "Dec %d), so accuracy is questionable", xs, ys);
		return;
	}
	if (c->cal.y_rate != GUIDE_CAL_RATE_UNCALIBRATED) {
		double non_ortho = degrees(fabs(fabs(guide_norm_angle(
		    c->cal.x_angle - c->cal.y_angle)) - M_PI / 2.));
		if (non_ortho > CAL_ALERT_ORTHOGONALITY_TOLERANCE)
			snprintf(c->advisory, sizeof(c->advisory),
			         "Calibration completed but RA/Dec axis angles are "
			         "questionable (%.1f deg from orthogonal) and guiding may "
			         "be impaired", non_ortho);
	}
}

/* MountCoords() in scope.cpp: the same transform as camera_to_mount, but with
 * explicit angles, used while the calibration is still being built. */
static double mount_coords_y(double cx, double cy, double xa, double ya) {
	double hyp = hypot(cx, cy);
	double theta = (cx != 0 || cy != 0) ? atan2(cy, cx) : 0.0;
	double y_err = guide_norm_angle((xa - ya) + M_PI / 2);
	return hyp * sin(theta - (xa + y_err));
}

static int fail(guide_cal_t *c, const char *msg) {
	snprintf(c->err, sizeof(c->err), "%s", msg);
	c->move_dir = GUIDE_DIR_NONE;
	c->move_ms = 0;
	guide_cal_clear(c);
	return GUIDE_CAL_FAILED;
}

int guide_cal_update(guide_cal_t *c, const guide_mount_t *m, double x, double y) {
	guide_pt_t cur = pt(x, y);
	double dX, dY, dist, dist_crit;
	double bl_delta, bl_cum_delta, nudge_amt, ndx, ndy, cos_theta, theta;
	double south_moved, north_moved, south_angle;
	int can_pulse = m ? m->can_pulse_guide : 0;

	c->move_dir = GUIDE_DIR_NONE;
	c->move_ms = 0;
	c->status[0] = '\0';

	if (!c->starting.valid)
		c->starting = cur;

	dX = pt_dx(c->starting, cur);
	dY = pt_dy(c->starting, cur);
	dist = pt_dist(c->starting, cur);
	dist_crit = c->distance_px;

	switch (c->state) {
	case GUIDE_CAL_CLEARED:
		return fail(c, "calibration was not started");

	case GUIDE_CAL_GO_WEST:
		if (dist < dist_crit) {
			if (c->steps++ > MAX_CALIBRATION_STEPS)
				return fail(c, "RA Calibration Failed: star did not move enough");
			snprintf(c->status, sizeof(c->status), "West step %3d, dist=%4.1f",
			         c->steps, dist);
			schedule(c, GUIDE_WEST, c->duration_ms);
			break;
		}

		/* West calibration complete */
		c->work.x_angle = pt_angle(c->starting, cur);
		c->work.x_rate = dist / (c->steps * c->duration_ms);
		c->work.ra_steps = c->steps;

		/* For GO_EAST recenter_remaining holds the total remaining duration.
		 * Choose the largest pulse that will not lose the guide star or
		 * exceed the max pulse. */
		c->recenter_remaining = c->steps * c->duration_ms;
		if (c->fast_recenter) {
			c->recenter_duration = (int)floor((double)c->max_move_px / c->work.x_rate);
			if (c->recenter_duration > c->max_ra_ms)
				c->recenter_duration = c->max_ra_ms;
			if (c->recenter_duration < c->duration_ms)
				c->recenter_duration = c->duration_ms;
		} else {
			c->recenter_duration = c->duration_ms;
		}
		c->steps = DIV_ROUND_UP(c->recenter_remaining, c->recenter_duration);
		c->state = GUIDE_CAL_GO_EAST;
		c->east_start = cur;
		/* fall through */

	case GUIDE_CAL_GO_EAST:
		if (c->recenter_remaining > 0) {
			int duration = c->recenter_duration;
			if (duration > c->recenter_remaining)
				duration = c->recenter_remaining;
			snprintf(c->status, sizeof(c->status), "East step %3d, dist=%4.1f",
			         c->steps, dist);
			c->recenter_remaining -= duration;
			--c->steps;
			c->last = cur;
			schedule(c, GUIDE_EAST, duration);
			break;
		}

		/* If not pulse-guiding, check for an obvious guide cable problem:
		 * want a significant east move that retraces the west vector to
		 * within 30 degrees. */
		if (!can_pulse) {
			double east_moved = pt_dist(c->east_start, cur);
			double west_moved = pt_dist(c->starting, c->east_start);
			double east_angle = pt_angle(c->east_start, cur);
			if (fabs(east_moved) < 0.25 * west_moved ||
			    fabs(guide_norm_angle(east_angle - (c->work.x_angle + M_PI))) > radians(30))
				snprintf(c->advisory, sizeof(c->advisory),
				         "Little or no east movement was measured, so guiding "
				         "will probably be impaired. Check the guide cable.");
		}

		/* set up for clearing backlash */
		c->steps = 0;
		dist = dX = dY = 0.0;
		c->starting = cur;

		if (c->dec_mode == GUIDE_DEC_NONE) {
			c->state = GUIDE_CAL_COMPLETE;
			/* an arbitrary angle perpendicular to x; the rate marks the
			 * absence of Dec calibration */
			c->work.y_angle = guide_norm_angle(c->work.x_angle + M_PI / 2.);
			c->work.y_rate = GUIDE_CAL_RATE_UNCALIBRATED;
			c->work.dec_steps = 0;
			/* No pulse, and not complete yet: PHD2 breaks here and installs
			 * the calibration on the NEXT frame, from the COMPLETE case. */
			snprintf(c->status, sizeof(c->status),
			         "Skipping Dec calibration (Dec guide mode off)");
			break;
		}

		c->state = GUIDE_CAL_CLEAR_BACKLASH;
		c->bl_marker = cur;
		c->bl_expected_step = c->work.x_rate * c->duration_ms * 0.6;
		/* PHD2 scales this by DecSpeed/RASpeed when the mount reports guide
		 * rates; neither ST-4 nor the simulator does. */
		c->bl_max_clearing_pulses = BL_MAX_CLEARING_TIME / c->duration_ms;
		if (c->bl_max_clearing_pulses < 8)
			c->bl_max_clearing_pulses = 8;
		c->bl_last_cum_distance = 0;
		c->bl_accepted_moves = 0;
		/* fall through */

	case GUIDE_CAL_CLEAR_BACKLASH:
		bl_delta = pt_dist(c->bl_marker, cur);
		bl_cum_delta = dist;

		/* Want to see the mount moving north for 3 moves of at least the
		 * expected distance without any direction reversals. */
		if (c->steps == 0) {
			/* get things moving with the first clearing pulse */
			schedule(c, GUIDE_NORTH, c->duration_ms);
			c->steps = 1;
			snprintf(c->status, sizeof(c->status), "Clearing backlash step 1");
			break;
		}

		if (bl_delta >= c->bl_expected_step) {
			if (c->bl_accepted_moves == 0 || bl_cum_delta > c->bl_last_cum_distance)
				c->bl_accepted_moves++;
			else
				c->bl_accepted_moves = 0; /* reset on a direction reversal */
		} else if (bl_cum_delta < c->bl_last_cum_distance) {
			c->bl_accepted_moves = 0;
		}

		if (c->bl_accepted_moves < BL_BACKLASH_MIN_COUNT) {
			if (c->steps < c->bl_max_clearing_pulses && bl_cum_delta < dist_crit) {
				/* attempts left, and not yet moved the full distance */
				schedule(c, GUIDE_NORTH, c->duration_ms);
				c->steps++;
				c->bl_marker = cur;
				c->bl_last_cum_distance = bl_cum_delta;
				snprintf(c->status, sizeof(c->status),
				         "Clearing backlash step %3d", c->steps);
				break;
			}
			/* used up all attempts -- might be ok or not */
			if (bl_cum_delta >= BL_MIN_CLEARING_DISTANCE) {
				c->steps = 0;
				c->starting = cur;
				dX = 0;
				dY = 0;
				dist = 0;
			} else {
				return fail(c, "Backlash Clearing Failed: star did not move enough");
			}
		} else {
			/* Got the 3 moves. The last clearing move was big enough, so
			 * count it as the first north calibration step. */
			c->steps = 1;
			c->starting = c->bl_marker;
			dX = pt_dx(c->bl_marker, cur);
			dY = pt_dy(c->bl_marker, cur);
			dist = pt_dist(c->bl_marker, cur);
		}

		c->bl_distance_moved = pt_dist(c->bl_marker, c->initial); /* nudge limit */
		c->state = GUIDE_CAL_GO_NORTH;
		/* fall through */

	case GUIDE_CAL_GO_NORTH:
		if (dist < dist_crit) {
			if (c->steps++ > MAX_CALIBRATION_STEPS)
				return fail(c, "DEC Calibration Failed: star did not move enough");
			snprintf(c->status, sizeof(c->status), "North step %3d, dist=%4.1f",
			         c->steps, dist);
			schedule(c, GUIDE_NORTH, c->duration_ms);
			break;
		}

		/* Reversed from the RA calculation: that one calibrated WEST, but the
		 * angle is really relative to EAST. */
		if (c->assume_orthogonal) {
			double a1 = guide_norm_angle(c->work.x_angle + M_PI / 2.);
			double a2 = guide_norm_angle(c->work.x_angle - M_PI / 2.);
			double ya = pt_angle(cur, c->starting);
			double dec_dist;
			c->work.y_angle = fabs(guide_norm_angle(a1 - ya)) <
			                  fabs(guide_norm_angle(a2 - ya)) ? a1 : a2;
			dec_dist = dist * cos(ya - c->work.y_angle);
			c->work.y_rate = dec_dist / (c->steps * c->duration_ms);
		} else {
			c->work.y_angle = pt_angle(cur, c->starting);
			c->work.y_rate = dist / (c->steps * c->duration_ms);
		}
		c->work.dec_steps = c->steps;

		c->recenter_remaining = c->steps * c->duration_ms;
		if (c->fast_recenter) {
			c->recenter_duration =
			    (int)floor(0.8 * (double)c->max_move_px / c->work.y_rate);
			if (c->recenter_duration > c->max_dec_ms)
				c->recenter_duration = c->max_dec_ms;
			if (c->recenter_duration < c->duration_ms)
				c->recenter_duration = c->duration_ms;
		} else {
			c->recenter_duration = c->duration_ms;
		}
		c->steps = DIV_ROUND_UP(c->recenter_remaining, c->recenter_duration);
		c->state = GUIDE_CAL_GO_SOUTH;
		c->south_start = cur;
		/* fall through */

	case GUIDE_CAL_GO_SOUTH:
		if (c->recenter_remaining > 0) {
			int duration = c->recenter_duration;
			if (duration > c->recenter_remaining)
				duration = c->recenter_remaining;
			snprintf(c->status, sizeof(c->status), "South step %3d, dist=%4.1f",
			         c->steps, dist);
			c->recenter_remaining -= duration;
			--c->steps;
			schedule(c, GUIDE_SOUTH, duration);
			break;
		}

		/* PHD2 checks here for little or no south movement, then only logs
		 * the result ("Omitted calibration alert") -- Dec backlash makes it
		 * fire on healthy mounts. Not surfaced here either; the values are
		 * kept so a guide log can carry them later. */
		south_moved = pt_dist(c->south_start, cur);
		north_moved = pt_dist(c->starting, c->south_start);
		south_angle = pt_angle(cur, c->south_start);
		(void)south_moved;
		(void)north_moved;
		(void)south_angle;

		c->last = cur;
		/* Direction cosines of the north moves, used to make sure any nudging
		 * goes the right way. */
		c->north_dircos_x = pt_dx(c->initial, c->south_start) /
		                    pt_dist(c->initial, c->south_start);
		c->north_dircos_y = pt_dy(c->initial, c->south_start) /
		                    pt_dist(c->initial, c->south_start);
		/* magnitude and sign convention of the south moves already made */
		c->total_south_amt = mount_coords_y(c->south_start.x - c->last.x,
		                                    c->south_start.y - c->last.y,
		                                    c->work.x_angle, c->work.y_angle);
		c->state = GUIDE_CAL_NUDGE_SOUTH;
		c->steps = 0;
		/* fall through */

	case GUIDE_CAL_NUDGE_SOUTH:
		/* Nudge further south to get within 2 px N/S of the starting point;
		 * at most 3 times, and not at all if starting too far away. */
		nudge_amt = pt_dist(cur, c->initial);
		ndx = pt_dx(cur, c->initial) / nudge_amt;
		ndy = pt_dy(cur, c->initial) / nudge_amt;
		/* the nudge and the north vector should be roughly opposite */
		cos_theta = ndx * c->north_dircos_x + ndy * c->north_dircos_y;
		theta = acos(cos_theta);
		if (fabs(fabs(theta) * 180.0 / M_PI - 180.0) < 40.0) {
			if (c->steps <= MAX_NUDGES && nudge_amt > NUDGE_TOLERANCE &&
			    nudge_amt < dist_crit + c->bl_distance_moved) {
				double dec_amt = mount_coords_y(cur.x - c->initial.x,
				                                cur.y - c->initial.y,
				                                c->work.x_angle, c->work.y_angle);
				if (dec_amt * c->total_south_amt > 0.0) {
					int pulse;
					dec_amt = fabs(dec_amt);
					if (dec_amt > (double)c->max_move_px)
						dec_amt = (double)c->max_move_px;
					pulse = (int)floor(dec_amt / c->work.y_rate);
					/* conservative: the size that pushed us north */
					if (pulse > c->duration_ms)
						pulse = c->duration_ms;
					++c->steps;
					snprintf(c->status, sizeof(c->status), "Nudge South %3d",
					         c->steps);
					schedule(c, GUIDE_SOUTH, pulse);
					break;
				}
			}
		}
		c->state = GUIDE_CAL_COMPLETE;
		/* fall through */

	case GUIDE_CAL_COMPLETE:
		guide_cal_set(c, &c->work);
		sanity_check(c);
		snprintf(c->status, sizeof(c->status), "Calibration complete");
		return GUIDE_CAL_DONE;
	}

	(void)dX;
	(void)dY;
	return GUIDE_CAL_CONTINUE;
}

void guide_cal_camera_to_mount(const guide_cal_t *c, double cx, double cy,
                               double *mx, double *my) {
	double hyp = hypot(cx, cy);
	double theta = (cx != 0 || cy != 0) ? atan2(cy, cx) : 0.0;
	/* x angle: RA axis rotation vs camera X, positive is CW from x;
	 * y uses the orthogonality error */
	double xa = theta - c->cal.x_angle;
	double ya = theta - (c->cal.x_angle + c->y_angle_error);
	*mx = cos(xa) * hyp;
	*my = sin(ya) * hyp;
}

void guide_cal_mount_to_camera(const guide_cal_t *c, double mx, double my,
                               double *cx, double *cy) {
	double hyp = hypot(mx, my);
	double theta = (mx != 0 || my != 0) ? atan2(my, mx) : 0.0;
	double xa;
	if (fabs(c->y_angle_error) > M_PI / 2.)
		theta = -theta;
	xa = theta + c->cal.x_angle;
	*cx = cos(xa) * hyp;
	*cy = sin(xa) * hyp;
}

void guide_cal_guide_step(const guide_cal_t *c, guide_algo_t *ra,
                          guide_algo_t *dec, double cam_dx, double cam_dy,
                          guide_move_t *mv) {
	double xd, yd;
	int xms, yms;

	memset(mv, 0, sizeof(*mv));
	guide_cal_camera_to_mount(c, cam_dx, cam_dy, &mv->mount_x, &mv->mount_y);

	xd = guide_algo_result(ra, mv->mount_x);
	yd = guide_algo_result(dec, mv->mount_y);
	mv->dist_x = xd;
	mv->dist_y = yd;

	mv->dir_x = xd > 0.0 ? GUIDE_WEST : GUIDE_EAST;
	mv->dir_y = yd > 0.0 ? GUIDE_SOUTH : GUIDE_NORTH;

	/* Scope::MoveAxis limits for a guide step */
	xms = ROUND(fabs(xd / c->cal.x_rate));
	if (xms > c->max_ra_ms) {
		xms = c->max_ra_ms;
		mv->limited_x = 1;
	}

	yms = ROUND(fabs(yd / c->cal.y_rate));
	if (c->dec_mode == GUIDE_DEC_NONE ||
	    (mv->dir_y == GUIDE_SOUTH && c->dec_mode == GUIDE_DEC_NORTH) ||
	    (mv->dir_y == GUIDE_NORTH && c->dec_mode == GUIDE_DEC_SOUTH))
		yms = 0;
	if (yms > c->max_dec_ms) {
		yms = c->max_dec_ms;
		mv->limited_y = 1;
	}

	mv->ms_x = xms;
	mv->ms_y = yms;
}

const char *guide_cal_state_name(guide_cal_state_t s) {
	switch (s) {
	case GUIDE_CAL_GO_WEST:        return "west";
	case GUIDE_CAL_GO_EAST:        return "east";
	case GUIDE_CAL_CLEAR_BACKLASH: return "backlash";
	case GUIDE_CAL_GO_NORTH:       return "north";
	case GUIDE_CAL_GO_SOUTH:       return "south";
	case GUIDE_CAL_NUDGE_SOUTH:    return "nudge";
	case GUIDE_CAL_COMPLETE:       return "complete";
	default:                       return "cleared";
	}
}
