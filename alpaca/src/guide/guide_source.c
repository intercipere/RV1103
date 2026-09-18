#include "guide_source.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------- tiny HTTP client -- */

/*
 * Just enough HTTP to drive the Alpaca camera API: one request per connection,
 * no keep-alive, no chunked encoding. alpacad sends an exact Content-Length on
 * every response (that was a deliberate fix -- see the project notes), so the
 * body length is always known up front and this can stay this small.
 */

static int http_connect(const char *host, int port, char *err, size_t errlen) {
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct timeval tv;

	if (fd < 0) {
		snprintf(err, errlen, "socket: %s", strerror(errno));
		return -1;
	}
	tv.tv_sec = 180;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		snprintf(err, errlen, "bad host address '%s'", host);
		close(fd);
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		snprintf(err, errlen, "connect %s:%d: %s", host, port, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int read_full(int fd, void *buf, size_t len) {
	size_t got = 0;
	while (got < len) {
		ssize_t r = read(fd, (char *)buf + got, len - got);
		if (r <= 0)
			return -1;
		got += (size_t)r;
	}
	return 0;
}

/*
 * Sends one request and leaves the connection positioned at the start of the
 * body. Returns the socket in *fd (caller closes), the body length in *clen,
 * and whatever body bytes already arrived alongside the headers in prefix,
 * which must therefore be at least 4096 bytes -- one read() can return the
 * headers and a chunk of body together.
 *
 * Split this way on purpose. A frame is ~6 MB and this device has ~19 MB free
 * with alpacad resident -- buffering a whole response before copying it into
 * the frame holds two copies at once. That is not hypothetical: the first
 * version of this file did exactly that, and the kernel OOM-killed *alpacad*
 * rather than the test that caused it. The caller streams the body straight
 * where it belongs instead.
 */
static int http_begin(const char *host, int port, const char *method,
                      const char *path, const char *accept, const char *body_in,
                      int *fd_out, size_t *clen, char *prefix, size_t prefix_cap,
                      size_t *prefix_len, char *err, size_t errlen) {
	char req[1024], hdr[4096];
	int fd, status = 0;
	size_t hlen = 0;
	char *p, *end;
	int n;

	*fd_out = -1;
	*clen = 0;
	*prefix_len = 0;

	fd = http_connect(host, port, err, errlen);
	if (fd < 0)
		return -1;

	if (body_in)
		n = snprintf(req, sizeof(req),
		             "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
		             "Content-Type: application/x-www-form-urlencoded\r\n"
		             "Content-Length: %zu\r\n%s%s%s\r\n%s",
		             method, path, host, port, strlen(body_in),
		             accept ? "Accept: " : "", accept ? accept : "",
		             accept ? "\r\n" : "", body_in);
	else
		n = snprintf(req, sizeof(req),
		             "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
		             "%s%s%s\r\n",
		             method, path, host, port,
		             accept ? "Accept: " : "", accept ? accept : "",
		             accept ? "\r\n" : "");
	if (n < 0 || (size_t)n >= sizeof(req)) {
		snprintf(err, errlen, "request too long");
		close(fd);
		return -1;
	}
	if (write(fd, req, (size_t)n) != n) {
		snprintf(err, errlen, "write: %s", strerror(errno));
		close(fd);
		return -1;
	}

	for (;;) {
		ssize_t r;
		if (hlen + 1 >= sizeof(hdr)) {
			snprintf(err, errlen, "response header too large");
			close(fd);
			return -1;
		}
		r = read(fd, hdr + hlen, sizeof(hdr) - hlen - 1);
		if (r <= 0) {
			snprintf(err, errlen, "no response header (is alpacad running?)");
			close(fd);
			return -1;
		}
		hlen += (size_t)r;
		hdr[hlen] = '\0';
		if (strstr(hdr, "\r\n\r\n"))
			break;
	}

	if (sscanf(hdr, "HTTP/1.%*d %d", &status) != 1) {
		snprintf(err, errlen, "malformed status line");
		close(fd);
		return -1;
	}
	if (status != 200) {
		snprintf(err, errlen, "HTTP %d for %s", status, path);
		close(fd);
		return -1;
	}

	p = strstr(hdr, "Content-Length:");
	if (!p)
		p = strstr(hdr, "content-length:");
	if (!p) {
		snprintf(err, errlen, "no Content-Length for %s", path);
		close(fd);
		return -1;
	}
	*clen = (size_t)strtoul(p + 15, NULL, 10);

	end = strstr(hdr, "\r\n\r\n") + 4;
	{
		size_t have = hlen - (size_t)(end - hdr);
		if (have > *clen)
			have = *clen;
		if (have > prefix_cap) {
			snprintf(err, errlen, "response preamble larger than %zu bytes", prefix_cap);
			close(fd);
			return -1;
		}
		memcpy(prefix, end, have);
		*prefix_len = have;
	}
	*fd_out = fd;
	return 0;
}

/* For small JSON responses only: reads the whole body into a caller buffer. */
static int http_small(const char *host, int port, const char *method,
                      const char *path, const char *body_in,
                      char *out, size_t out_cap, char *err, size_t errlen) {
	int fd;
	size_t clen = 0, have = 0;

	if (http_begin(host, port, method, path, NULL, body_in, &fd, &clen,
	               out, out_cap - 1, &have, err, errlen) != 0)
		return -1;
	if (clen >= out_cap) {
		snprintf(err, errlen, "response of %zu bytes too large for %s", clen, path);
		close(fd);
		return -1;
	}
	if (have < clen && read_full(fd, out + have, clen - have) != 0) {
		snprintf(err, errlen, "short body for %s", path);
		close(fd);
		return -1;
	}
	out[clen] = '\0';
	close(fd);
	return 0;
}

/* ------------------------------------------------------------- camera src -- */

static int alpaca_value(const char *host, int port, const char *member,
                        char *json, size_t cap, char *err, size_t errlen) {
	char path[256];
	snprintf(path, sizeof(path), "/api/v1/camera/0/%s"
	         "?ClientID=77&ClientTransactionID=1", member);
	return http_small(host, port, "GET", path, NULL, json, cap, err, errlen);
}

static int alpaca_bool(const char *host, int port, const char *member, int *out,
                       char *err, size_t errlen) {
	char json[512];
	const char *v;

	if (alpaca_value(host, port, member, json, sizeof(json), err, errlen) != 0)
		return -1;
	if ((v = strstr(json, "\"Value\":")) == NULL) {
		snprintf(err, errlen, "no Value in %s response", member);
		return -1;
	}
	*out = strncmp(v + 8, "true", 4) == 0;
	return 0;
}

static int alpaca_int(const char *host, int port, const char *member, int *out,
                      char *err, size_t errlen) {
	char json[512];
	const char *v;

	if (alpaca_value(host, port, member, json, sizeof(json), err, errlen) != 0)
		return -1;
	if ((v = strstr(json, "\"Value\":")) == NULL) {
		snprintf(err, errlen, "no Value in %s response", member);
		return -1;
	}
	*out = atoi(v + 8);
	return 0;
}

static int camera_capture(guide_source_t *src) {
	/* prefix must be at least as large as http_begin's own header buffer:
	 * one read() can pull the headers and several KB of body together. */
	char path[256], form[160], json[512], prefix[4096];
	size_t npix = (size_t)src->width * src->height;
	size_t clen = 0, have = 0, want = npix * 2;
	int ready = 0, spins = 0, fd = -1;
	int32_t hdr[11];
	struct timespec ts;

	snprintf(path, sizeof(path), "/api/v1/camera/0/startexposure");
	snprintf(form, sizeof(form),
	         "Duration=%.6f&Light=true&ClientID=77&ClientTransactionID=1",
	         src->exposure_s);
	if (http_small(src->host, src->port, "PUT", path, form, json, sizeof(json),
	               src->err, sizeof(src->err)) != 0)
		return -1;

	/* Poll rather than sleep out the exposure: the real wait includes the
	 * driver's settle discards on a control change, which is not derivable
	 * from the requested duration. */
	while (!ready) {
		if (alpaca_bool(src->host, src->port, "imageready", &ready,
		                src->err, sizeof(src->err)) != 0)
			return -1;
		if (ready)
			break;
		if (++spins > 1200) {
			snprintf(src->err, sizeof(src->err), "timed out waiting for imageready");
			return -1;
		}
		/* 100 ms, not 20: every poll is a fresh TCP connection (this client
		 * does not keep-alive), and on a 32 MB board that churn is not free.
		 * Nothing here needs finer resolution than a tenth of a second. */
		ts.tv_sec = 0;
		ts.tv_nsec = 100 * 1000 * 1000;
		nanosleep(&ts, NULL);
	}

	snprintf(path, sizeof(path), "/api/v1/camera/0/imagearray"
	         "?ClientID=77&ClientTransactionID=1");
	if (http_begin(src->host, src->port, "GET", path, "application/imagebytes",
	               NULL, &fd, &clen, prefix, sizeof(prefix), &have,
	               src->err, sizeof(src->err)) != 0)
		return -1;

	if (have < 44 && read_full(fd, prefix + have, 44 - have) == 0)
		have = 44;
	if (have < 44) {
		snprintf(src->err, sizeof(src->err), "imagebytes header truncated");
		close(fd);
		return -1;
	}
	memcpy(hdr, prefix, 44);

	if (hdr[1] != 0) {
		snprintf(src->err, sizeof(src->err), "alpaca error %d", (int)hdr[1]);
		close(fd);
		return -1;
	}
	if (hdr[6] != 8) {
		snprintf(src->err, sizeof(src->err),
		         "expected UInt16 transmission type, got %d", (int)hdr[6]);
		close(fd);
		return -1;
	}
	if (hdr[8] != src->width || hdr[9] != src->height) {
		snprintf(src->err, sizeof(src->err), "frame is %dx%d, expected %dx%d",
		         (int)hdr[8], (int)hdr[9], src->width, src->height);
		close(fd);
		return -1;
	}
	if (clen < (size_t)hdr[4] + want) {
		snprintf(src->err, sizeof(src->err), "imagebytes body too short");
		close(fd);
		return -1;
	}

	/* Skip anything between the 44-byte header and dataStart, then stream the
	 * pixels straight into the frame -- never into a second 6 MB buffer. */
	{
		size_t skip = (size_t)hdr[4] > 44 ? (size_t)hdr[4] - 44 : 0;
		size_t carry = have - 44;
		char junk[256];

		while (skip > 0) {
			size_t take = carry < skip ? carry : skip;
			if (take > 0) {
				memmove(prefix + 44, prefix + 44 + take, carry - take);
				carry -= take;
				skip -= take;
				continue;
			}
			take = skip < sizeof(junk) ? skip : sizeof(junk);
			if (read_full(fd, junk, take) != 0) {
				snprintf(src->err, sizeof(src->err), "short read skipping to dataStart");
				close(fd);
				return -1;
			}
			skip -= take;
		}
		if (carry > want)
			carry = want;
		memcpy(src->pixels, prefix + 44, carry);
		if (read_full(fd, (char *)src->pixels + carry, want - carry) != 0) {
			snprintf(src->err, sizeof(src->err), "short read of pixel data");
			close(fd);
			return -1;
		}
	}
	close(fd);
	src->frame++;
	return 0;
}

/* MemAvailable in kB, or -1 if it cannot be read. */
static long mem_available_kb(void) {
	FILE *f = fopen("/proc/meminfo", "r");
	char line[256];
	long kb = -1;
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1)
			break;
	fclose(f);
	return kb;
}

int guide_source_open_camera(guide_source_t *src, const char *host, int port,
                             double exposure_s) {
	int w = 0, h = 0, maxadu = 1023;
	long avail_kb;

	memset(src, 0, sizeof(*src));
	src->kind = GUIDE_SRC_CAMERA;
	snprintf(src->host, sizeof(src->host), "%s", host);
	src->port = port;
	src->exposure_s = exposure_s;

	/* Ask the camera its own geometry rather than assuming a sensor. */
	if (alpaca_int(host, port, "cameraxsize", &w, src->err, sizeof(src->err)) != 0)
		return -1;
	if (alpaca_int(host, port, "cameraysize", &h, src->err, sizeof(src->err)) != 0)
		return -1;
	if (alpaca_int(host, port, "maxadu", &maxadu, src->err, sizeof(src->err)) != 0)
		maxadu = 1023; /* optional */

	if (w <= 0 || h <= 0) {
		snprintf(src->err, sizeof(src->err), "camera reported %dx%d", w, h);
		return -1;
	}

	/*
	 * Refuse rather than take the daemon down with us. alpacad retains a whole
	 * frame after each exposure and allocates a second while capturing the
	 * next, so pulling frames from a separate process needs room for roughly
	 * three: ours plus alpacad's two. On the RV1103 there is not enough, and
	 * the kernel's OOM killer picks alpacad -- the useful process -- over the
	 * test that caused it. Learned by doing it three times.
	 */
	{
		long need_kb = (long)((size_t)w * h * 2 / 1024) * 3 + 2048;
		avail_kb = mem_available_kb();
		if (avail_kb >= 0 && avail_kb < need_kb) {
			snprintf(src->err, sizeof(src->err),
			         "only %ld kB available, need about %ld kB to pull frames "
			         "from alpacad without risking it being OOM-killed. Use "
			         "--source sim here, and run the camera source on a host "
			         "or on the 256 MB part",
			         avail_kb, need_kb);
			return -1;
		}
	}

	src->width = w;
	src->height = h;
	src->max_adu = (uint16_t)maxadu;
	src->pixels = malloc((size_t)w * h * sizeof(uint16_t));
	if (!src->pixels) {
		snprintf(src->err, sizeof(src->err), "out of memory for a %dx%d frame", w, h);
		return -1;
	}

	src->image.px = src->pixels;
	src->image.width = w;
	src->image.height = h;
	src->image.x_stride = h; /* alpacad wire order: x outer */
	src->image.y_stride = 1;
	src->image.max_adu = src->max_adu;
	src->next = camera_capture;
	return 0;
}

/* ---------------------------------------------------------------- sim src -- */

static int sim_next(guide_source_t *src);

static void sim_inject(uint16_t *px, const guide_image_t *im,
                       double cx, double cy, double peak) {
	const double sigma = 1.6, s2 = 2.0 * sigma * sigma;
	int x0 = (int)(cx - 8), x1 = (int)(cx + 8);
	int y0 = (int)(cy - 8), y1 = (int)(cy + 8);
	int x, y;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= im->width) x1 = im->width - 1;
	if (y1 >= im->height) y1 = im->height - 1;

	for (y = y0; y <= y1; y++) {
		for (x = x0; x <= x1; x++) {
			double dx = x - cx, dy = y - cy;
			double v = peak * exp(-(dx * dx + dy * dy) / s2);
			size_t i = (size_t)x * im->x_stride + (size_t)y * im->y_stride;
			double acc = (double)px[i] + v;
			px[i] = acc > im->max_adu ? im->max_adu : (uint16_t)(acc + 0.5);
		}
	}
}

int guide_source_open_sim(guide_source_t *src, const char *base_path,
                          int width, int height, int nstars) {
	size_t npix = (size_t)width * height;
	int i;
	int px, py;

	memset(src, 0, sizeof(*src));
	src->kind = GUIDE_SRC_SIM;
	src->width = width;
	src->height = height;
	src->max_adu = 1023;
	src->nstars = nstars > GUIDE_SIM_MAX_STARS ? GUIDE_SIM_MAX_STARS : nstars;

	src->pixels = malloc(npix * sizeof(uint16_t));
	if (!src->pixels) {
		snprintf(src->err, sizeof(src->err), "out of memory for %.1f MB",
		         npix * 2 / 1e6);
		return -1;
	}

	if (base_path) {
		FILE *f = fopen(base_path, "rb");
		if (!f) {
			snprintf(src->err, sizeof(src->err), "%s: %s", base_path, strerror(errno));
			return -1;
		}
		if (fread(src->pixels, sizeof(uint16_t), npix, f) != npix) {
			snprintf(src->err, sizeof(src->err),
			         "%s: short read, expected %dx%d uint16", base_path, width, height);
			fclose(f);
			return -1;
		}
		fclose(f);
	} else {
		/* Flat pedestal with a little shot noise. Honest enough to run the
		 * loop, but it will flatter the detector compared to a real frame --
		 * prefer a real base whenever one is available. */
		unsigned int seed = 12345;
		size_t k;
		for (k = 0; k < npix; k++) {
			seed = seed * 1103515245u + 12345u;
			src->pixels[k] = (uint16_t)(64 + ((seed >> 16) & 0x0f));
		}
	}

	src->image.px = src->pixels;
	src->image.width = width;
	src->image.height = height;
	src->image.x_stride = height;
	src->image.y_stride = 1;
	src->image.max_adu = src->max_adu;

	/* Deliberately non-integer positions, so a centroid that silently rounds
	 * to the peak pixel shows up immediately. */
	for (i = 0; i < src->nstars; i++) {
		src->stars[i].x = 220.0 + (i * 331) % (width - 460) + 0.37 * (i + 1);
		src->stars[i].y = 200.0 + (i * 217) % (height - 420) + 0.21 * (i + 1);
		src->stars[i].peak = 420.0 - i * 45.0;

		/* Save the pristine pixels under this star, before anything is drawn. */
		for (py = 0; py < GUIDE_SIM_PATCH_W; py++) {
			int sy = (int)src->stars[i].y - GUIDE_SIM_PATCH_R + py;
			for (px = 0; px < GUIDE_SIM_PATCH_W; px++) {
				int sx = (int)src->stars[i].x - GUIDE_SIM_PATCH_R + px;
				uint16_t v = 0;
				if (sx >= 0 && sx < width && sy >= 0 && sy < height)
					v = src->pixels[(size_t)sx * src->image.x_stride +
					                (size_t)sy * src->image.y_stride];
				src->stars[i].patch[py * GUIDE_SIM_PATCH_W + px] = v;
			}
		}
	}
	src->next = sim_next;
	return 0;
}

static int sim_next(guide_source_t *src) {
	long f = src->frame;
	int i, px, py;

	/* A slow periodic term plus a slower one, which is what a worm-driven
	 * mount actually does; the loop above must correct it without knowing. */
	src->drift_x = 1.6 * sin((double)f / 5.5) + 0.25 * sin((double)f / 1.7);
	src->drift_y = 0.9 * sin((double)f / 9.0 + 1.0);

	/* Restore only what the previous frame's stars touched, then redraw. */
	for (i = 0; i < src->nstars; i++) {
		for (py = 0; py < GUIDE_SIM_PATCH_W; py++) {
			int sy = (int)src->stars[i].y - GUIDE_SIM_PATCH_R + py;
			if (sy < 0 || sy >= src->height)
				continue;
			for (px = 0; px < GUIDE_SIM_PATCH_W; px++) {
				int sx = (int)src->stars[i].x - GUIDE_SIM_PATCH_R + px;
				if (sx < 0 || sx >= src->width)
					continue;
				src->pixels[(size_t)sx * src->image.x_stride +
				            (size_t)sy * src->image.y_stride] =
				    src->stars[i].patch[py * GUIDE_SIM_PATCH_W + px];
			}
		}
	}
	for (i = 0; i < src->nstars; i++)
		sim_inject(src->pixels, &src->image,
		           src->stars[i].x + src->drift_x,
		           src->stars[i].y + src->drift_y,
		           src->stars[i].peak);
	src->frame++;
	return 0;
}

/* ------------------------------------------------------------------ api -- */

int guide_source_next(guide_source_t *src) {
	return src->next ? src->next(src) : -1;
}

void guide_source_set_exposure(guide_source_t *src, double exposure_s) {
	src->exposure_s = exposure_s;
}

void guide_source_release(guide_source_t *src) {
	/* Only the V4L2 source allocates a fresh buffer per frame. The simulated
	 * source draws into a buffer it owns and must keep, and the HTTP camera
	 * source reuses one allocation, so for those this is a no-op. */
	if (src->kind != GUIDE_SRC_V4L2)
		return;
	free(src->pixels);
	src->pixels = NULL;
	src->image.px = NULL;
}

void guide_source_truth(const guide_source_t *src, double *dx, double *dy) {
	if (src->kind == GUIDE_SRC_SIM) {
		*dx = src->drift_x;
		*dy = src->drift_y;
	} else {
		*dx = 0.0;
		*dy = 0.0;
	}
}

const char *guide_source_name(const guide_source_t *src) {
	switch (src->kind) {
	case GUIDE_SRC_CAMERA: return "camera";
	case GUIDE_SRC_V4L2:   return "camera";
	default:               return "simulated";
	}
}

void guide_source_close(guide_source_t *src) {
	free(src->pixels);
	src->pixels = NULL;
}
