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
} device_state_t;

extern device_state_t g_device;

void device_state_init(void);

#endif
