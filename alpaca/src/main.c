/*
 * OpenAstroGuider Alpaca driver -- entry point.
 * See ../README.md for the design and current status.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "camera_api.h"
#include "civetweb.h"
#include "device_state.h"
#include "discovery.h"
#include "management_api.h"
#include "setup_api.h"
#include "switch_api.h"
#include "v4l2_capture.h"

#define ALPACA_TCP_PORT 11111

int main(void) {
	/* stderr is fully block-buffered (not line-buffered) once redirected to
	 * a regular file, as S60alpacad does -- without this, diagnostic
	 * fprintf(stderr, ...) calls elsewhere in this program can sit in libc's
	 * internal buffer indefinitely instead of reaching /tmp/alpacad.log,
	 * making the log look empty even when requests are being handled. */
	setvbuf(stderr, NULL, _IONBF, 0);

	device_state_init();
	fprintf(stderr, "detected sensor: %s (%dx%d, row_time_us=%.3f)\n",
	        g_device.sensor->display_name, g_device.sensor->width,
	        g_device.sensor->height, g_device.sensor->row_time_us);

	/* Gain and vertical_blanking are pinned once, here, rather than per
	 * exposure -- and before streaming starts, so the very first frame is
	 * already captured with them. See camera_api.c. */
	camera_apply_fixed_sensor_settings(g_device.sensor);

	/* Opened once here and kept streaming for the daemon's whole lifetime --
	 * see v4l2_capture.h for why (avoids a ~515-523ms open/STREAMON/.../
	 * STREAMOFF/close teardown on every single exposure). If this fails
	 * there's no point starting the HTTP server at all. */
	if (v4l2_capture_init(g_device.sensor) != 0) {
		fprintf(stderr, "v4l2_capture_init failed for %s\n",
		        g_device.sensor->video_path);
		return 1;
	}

	/* Restores the persisted dew-heater state and drives its GPIO before any
	 * client can connect, so the heater comes back on by itself after a power
	 * cycle rather than waiting to be re-enabled. */
	switch_api_init();

	discovery_start(ALPACA_TCP_PORT);

	const char *options[] = {"listening_ports", "11111", "num_threads", "4",
	                          NULL};
	struct mg_callbacks callbacks;
	memset(&callbacks, 0, sizeof(callbacks));

	mg_init_library(0);
	struct mg_context *ctx = mg_start(&callbacks, NULL, options);
	if (ctx == NULL) {
		fprintf(stderr, "mg_start failed\n");
		return 1;
	}

	management_api_register(ctx);
	camera_api_register(ctx);
	switch_api_register(ctx);
	/* Browser-facing setup page (/, /setup, /setup/v1/<type>/<n>/setup) --
	 * what a client's "Settings" button actually opens for an Alpaca device,
	 * and the only way to reach the dew heater from a client with no ASCOM
	 * Switch support. See setup_api.h. */
	setup_api_register(ctx);

	printf("alpacad listening on :%d (discovery UDP :32227)\n", ALPACA_TCP_PORT);
	for (;;)
		sleep(3600);

	mg_stop(ctx);
	mg_exit_library();
	return 0;
}
