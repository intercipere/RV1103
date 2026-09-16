#ifndef CAMERA_API_H
#define CAMERA_API_H

struct mg_context;
#include "sensor.h"

/* Registers the single "/api/v1/camera/0/" wildcard pattern and dispatches
 * to common_dispatch() for the seven shared members, falling through to
 * the camera-specific members implemented here. */
void camera_api_register(struct mg_context *ctx);

/* Pins analogue_gain and vertical_blanking to their fixed values (see
 * camera_api.c). Call once at startup, before streaming starts. */
void camera_apply_fixed_sensor_settings(const sensor_desc_t *s);

#endif
