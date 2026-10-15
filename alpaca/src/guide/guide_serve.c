/*
 * guide_serve -- the /guide page and its guide loop, served from a host with
 * no board attached.
 *
 * Links the same guide_loop.c and guide_api.c that alpacad does, with the
 * simulated frame source (and its simulated mount) standing in for the camera.
 * It exists so the page and the full select -> calibrate -> guide cycle can be
 * driven in a real browser while the board is unplugged; everything except the
 * V4L2 capture is the code that runs on the device.
 *
 *   make guide_serve && ./guide_serve [port] [--web ../../web]
 *   then open http://127.0.0.1:8090/guide. With --web the page is read from
 *   disk on every request, so edits to web/ show on a browser reload.
 */
#include "civetweb.h"
#include "guide_api.h"
#include "guide_loop.h"
#include "guide_source.h"
#include "sensor.h"
#include "web.h"
#include "guide_settings.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* No camera on a host. The page's Camera button reaches this and reports it,
 * which is also what the device does when V4L2 is unavailable. */
int guide_source_open_v4l2(guide_source_t *src, const void *desc, double exposure_s) {
	(void)desc;
	(void)exposure_s;
	memset(src, 0, sizeof(*src));
	snprintf(src->err, sizeof(src->err), "no camera on this host -- use Simulated");
	return -1;
}

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

int main(int argc, char **argv) {
	static sensor_desc_t desc; /* only width and height are read */
	char port[16];
	const char *opts[] = { "listening_ports", port, "num_threads", "4", NULL };
	struct mg_callbacks cb;
	struct mg_context *ctx;

	{
		int i;
		const char *p = NULL;
		for (i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "--web") && i + 1 < argc)
				web_set_dev_dir(argv[++i]);
			else
				p = argv[i];
		}
		snprintf(port, sizeof(port), "%s", p ? p : "8090");
	}
	desc.width = 2304;
	desc.height = 1296;
	desc.pixel_size_um_x = desc.pixel_size_um_y = 2.45; /* SC3336, as on the board */

	mg_init_library(0);
	memset(&cb, 0, sizeof(cb));
	ctx = mg_start(&cb, NULL, opts);
	if (!ctx) {
		fprintf(stderr, "cannot listen on %s\n", port);
		return 1;
	}
	guide_settings_init("guide.cfg"); /* in the working directory */
	if (guide_loop_init(&desc) != 0)
		return 1;
	guide_api_register(ctx);
	web_register(ctx);
	/* Start on the simulated source, since it is the only one here. */
	guide_loop_set_source(GUIDE_SRC_SIM);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	printf("serving http://127.0.0.1:%s/guide (simulated source and mount)\n", port);
	while (!stop)
		sleep(1);
	mg_stop(ctx);
	return 0;
}
