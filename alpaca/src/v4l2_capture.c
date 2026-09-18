#include "v4l2_capture.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
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

/* Resolving a control by name used to re-open the subdev and walk the whole
 * VIDIOC_QUERY_EXT_CTRL enumeration on *every* get/set, and the exposure path
 * does several of those per frame. The fd and each resolved control are now
 * cached, so the steady-state cost is one ioctl. Guarded by a mutex: the
 * exposure thread and CivetWeb's handler threads both come through here. */
#define CTRL_CACHE_MAX 8
static pthread_mutex_t g_ctrl_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Serialises a whole "set controls, then capture the frame that reflects them"
 * sequence. g_ctrl_lock above only guards individual control ioctls, which is
 * not enough: the unit that must be atomic spans the control writes, the
 * settle reference timestamp taken right after them, and the capture itself.
 * Two threads interleaving there share one fd and one mmap'd buffer pool, and
 * which thread receives which frame becomes undefined.
 *
 * There are now two callers -- the Alpaca exposure worker and the guide loop --
 * so this is load-bearing rather than theoretical. It also closes the
 * concurrent-exposure_worker race noted in the project's open issues: a second
 * startexposure arriving mid-capture now waits instead of racing.
 */
static pthread_mutex_t g_exposure_lock = PTHREAD_MUTEX_INITIALIZER;

void v4l2_exposure_lock(void) { pthread_mutex_lock(&g_exposure_lock); }
void v4l2_exposure_unlock(void) { pthread_mutex_unlock(&g_exposure_lock); }
static int g_sub_fd = -1;
static char g_sub_path[128];
static struct {
	char name[64];
	struct v4l2_query_ext_ctrl qc;
} g_ctrl_cache[CTRL_CACHE_MAX];
static int g_ctrl_cache_n;

/* Caller holds g_ctrl_lock. */
static int subdev_fd_locked(const char *subdev_path) {
	if (g_sub_fd >= 0 && strcmp(g_sub_path, subdev_path) == 0)
		return g_sub_fd;
	if (g_sub_fd >= 0) {
		close(g_sub_fd);
		g_sub_fd = -1;
		g_ctrl_cache_n = 0;
	}
	int fd = open(subdev_path, O_RDWR);
	if (fd < 0)
		return -1;
	snprintf(g_sub_path, sizeof(g_sub_path), "%s", subdev_path);
	g_sub_fd = fd;
	return fd;
}

/* Caller holds g_ctrl_lock. */
static const struct v4l2_query_ext_ctrl *ctrl_lookup_locked(int fd,
                                                             const char *name) {
	for (int i = 0; i < g_ctrl_cache_n; i++)
		if (strcmp(g_ctrl_cache[i].name, name) == 0)
			return &g_ctrl_cache[i].qc;
	struct v4l2_query_ext_ctrl qc;
	if (find_ctrl(fd, name, &qc) != 0)
		return NULL;
	if (g_ctrl_cache_n == CTRL_CACHE_MAX)
		return NULL; /* more distinct controls than any sensor desc uses */
	snprintf(g_ctrl_cache[g_ctrl_cache_n].name,
	         sizeof(g_ctrl_cache[0].name), "%s", name);
	g_ctrl_cache[g_ctrl_cache_n].qc = qc;
	return &g_ctrl_cache[g_ctrl_cache_n++].qc;
}

static int ctrl_io(const char *subdev_path, const char *name, int64_t *value,
                    int is_set) {
	pthread_mutex_lock(&g_ctrl_lock);
	int fd = subdev_fd_locked(subdev_path);
	if (fd < 0) {
		pthread_mutex_unlock(&g_ctrl_lock);
		return -1;
	}
	const struct v4l2_query_ext_ctrl *qc = ctrl_lookup_locked(fd, name);
	if (qc == NULL) {
		pthread_mutex_unlock(&g_ctrl_lock);
		errno = ENOENT;
		return -1;
	}

	struct v4l2_ext_control ctrl;
	struct v4l2_ext_controls ctrls;
	memset(&ctrl, 0, sizeof(ctrl));
	memset(&ctrls, 0, sizeof(ctrls));
	ctrl.id = qc->id;
	ctrls.ctrl_class = V4L2_CTRL_ID2CLASS(qc->id);
	ctrls.count = 1;
	ctrls.controls = &ctrl;
	int is64 = (qc->type == V4L2_CTRL_TYPE_INTEGER64);

	if (is_set) {
		if (is64)
			ctrl.value64 = *value;
		else
			ctrl.value = (int32_t)*value;
	}
	int r = ioctl(fd, is_set ? VIDIOC_S_EXT_CTRLS : VIDIOC_G_EXT_CTRLS, &ctrls);
	if (r == 0 && !is_set)
		*value = is64 ? ctrl.value64 : ctrl.value;
	pthread_mutex_unlock(&g_ctrl_lock);
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
	pthread_mutex_lock(&g_ctrl_lock);
	int fd = subdev_fd_locked(subdev_path);
	if (fd < 0) {
		pthread_mutex_unlock(&g_ctrl_lock);
		return -1;
	}
	/* Not cached: a control's min/max can change at runtime (exposure's max
	 * tracks vertical_blanking), so this re-queries every time. */
	struct v4l2_query_ext_ctrl qc;
	if (find_ctrl(fd, name, &qc) != 0) {
		pthread_mutex_unlock(&g_ctrl_lock);
		errno = ENOENT;
		return -1;
	}
	pthread_mutex_unlock(&g_ctrl_lock);
	*min = qc.minimum;
	*max = qc.maximum;
	return 0;
}

/* Persistent capture state, set up once by v4l2_capture_init() and reused by
 * every v4l2_capture_frame() call.
 *
 * NUM_BUFFERS is deliberately > 1 again (it was briefly 1, which broke real
 * captures): with a single buffer the driver has nowhere to write from the
 * moment we dequeue it until we requeue it, which on rkcif means the in-flight
 * frame lands back in the very buffer userspace is still unpacking. Keeping a
 * small pool queued means the DMA target is always a buffer we don't own, so a
 * frame is never torn, and the sensor keeps streaming continuously between
 * exposures instead of stalling. */
#define NUM_BUFFERS 3

static int g_fd = -1;
static const sensor_desc_t *g_desc;
static void *g_bufs[NUM_BUFFERS];
static __u32 g_buf_lengths[NUM_BUFFERS];
static unsigned g_nbufs;
static __u32 g_stride;

static void teardown_locked(void) {
	if (g_fd < 0)
		return;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ioctl(g_fd, VIDIOC_STREAMOFF, &type);
	for (unsigned i = 0; i < g_nbufs; i++)
		if (g_bufs[i])
			munmap(g_bufs[i], g_buf_lengths[i]);
	memset(g_bufs, 0, sizeof(g_bufs));
	close(g_fd);
	g_fd = -1;
	g_nbufs = 0;
}

static int qbuf_index(unsigned index) {
	struct v4l2_plane plane;
	struct v4l2_buffer buf;
	memset(&plane, 0, sizeof(plane));
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = index;
	buf.m.planes = &plane;
	buf.length = 1;
	return ioctl(g_fd, VIDIOC_QBUF, &buf);
}

/* Dequeues one buffer if one is already done, without blocking. Returns its
 * index, -1 if none is ready (EAGAIN) or on error. When ts_ns is non-NULL it
 * receives the buffer's timestamp, which rkcif sets from the CSI frame-START
 * interrupt (capture.c: vb2_buf.timestamp = readout.fs_timestamp). Note that
 * this is when READOUT started, not when integration started -- on a rolling
 * shutter those differ by about a frame period, which is why the caller needs
 * one more discard beyond the timestamp test. */
static int dqbuf_nowait(uint64_t *ts_ns) {
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
	if (ts_ns != NULL)
		*ts_ns = (uint64_t)buf.timestamp.tv_sec * 1000000000ull +
		         (uint64_t)buf.timestamp.tv_usec * 1000ull;
	return (int)buf.index;
}

/* Waits (up to timeout_ms) for a completed buffer and dequeues it. The fd is
 * O_NONBLOCK so that dqbuf_nowait() above works; poll() supplies the blocking
 * half, and unlike a blocking DQBUF it can actually time out rather than
 * wedging the exposure thread forever if the CSI link drops. */
static int dqbuf_wait(int timeout_ms, uint64_t *ts_ns) {
	struct pollfd pfd = {.fd = g_fd, .events = POLLIN, .revents = 0};
	for (;;) {
		int r = poll(&pfd, 1, timeout_ms);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		int idx = dqbuf_nowait(ts_ns);
		if (idx >= 0)
			return idx;
		if (errno != EAGAIN)
			return -1;
	}
}

int v4l2_capture_init(const sensor_desc_t *desc) {
	if (g_fd >= 0)
		teardown_locked();

	int fd = open(desc->video_path, O_RDWR | O_NONBLOCK);
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
	g_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count = NUM_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
		close(fd);
		return -1;
	}

	g_fd = fd;
	g_desc = desc;
	g_nbufs = req.count < NUM_BUFFERS ? req.count : NUM_BUFFERS;
	memset(g_bufs, 0, sizeof(g_bufs));

	for (unsigned i = 0; i < g_nbufs; i++) {
		struct v4l2_plane plane;
		struct v4l2_buffer buf;
		memset(&plane, 0, sizeof(plane));
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.m.planes = &plane;
		buf.length = 1;
		if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			teardown_locked();
			return -1;
		}
		g_bufs[i] = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED,
		                  fd, plane.m.mem_offset);
		if (g_bufs[i] == MAP_FAILED) {
			g_bufs[i] = NULL;
			teardown_locked();
			return -1;
		}
		g_buf_lengths[i] = plane.length;
		if (qbuf_index(i) < 0) {
			teardown_locked();
			return -1;
		}
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		teardown_locked();
		return -1;
	}

	/* First capture after STREAMON can still reflect pre-streaming defaults
	 * (confirmed on real hardware) -- absorb that here, before the HTTP
	 * server starts, so no client ever sees it. */
	v4l2_frame_t warmup;
	if (v4l2_capture_frame(&warmup, 0.0, 0) != 0) {
		teardown_locked();
		return -1;
	}
	free(warmup.pixels);
	return 0;
}

void v4l2_capture_shutdown(void) {
	teardown_locked();
}

/* The DQBUF timeout exists to fail an exposure rather than hang the daemon if
 * the CSI link stops delivering -- it must never be the thing that bounds a
 * legitimate exposure. It used to be a flat 5000ms, which was fine only
 * because vertical_blanking was pinned; with client-driven blanking a frame
 * period can exceed it, and the exposure then fails for no sensor reason.
 *
 * Each dqbuf_wait() below blocks for at most one frame period, so scale from
 * that, with a wide multiplier (the first frame after a blanking change can
 * straddle an old, longer period) and a floor that preserves the previous
 * behaviour for short exposures. */
#define DQBUF_TIMEOUT_FLOOR_MS 5000

static int dqbuf_timeout_ms(double frame_period_s) {
	if (!(frame_period_s > 0.0))
		return DQBUF_TIMEOUT_FLOOR_MS;
	double ms = frame_period_s * 1000.0 * 3.0 + DQBUF_TIMEOUT_FLOOR_MS;
	if (ms > 120000.0)
		ms = 120000.0; /* absolute backstop, still > any sane exposure */
	return (int)ms;
}

uint64_t v4l2_capture_now_ns(void) {
	/* Must match the clock rkcif stamps buffers with. On RV1106 that is
	 * rkcif_time_get_ns() -> ktime_get_boottime_ns() (cif/dev.h), even though
	 * the queue advertises V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC. The two differ
	 * only by time spent suspended, which this board never does, but matching
	 * the driver exactly costs nothing and removes the assumption. */
	struct timespec ts;
	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Bounds the freshness loop below. Each iteration consumes one real frame, so
 * this only trips if the driver keeps handing back frames that started before
 * the controls were written -- a stuck pipeline, not a slow one. The dqbuf
 * timeout is what bounds waiting for a *slow* frame. */
#define MAX_STALE_DISCARDS 8

int v4l2_capture_frame(v4l2_frame_t *out, double frame_period_s,
                       uint64_t settle_ref_ns) {
	const int timeout_ms = dqbuf_timeout_ms(frame_period_s);
	if (g_fd < 0)
		return -1;

	double t0 = now_ms();

	/* Streaming never stops between exposures, so the pool holds frames that
	 * began before the caller's v4l2_ctrl_set() calls and therefore carry the
	 * *previous* exposure's settings. Discard until we get one that started
	 * after settle_ref_ns (the moment those writes completed).
	 *
	 * This used to be "drain what's ready, then discard exactly one more",
	 * which is not sound: with a 3-buffer pool up to two frames can still be
	 * in flight, so whether the survivor was fresh depended on timing. It
	 * failed for real -- 3 of 8 alternating 0.05s/0.5s exposures came back
	 * with the previous frame (measured 2026-09-17: dqbuf ~46ms for a 0.5s
	 * exposure, which is physically impossible).
	 *
	 * An earlier `extra_settle` flag papered over this by discarding a second
	 * frame on any blanking change. It was removed on 2026-09-16 as "measured
	 * obsolete", but that A/B used OAG_FRAME_STATS as ground truth and the
	 * stats walk adds ~85ms per capture, which masked the race: with stats on
	 * the failure does not reproduce at all, with stats off it is ~40%.
	 * Comparing frame timestamps makes that first part deterministic: however
	 * many frames the pool happens to be holding, they are identified by when
	 * they started rather than by counting.
	 *
	 * That alone is NOT sufficient, and measuring is what showed it. The
	 * timestamp rkcif reports is the frame-START (readout) interrupt, but this
	 * is a ROLLING SHUTTER: integration for a frame begins roughly one frame
	 * period before its readout starts. So the first frame with
	 * fs_timestamp >= settle_ref_ns was already integrating when the controls
	 * were written, and still carries the old exposure -- observed directly
	 * (2026-09-17): discarding purely by timestamp still returned a 151ms
	 * frame for a 0.5s request. Hence exactly one more discard after the
	 * timestamp filter passes. That one is principled rather than tuned: it is
	 * the frame whose integration straddles the write. */
	int discarded = 0;
	int idx;
	uint64_t ts_ns = 0;
	for (;;) {
		idx = dqbuf_wait(timeout_ms, &ts_ns);
		if (idx < 0)
			return -1;
		if (settle_ref_ns == 0 || ts_ns == 0 || ts_ns >= settle_ref_ns)
			break; /* started after the controls landed (or unknowable) */
		if (qbuf_index((unsigned)idx) < 0)
			return -1;
		if (++discarded >= MAX_STALE_DISCARDS) {
			fprintf(stderr,
			        "[v4l2] gave up after %d stale frames (ref=%llu)\n",
			        discarded, (unsigned long long)settle_ref_ns);
			return -1;
		}
	}
	uint64_t first_fresh_ns = ts_ns;
	if (settle_ref_ns != 0) {
		if (qbuf_index((unsigned)idx) < 0)
			return -1;
		discarded++;
		idx = dqbuf_wait(timeout_ms, &ts_ns);
		if (idx < 0)
			return -1;
	}
	/* How long after the control writes the returned frame actually began
	 * readout. Anything much less than one frame period means the frame did
	 * not integrate for the full requested time -- the exact failure this
	 * discard logic exists to prevent, and not otherwise visible when the
	 * scene is saturated (frame means are then identical regardless). */
	if (settle_ref_ns != 0 && ts_ns != 0)
		fprintf(stderr,
		        "[v4l2] frame_start_after_ctrl=%.0fms (first_fresh=%.0fms)\n",
		        (double)(ts_ns - settle_ref_ns) / 1e6,
		        (double)(first_fresh_ns - settle_ref_ns) / 1e6);
	double t_discard = now_ms();
	double t_dqbuf = t_discard;

	out->width = g_desc->width;
	out->height = g_desc->height;
	out->pixels =
	    malloc((size_t)g_desc->width * g_desc->height * sizeof(uint16_t));
	if (out->pixels == NULL) {
		qbuf_index((unsigned)idx);
		return -1;
	}
	g_desc->unpack((const uint8_t *)g_bufs[idx], (int)g_stride, g_desc->width,
	               g_desc->height, out->pixels);
	double t_unpack = now_ms();

	/* Only now is this buffer safe to hand back to the driver. */
	if (qbuf_index((unsigned)idx) < 0) {
		free(out->pixels);
		out->pixels = NULL;
		return -1;
	}
	double t_requeue = now_ms();

	fprintf(stderr,
	        "[v4l2] wait=%.0f discarded=%d unpack=%.0f requeue=%.0f "
	        "total=%.0f\n",
	        t_discard - t0, discarded, t_unpack - t_dqbuf,
	        t_requeue - t_unpack, t_requeue - t0);
	return 0;
}
