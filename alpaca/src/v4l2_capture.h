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

/* NOTE: `pixels` is in ASCOM ImageBytes wire order -- pixels[x * height + y],
 * X outer -- not row-major. desc->unpack produces it that way directly; see
 * sensor.h and unpack.h for why. */

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
 * see the .c file). Returns 0 on success, -1 on error (including if
 * v4l2_capture_init() wasn't called or failed).
 *
 * frame_period_s is the caller's estimate of the sensor's *current* frame
 * period (height + vertical_blanking, in seconds). Each internal DQBUF wait
 * blocks for up to one frame period, so this is what the DQBUF timeout is
 * derived from; a hardcoded timeout silently caps the longest exposure the
 * daemon can return regardless of what the sensor supports. Pass 0.0 when
 * the period is unknown or short -- the timeout has a floor.
 *
 * settle_ref_ns is v4l2_capture_now_ns() sampled immediately *after* the
 * caller's control writes completed. Frames that started before it carry the
 * previous exposure's settings and are discarded. Pass 0 to disable the check
 * and take the next frame (startup warm-up, standalone test tools).
 */
int v4l2_capture_frame(v4l2_frame_t *out, double frame_period_s,
                       uint64_t settle_ref_ns);

/*
 * Current time on the same clock rkcif stamps buffers with, for settle_ref_ns.
 * Callers must use this rather than their own clock_gettime(), because the two
 * have to be comparable.
 */
uint64_t v4l2_capture_now_ns(void);

#endif
