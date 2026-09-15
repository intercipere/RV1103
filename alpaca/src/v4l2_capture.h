#ifndef V4L2_CAPTURE_H
#define V4L2_CAPTURE_H

#include <stdint.h>
#include "sensor.h"

/*
 * Integer V4L2 control access by name (as printed by `v4l2-ctl --list-ctrls`,
 * e.g. "exposure", "analogue_gain", "vertical_blanking",
 * "horizontal_blanking", "pixel_rate"). Implemented via
 * VIDIOC_QUERY_EXT_CTRL + VIDIOC_G/S_EXT_CTRLS so both plain 32-bit and
 * INTEGER64 controls (pixel_rate) work uniformly. Returns 0 on success,
 * -1 on error (control not found / ioctl failure, errno set).
 */
int v4l2_ctrl_get(const char *subdev_path, const char *name, int64_t *value);
int v4l2_ctrl_set(const char *subdev_path, const char *name, int64_t value);
int v4l2_ctrl_get_range(const char *subdev_path, const char *name,
                         int64_t *min, int64_t *max);

typedef struct {
	uint16_t *pixels; /* width*height samples, row-major; caller frees */
	int width;
	int height;
} v4l2_frame_t;

/*
 * Captures a single frame from desc->video_path using the multiplanar
 * capture API (confirmed required for the rkcif driver via
 * `v4l2-ctl --list-formats-ext`: "Video Capture Multiplanar"), unpacks it
 * via desc->unpack, and fills `out`. Returns 0 on success, -1 on error.
 */
int v4l2_capture_frame(const sensor_desc_t *desc, v4l2_frame_t *out);

#endif
