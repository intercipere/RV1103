/*
 * The in-daemon frame source: captures straight off V4L2, sharing the streaming
 * device alpacad already owns.
 *
 * Compiled only into alpacad. The host-side test harness uses the HTTP source
 * in guide_source.c instead, because from outside the daemon there is no way to
 * reach /dev/video0 -- it has exactly one owner and alpacad is it.
 *
 * This duplicates the control sequence from camera_api.c's exposure_worker()
 * rather than calling it, because that function is bound to the Alpaca device
 * state machine (it publishes into g_device.last_frame and drives
 * CameraState). The sequence itself is small; what must not be duplicated is
 * the reasoning behind the control write *order* and the settle logic, so the
 * comments point at the original rather than restating it.
 */

#include "guide_source.h"

#include "sensor.h"
#include "v4l2_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Matches camera_api.c. Keeping the guide loop inside the same blanking
 * headroom means it does not fight the Alpaca path over frame timing. */
#define VBLANK_HEADROOM 4
#define OAG_FIXED_VBLANK 64

static int v4l2_next(guide_source_t *src) {
	const sensor_desc_t *s = (const sensor_desc_t *)src->desc;
	v4l2_frame_t frame;
	int64_t vblank_cur = 0, vblank_min = 0, vblank_max = 0;
	int64_t exp_min = 0, exp_max_cur = 0, exp_cur = 0;
	int64_t need_vblank, rows;
	long margin;
	int vblank_changed, controls_changed;
	uint64_t settle_ref_ns;
	double frame_period_s;
	int64_t applied_vblank = -1;
	int ok;

	rows = (int64_t)(src->exposure_s * 1e6 / s->row_time_us + 0.5);
	if (rows < 1)
		rows = 1;

	v4l2_exposure_lock();

	v4l2_ctrl_get(s->subdev_path, s->ctrl_vblank, &vblank_cur);
	v4l2_ctrl_get(s->subdev_path, s->ctrl_exposure, &exp_cur);
	v4l2_ctrl_get_range(s->subdev_path, s->ctrl_vblank, &vblank_min, &vblank_max);
	v4l2_ctrl_get_range(s->subdev_path, s->ctrl_exposure, &exp_min, &exp_max_cur);

	if (rows < exp_min)
		rows = exp_min;

	margin = (s->height + vblank_cur) - exp_max_cur;
	need_vblank = rows + margin + VBLANK_HEADROOM - s->height;
	if (need_vblank < OAG_FIXED_VBLANK) need_vblank = OAG_FIXED_VBLANK;
	if (need_vblank > vblank_max) need_vblank = vblank_max;
	if (need_vblank < vblank_min) need_vblank = vblank_min;

	/* Write order depends on direction -- the driver clamps exposure against
	 * the *current* blanking. See camera_api.c's exposure_worker() for the
	 * full reasoning; this must stay in step with it. */
	vblank_changed = (need_vblank != vblank_cur);
	if (need_vblank > vblank_cur) {
		v4l2_ctrl_set(s->subdev_path, s->ctrl_vblank, need_vblank);
		v4l2_ctrl_set(s->subdev_path, s->ctrl_exposure, rows);
	} else {
		v4l2_ctrl_set(s->subdev_path, s->ctrl_exposure, rows);
		if (vblank_changed)
			v4l2_ctrl_set(s->subdev_path, s->ctrl_vblank, need_vblank);
		v4l2_ctrl_set(s->subdev_path, s->ctrl_exposure, rows);
	}

	/* Nothing moved means nothing in flight is stale, and skipping the settle
	 * discards saves a whole frame period. A guiding loop repeats one exposure
	 * indefinitely, so this is the common case here -- more so than on the
	 * Alpaca path it was added for. */
	controls_changed = vblank_changed || (rows != exp_cur);
	settle_ref_ns = controls_changed ? v4l2_capture_now_ns() : 0;

	v4l2_ctrl_get(s->subdev_path, s->ctrl_vblank, &applied_vblank);
	frame_period_s = applied_vblank >= 0
	                     ? (double)(s->height + applied_vblank) * s->row_time_us / 1e6
	                     : 0.0;

	ok = (v4l2_capture_frame(&frame, frame_period_s, settle_ref_ns) == 0);
	v4l2_exposure_unlock();

	if (!ok) {
		snprintf(src->err, sizeof(src->err), "v4l2_capture_frame failed");
		return -1;
	}
	if (frame.width != src->width || frame.height != src->height) {
		snprintf(src->err, sizeof(src->err), "frame is %dx%d, expected %dx%d",
		         frame.width, frame.height, src->width, src->height);
		free(frame.pixels);
		return -1;
	}

	/* The capture layer hands over a freshly malloc'd buffer each time. Rather
	 * than keep both that and our own, adopt it and release the previous one --
	 * one frame resident, not two. On a 32 MB board that difference is the
	 * whole ball game. */
	free(src->pixels);
	src->pixels = frame.pixels;
	src->image.px = src->pixels;
	src->frame++;
	return 0;
}

int guide_source_open_v4l2(guide_source_t *src, const void *desc,
                           double exposure_s) {
	const sensor_desc_t *s = (const sensor_desc_t *)desc;

	memset(src, 0, sizeof(*src));
	src->kind = GUIDE_SRC_V4L2;
	src->next = v4l2_next;
	src->desc = desc;
	src->exposure_s = exposure_s;
	src->width = s->width;
	src->height = s->height;
	src->max_adu = (uint16_t)s->max_adu;

	src->pixels = NULL; /* adopted from the first capture */
	src->image.px = NULL;
	src->image.width = src->width;
	src->image.height = src->height;
	src->image.x_stride = src->height; /* alpacad wire order: x outer */
	src->image.y_stride = 1;
	src->image.max_adu = src->max_adu;
	return 0;
}
