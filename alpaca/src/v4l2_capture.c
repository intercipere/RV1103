#include "v4l2_capture.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

/* The kernel reports control names in human-readable form ("Exposure",
 * "Analogue Gain"); v4l2-ctl (and this project's sensor.h ctrl_* names,
 * matching v4l2-ctl's convention) normalize that to lowercase with
 * underscores ("exposure", "analogue_gain"). Normalize both sides the same
 * way before comparing so callers can use the v4l2-ctl-style names. */
static void normalize(char *dst, const char *src, size_t dstlen) {
	size_t i = 0;
	for (; src[i] != '\0' && i + 1 < dstlen; i++)
		dst[i] = (char)(src[i] == ' ' ? '_' : tolower((unsigned char)src[i]));
	dst[i] = '\0';
}

/* Finds a control's id/type by name via VIDIOC_QUERY_EXT_CTRL enumeration
 * (V4L2_CTRL_FLAG_NEXT_CTRL walks every control the device exposes, exactly
 * like `v4l2-ctl --list-ctrls` does -- including one V4L2_CTRL_TYPE_CTRL_CLASS
 * pseudo-entry per class, which simply won't match any real control name).
 * Returns 0 and fills *qc on success. */
static int find_ctrl(int fd, const char *name, struct v4l2_query_ext_ctrl *qc) {
	char norm[64];
	memset(qc, 0, sizeof(*qc));
	qc->id = V4L2_CTRL_FLAG_NEXT_CTRL;
	while (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, qc) == 0) {
		normalize(norm, (const char *)qc->name, sizeof(norm));
		if (strcmp(norm, name) == 0)
			return 0;
		qc->id |= V4L2_CTRL_FLAG_NEXT_CTRL;
	}
	return -1;
}

static int ctrl_io(const char *subdev_path, const char *name, int64_t *value,
                    int is_set) {
	int fd = open(subdev_path, O_RDWR);
	if (fd < 0)
		return -1;

	struct v4l2_query_ext_ctrl qc;
	if (find_ctrl(fd, name, &qc) != 0) {
		close(fd);
		errno = ENOENT;
		return -1;
	}

	struct v4l2_ext_control ctrl;
	struct v4l2_ext_controls ctrls;
	memset(&ctrl, 0, sizeof(ctrl));
	memset(&ctrls, 0, sizeof(ctrls));
	ctrl.id = qc.id;
	ctrls.ctrl_class = V4L2_CTRL_ID2CLASS(qc.id);
	ctrls.count = 1;
	ctrls.controls = &ctrl;

	if (qc.type == V4L2_CTRL_TYPE_INTEGER64) {
		if (is_set)
			ctrl.value64 = *value;
		int r = ioctl(fd, is_set ? VIDIOC_S_EXT_CTRLS : VIDIOC_G_EXT_CTRLS,
		              &ctrls);
		if (r == 0 && !is_set)
			*value = ctrl.value64;
		close(fd);
		return r;
	}

	if (is_set)
		ctrl.value = (int32_t)*value;
	int r = ioctl(fd, is_set ? VIDIOC_S_EXT_CTRLS : VIDIOC_G_EXT_CTRLS, &ctrls);
	if (r == 0 && !is_set)
		*value = ctrl.value;
	close(fd);
	return r;
}

int v4l2_ctrl_get(const char *subdev_path, const char *name, int64_t *value) {
	return ctrl_io(subdev_path, name, value, 0);
}

int v4l2_ctrl_set(const char *subdev_path, const char *name, int64_t value) {
	return ctrl_io(subdev_path, name, &value, 1);
}

int v4l2_ctrl_get_range(const char *subdev_path, const char *name,
                         int64_t *min, int64_t *max) {
	int fd = open(subdev_path, O_RDWR);
	if (fd < 0)
		return -1;
	struct v4l2_query_ext_ctrl qc;
	if (find_ctrl(fd, name, &qc) != 0) {
		close(fd);
		errno = ENOENT;
		return -1;
	}
	close(fd);
	*min = qc.minimum;
	*max = qc.maximum;
	return 0;
}

#define NUM_BUFFERS 4

int v4l2_capture_frame(const sensor_desc_t *desc, v4l2_frame_t *out) {
	int fd = open(desc->video_path, O_RDWR);
	if (fd < 0)
		return -1;

	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = desc->width;
	fmt.fmt.pix_mp.height = desc->height;
	fmt.fmt.pix_mp.pixelformat = desc->v4l2_fourcc;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		close(fd);
		return -1;
	}
	__u32 stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	__u32 planesize = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count = NUM_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
		close(fd);
		return -1;
	}

	void *bufs[NUM_BUFFERS] = {0};
	__u32 buf_lengths[NUM_BUFFERS] = {0};
	unsigned nbufs = req.count;

	for (unsigned i = 0; i < nbufs; i++) {
		struct v4l2_plane plane;
		struct v4l2_buffer buf;
		memset(&plane, 0, sizeof(plane));
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.m.planes = &plane;
		buf.length = 1;
		if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
			goto fail_unmap;
		bufs[i] = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED,
		               fd, plane.m.mem_offset);
		if (bufs[i] == MAP_FAILED) {
			bufs[i] = NULL;
			goto fail_unmap;
		}
		buf_lengths[i] = plane.length;
		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
			goto fail_unmap;
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
		goto fail_unmap;

	struct v4l2_plane plane;
	struct v4l2_buffer buf;
	memset(&plane, 0, sizeof(plane));
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.m.planes = &plane;
	buf.length = 1;
	if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
		ioctl(fd, VIDIOC_STREAMOFF, &type);
		goto fail_unmap;
	}

	out->width = desc->width;
	out->height = desc->height;
	out->pixels = malloc((size_t)desc->width * desc->height * sizeof(uint16_t));
	if (out->pixels == NULL) {
		ioctl(fd, VIDIOC_STREAMOFF, &type);
		goto fail_unmap;
	}
	desc->unpack((const uint8_t *)bufs[buf.index], (int)stride, desc->width,
	             desc->height, out->pixels);

	ioctl(fd, VIDIOC_STREAMOFF, &type);
	for (unsigned i = 0; i < nbufs; i++)
		if (bufs[i])
			munmap(bufs[i], buf_lengths[i]);
	close(fd);
	(void)planesize;
	return 0;

fail_unmap:
	for (unsigned i = 0; i < nbufs; i++)
		if (bufs[i])
			munmap(bufs[i], buf_lengths[i]);
	close(fd);
	return -1;
}
