#ifndef CAMERA_API_H
#define CAMERA_API_H

struct mg_context;

/* Registers the single "/api/v1/camera/0/" wildcard pattern and dispatches
 * to common_dispatch() for the seven shared members, falling through to
 * the camera-specific members implemented here. */
void camera_api_register(struct mg_context *ctx);

#endif
