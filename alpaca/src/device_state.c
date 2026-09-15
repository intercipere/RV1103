#include "device_state.h"
#include <string.h>

device_state_t g_device;

void device_state_init(void) {
	memset(&g_device, 0, sizeof(g_device));
	pthread_mutex_init(&g_device.lock, NULL);
	g_device.sensor = sensor_detect();
	g_device.state = CAM_IDLE;
	g_device.gain = 128; /* 1x, matches the subdev's reported default */
}
