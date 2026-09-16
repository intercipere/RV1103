#ifndef SENSOR_H
#define SENSOR_H

#include <stdint.h>
#include <stddef.h>

/* BAYER_NONE marks a monochrome sensor -- SensorType=Monochrome, no
 * BayerOffsetX/Y in the Alpaca camera API. Everything else is a Bayer
 * colour sensor read out through the same raw V4L2 path. */
typedef enum {
	BAYER_NONE = 0,
	BAYER_RGGB,
	BAYER_GRBG,
	BAYER_GBRG,
	BAYER_BGGR,
} bayer_pattern_t;

/*
 * Unpacks one frame of packed raw sensor data into 16-bit samples.
 * `stride_bytes` is the byte stride of one row as delivered by V4L2
 * (may be wider than the packed pixel data itself); `out` must hold
 * width*height uint16_t values in row-major order.
 */
typedef void (*unpack_fn_t)(const uint8_t *packed, int stride_bytes,
                             int width, int height, uint16_t *out);

typedef struct {
	const char *match_substr; /* matched against the subdev sysfs "name" file */
	const char *display_name; /* Alpaca SensorName */

	const char *subdev_path;
	const char *video_path;

	int width;
	int height;
	uint32_t v4l2_fourcc; /* as accepted by VIDIOC_S_FMT on video_path */

	/* BAYER_NONE => monochrome. The Alpaca BayerOffsetX/Y values are derived
	 * from this by bayer_offsets() in camera_api.c, not stored separately --
	 * keeping both was how they drifted apart and swapped red/blue. */
	bayer_pattern_t bayer;

	int max_adu;             /* e.g. 1023 for a 10-bit sensor */
	double pixel_size_um_x;
	double pixel_size_um_y;

	/* Row time in microseconds = (width + horizontal_blanking) / pixel_rate.
	 * Used to convert Alpaca's Duration-in-seconds to the sensor's
	 * exposure-in-rows control. Populated at detect time from the live
	 * V4L2 control values (see sensor_detect), not hardcoded, since
	 * horizontal_blanking/pixel_rate are read-only controls that could
	 * differ per sensor revision or clock config. */
	double row_time_us;

	/* V4L2 control names on subdev_path (see v4l2_capture.h for get/set) */
	const char *ctrl_exposure;
	const char *ctrl_gain;
	const char *ctrl_vblank;

	/* Writes ASCOM ImageBytes wire order -- out[x * height + y], X outer --
	 * NOT row-major. The daemon's only consumer of a frame is the wire
	 * format, so the transpose is folded into the unpack rather than paid as
	 * a second pass. See unpack.h. */
	unpack_fn_t unpack;
} sensor_desc_t;

/*
 * Detects the connected sensor by matching match_substr against
 * /sys/class/video4linux/<subdev_basename>/name, and fills in row_time_us
 * from the live pixel_rate/horizontal_blanking controls. Falls back to a
 * generic, clearly-marked descriptor (logged to stderr) if no known sensor
 * matches, rather than aborting -- an unrecognised sensor should degrade to
 * "best effort", not crash the server.
 */
const sensor_desc_t *sensor_detect(void);

#endif
