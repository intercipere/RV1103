#ifndef GUIDE_CAL_H
#define GUIDE_CAL_H

#include "guide_algo.h"
#include "guide_mount.h"

/*
 * Calibration and the camera-to-mount transform, ported from PHD2 (BSD
 * 3-clause, Copyright (c) 2013-2019 Open PHD Guiding development team):
 *
 *   guide_cal_begin / guide_cal_update   Scope::BeginCalibration and
 *                                        Scope::UpdateCalibrationState
 *   guide_cal_set                        Mount::SetCalibration
 *   guide_cal_camera_to_mount            Mount::TransformCameraCoordinatesToMountCoordinates
 *   guide_cal_mount_to_camera            Mount::TransformMountCoordinatesToCameraCoordinates
 *   guide_cal_guide_step                 Mount::MoveOffset + Scope::MoveAxis
 *
 * Calibration pulses the mount west, then east back to the start, clears Dec
 * backlash, then north and south, and nudges back toward where it began --
 * measuring after every pulse how far and in which direction the star moved.
 * The result is two angles and two rates: which way each mount axis points in
 * the camera frame, and how many pixels a millisecond of pulse moves the star.
 * Guiding needs both: the angles turn a pixel error into an RA/Dec error, the
 * rates turn that into a pulse length.
 *
 * Constants, thresholds and the state sequence are PHD2's, unchanged. What was
 * stripped is the wx shell: alerts, the guide log, the event server, and the
 * config store. What was NOT ported, because this mount interface cannot
 * supply it yet: RA/Dec guide parity (needs the mount to report coordinates),
 * declination compensation and meridian-flip handling (need declination and
 * side of pier). ST-4 cannot report any of these; an LX200 backend can, and
 * those are the pieces to port next when one exists.
 */

#define GUIDE_CAL_RATE_UNCALIBRATED 123e4 /* PHD2's CALIBRATION_RATE_UNCALIBRATED */

typedef enum {
	GUIDE_DEC_NONE = 0, /* PHD2 DEC_GUIDE_MODE, same values */
	GUIDE_DEC_AUTO,
	GUIDE_DEC_NORTH,
	GUIDE_DEC_SOUTH,
} guide_dec_mode_t;

typedef enum {
	GUIDE_CAL_CLEARED = 0,
	GUIDE_CAL_GO_WEST,
	GUIDE_CAL_GO_EAST,
	GUIDE_CAL_CLEAR_BACKLASH,
	GUIDE_CAL_GO_NORTH,
	GUIDE_CAL_GO_SOUTH,
	GUIDE_CAL_NUDGE_SOUTH,
	GUIDE_CAL_COMPLETE,
} guide_cal_state_t;

typedef struct {
	double x, y;
	int valid;
} guide_pt_t;

/* PHD2's Calibration struct, minus the fields this mount interface cannot
 * fill (parity, declination, pier side, rotator angle, binning). */
typedef struct {
	double x_angle; /* radians, camera frame */
	double y_angle;
	double x_rate;  /* px per ms of pulse */
	double y_rate;  /* GUIDE_CAL_RATE_UNCALIBRATED when Dec was skipped */
	int ra_steps, dec_steps;
} guide_calibration_t;

typedef struct {
	/* --- settings (PHD2 defaults set by guide_cal_init) --------------- */
	int duration_ms;       /* calibration step: 750 */
	int distance_px;       /* how far each axis must move: 25 */
	int max_ra_ms;         /* largest guide pulse: 2500 */
	int max_dec_ms;        /* 2500 */
	int max_move_px;       /* guider search region: 15 */
	int fast_recenter;     /* recenter in big pulses after each leg: on */
	int assume_orthogonal; /* off */
	guide_dec_mode_t dec_mode; /* auto */

	/* --- the calibration in force (Mount::m_cal and friends) ---------- */
	int calibrated;
	guide_calibration_t cal;
	double y_angle_error; /* Mount::m_yAngleError: orthogonality error */

	/* --- calibration in progress (Scope's m_calibration* fields) ------ */
	guide_cal_state_t state;
	guide_calibration_t work;
	int steps;
	int recenter_remaining, recenter_duration;
	guide_pt_t initial;  /* star position when calibration began */
	guide_pt_t starting; /* start of the current measured leg */
	guide_pt_t south_start, east_start, last;
	double total_south_amt;
	double north_dircos_x, north_dircos_y;
	guide_pt_t bl_marker;
	double bl_expected_step, bl_last_cum_distance, bl_distance_moved;
	int bl_accepted_moves, bl_max_clearing_pulses;

	/* --- result of the last guide_cal_update() ------------------------- */
	guide_dir_t move_dir; /* the pulse to send before the next frame */
	int move_ms;
	char status[96];      /* "West step 3, dist=5.6" */
	char err[160];        /* why calibration failed */
	char advisory[200];   /* completed, but something looked wrong */
} guide_cal_t;

/* Sets PHD2's defaults and clears any calibration. */
void guide_cal_init(guide_cal_t *c);

/* PHD2's ClearCalibration: drops the calibration in force. */
void guide_cal_clear(guide_cal_t *c);

/*
 * Starts calibrating from the star's current position. The first
 * guide_cal_update() call, on the next frame, sends the first west pulse.
 */
void guide_cal_begin(guide_cal_t *c, const guide_mount_t *m, double x, double y);

#define GUIDE_CAL_CONTINUE 0  /* send move_dir/move_ms, then take a frame */
#define GUIDE_CAL_DONE     1  /* calibrated; guiding can start */
#define GUIDE_CAL_FAILED  (-1) /* see err; the calibration was cleared */

/*
 * One calibration step, called once per frame with the star's measured
 * position. A frame on which the star was lost must NOT be passed in -- PHD2
 * skips it and "blunders on", re-measuring on the next one.
 */
int guide_cal_update(guide_cal_t *c, const guide_mount_t *m, double x, double y);

/* PHD2 Scope::IsCalibrated: a calibration exists, and if Dec is being guided
 * it includes Dec. */
int guide_cal_is_calibrated(const guide_cal_t *c);

/* Installs a calibration (PHD2 Mount::SetCalibration): derives the
 * orthogonality error the transforms use and marks the mount calibrated. */
void guide_cal_set(guide_cal_t *c, const guide_calibration_t *cal);

/* Camera pixel offset -> mount-axis offset in pixels, and back. */
void guide_cal_camera_to_mount(const guide_cal_t *c, double cx, double cy,
                               double *mx, double *my);
void guide_cal_mount_to_camera(const guide_cal_t *c, double mx, double my,
                               double *cx, double *cy);

typedef struct {
	double mount_x, mount_y; /* measured error in mount axes, px */
	double dist_x, dist_y;   /* what the algorithms asked for, px */
	guide_dir_t dir_x, dir_y;
	int ms_x, ms_y;          /* pulse lengths after limits and Dec mode */
	int limited_x, limited_y;
} guide_move_t;

/*
 * One guide step (PHD2 Mount::MoveOffset with MOVEOPTS_GUIDE_STEP, plus the
 * limits from Scope::MoveAxis). cam_dx/cam_dy is the star's offset from the
 * lock position, camera pixels, star minus lock -- PHD2's cameraOfs.
 *
 * Computes the pulses; does not send them. The caller sends RA then Dec, as
 * PHD2 does.
 */
void guide_cal_guide_step(const guide_cal_t *c, guide_algo_t *ra,
                          guide_algo_t *dec, double cam_dx, double cam_dy,
                          guide_move_t *mv);

const char *guide_cal_state_name(guide_cal_state_t s);

/* PHD2's norm_angle(): radians, into [-pi, pi). */
double guide_norm_angle(double a);

#endif
