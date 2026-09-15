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

#define ALPACA_TCP_PORT 11111

int main(void) {
	device_state_init();
	fprintf(stderr, "detected sensor: %s (%dx%d, row_time_us=%.3f)\n",
	        g_device.sensor->display_name, g_device.sensor->width,
	        g_device.sensor->height, g_device.sensor->row_time_us);

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

	printf("alpacad listening on :%d (discovery UDP :32227)\n", ALPACA_TCP_PORT);
	for (;;)
		sleep(3600);

	mg_stop(ctx);
	mg_exit_library();
	return 0;
}
