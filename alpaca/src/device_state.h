#ifndef DEVICE_STATE_H
#define DEVICE_STATE_H

#include <pthread.h>
#include <time.h>
#include "sensor.h"
#include "v4l2_capture.h"

/* ASCOM CameraStates enum (ICameraV3). */
typedef enum {
	CAM_IDLE = 0,
	CAM_WAITING = 1,
	CAM_EXPOSING = 2,
	CAM_READING = 3,
	CAM_DOWNLOAD = 4,
	CAM_ERROR = 5,
} camera_state_t;

typedef struct {
	pthread_mutex_t lock;

	int connected;
	const sensor_desc_t *sensor;

	camera_state_t state;
	int image_ready;
	v4l2_frame_t last_frame; /* valid iff image_ready */
	double last_exposure_duration_s;
	time_t last_exposure_start;

	long gain; /* raw analogue_gain V4L2 control units, 128 = 1x */

	/* StartX/StartY/NumX/NumY -- ASCOM's subframe members. Real capture
	 * doesn't crop (V4L2 cropping isn't implemented), so these are
	 * accepted/stored/reported for client compatibility -- many Alpaca
	 * clients (N.I.N.A. included) PUT NumX/NumY to the full frame size
	 * before every exposure, even a full-frame one, and abort if that PUT
	 * isn't implemented. StartX/StartY other than 0 or NumX/NumY other
	 * than the full frame size are accepted but silently not honored --
	 * the returned imagearray is always the full sensor frame. */
	long start_x, start_y, num_x, num_y;
} device_state_t;

extern device_state_t g_device;

void device_state_init(void);

#endif
