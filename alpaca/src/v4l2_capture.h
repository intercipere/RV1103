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
 * Opens desc->video_path, allocates a single mmap'd buffer, and starts
 * streaming -- once, kept open across exposures instead of reopened per
 * capture (see alpaca/README.md, "Persistent V4L2 device"). Must be called
 * once before any v4l2_capture_frame() call. Returns 0 on success, -1 on
 * error.
 */
int v4l2_capture_init(const sensor_desc_t *desc);

/*
 * Stops streaming and releases the buffer/fd. Not called anywhere in the
 * daemon's normal lifecycle (it's killed and restarted wholesale via
 * S60alpacad); provided for symmetry and for test_capture.c.
 */
void v4l2_capture_shutdown(void);

/*
 * Captures one frame reflecting the caller's most recent v4l2_ctrl_set()
 * calls, discarding whatever the driver already had queued from before
 * those calls (necessary because streaming never stops between exposures --
 * see the .c file). Pass extra_settle=1 if this call just changed
 * vertical_blanking; a plain gain/exposure change doesn't need it (see
 * alpaca/README.md, "Persistent V4L2 device"). Returns 0 on success, -1 on
 * error (including if v4l2_capture_init() wasn't called or failed).
 */
int v4l2_capture_frame(v4l2_frame_t *out, int extra_settle);

#endif
