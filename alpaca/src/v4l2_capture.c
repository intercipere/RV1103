#include "v4l2_capture.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

/* Per-stage timing for v4l2_capture_frame(), to find out precisely which V4L2
 * ioctl the observed ~650ms fixed floor (independent of actual exposure
 * duration) is actually in, rather than guessing. Mirrors camera_api.c's
 * now_ms() helper (not shared, to avoid a cross-file dependency for a few
 * lines of diagnostic code). */
static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

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

/* Persistent capture state, set up once by v4l2_capture_init() and reused by
 * every v4l2_capture_frame() call -- see v4l2_capture.h for why a single
 * buffer (not the old NUM_BUFFERS=4 pool) is sufficient and in fact what
 * makes the discard-then-capture correctness argument in
 * v4l2_capture_frame() straightforward: at most one buffer is ever in
 * flight, so there is no ambiguity about which completed buffer is "the
 * stale one" vs. "the fresh one". */
static int g_fd = -1;
static const sensor_desc_t *g_desc;
static void *g_buf;
static __u32 g_buf_length;
static __u32 g_stride;

static void teardown_locked(void) {
	if (g_fd < 0)
		return;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ioctl(g_fd, VIDIOC_STREAMOFF, &type);
	if (g_buf)
		munmap(g_buf, g_buf_length);
	close(g_fd);
	g_fd = -1;
	g_buf = NULL;
}

int v4l2_capture_init(const sensor_desc_t *desc) {
	if (g_fd >= 0)
		teardown_locked();

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

	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count = 1;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
		close(fd);
		return -1;
	}

	struct v4l2_plane plane;
	struct v4l2_buffer buf;
	memset(&plane, 0, sizeof(plane));
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = 0;
	buf.m.planes = &plane;
	buf.length = 1;
	if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
		close(fd);
		return -1;
	}
	void *bufptr = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED,
	                     fd, plane.m.mem_offset);
	if (bufptr == MAP_FAILED) {
		close(fd);
		return -1;
	}
	if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
		munmap(bufptr, plane.length);
		close(fd);
		return -1;
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		munmap(bufptr, plane.length);
		close(fd);
		return -1;
	}

	g_fd = fd;
	g_desc = desc;
	g_buf = bufptr;
	g_buf_length = plane.length;
	g_stride = stride;

	/* First capture after STREAMON can still reflect pre-streaming defaults
	 * (confirmed on real hardware) -- absorb that here, before the HTTP
	 * server starts, so no client ever sees it. */
	v4l2_frame_t warmup;
	if (v4l2_capture_frame(&warmup, 0) != 0) {
		teardown_locked();
		return -1;
	}
	free(warmup.pixels);
	return 0;
}

void v4l2_capture_shutdown(void) {
	teardown_locked();
}

/* Dequeues buffer 0 (blocking) and immediately requeues it. Used both to
 * discard a stale frame and, after unpacking, to re-arm the buffer for the
 * next call. */
static int dqbuf_requeue(int requeue) {
	struct v4l2_plane plane;
	struct v4l2_buffer buf;
	memset(&plane, 0, sizeof(plane));
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.m.planes = &plane;
	buf.length = 1;
	if (ioctl(g_fd, VIDIOC_DQBUF, &buf) < 0)
		return -1;
	if (requeue && ioctl(g_fd, VIDIOC_QBUF, &buf) < 0)
		return -1;
	return 0;
}

int v4l2_capture_frame(v4l2_frame_t *out, int extra_settle) {
	if (g_fd < 0)
		return -1;

	double t0 = now_ms();
	/* Discard whatever the driver already had ready, then requeue -- the
	 * driver can't start its next capture before that requeue, which
	 * happens after the caller's v4l2_ctrl_set() calls. A second round is
	 * needed when vertical_blanking (frame period, not just an integration
	 * value) just changed -- see v4l2_capture.h. */
	if (dqbuf_requeue(1) != 0)
		return -1;
	if (extra_settle && dqbuf_requeue(1) != 0)
		return -1;
	double t_discard = now_ms();

	/* This is the frame whose capture window began at or after the requeue
	 * above -- guaranteed fresh regardless of whether this sensor latches
	 * new exposure/gain at the very next frame or one frame later. Don't
	 * requeue yet: the buffer must stay mapped and untouched by the driver
	 * until unpack() below has copied its contents out. */
	if (dqbuf_requeue(0) != 0)
		return -1;
	double t_dqbuf = now_ms();

	out->width = g_desc->width;
	out->height = g_desc->height;
	out->pixels =
	    malloc((size_t)g_desc->width * g_desc->height * sizeof(uint16_t));
	if (out->pixels == NULL)
		return -1;
	g_desc->unpack((const uint8_t *)g_buf, (int)g_stride, g_desc->width,
	               g_desc->height, out->pixels);
	double t_unpack = now_ms();

	/* Re-arm for the next call's discard step, keeping the driver capturing
	 * continuously in the background between exposures. */
	struct v4l2_plane plane;
	struct v4l2_buffer buf;
	memset(&plane, 0, sizeof(plane));
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = 0;
	buf.m.planes = &plane;
	buf.length = 1;
	if (ioctl(g_fd, VIDIOC_QBUF, &buf) < 0) {
		free(out->pixels);
		out->pixels = NULL;
		return -1;
	}
	double t_requeue = now_ms();

	fprintf(stderr,
	        "[v4l2] discard=%.0f dqbuf=%.0f unpack=%.0f requeue=%.0f "
	        "total=%.0f\n",
	        t_discard - t0, t_dqbuf - t_discard, t_unpack - t_dqbuf,
	        t_requeue - t_unpack, t_requeue - t0);
	return 0;
}
