#ifndef COMMON_API_H
#define COMMON_API_H

#include "util.h"

#include <pthread.h>

struct mg_connection;

/*
 * Identifies which device the common members are being answered for. Every
 * Alpaca device on this server (camera 0, switch 0, ...) has its own name,
 * description, interface version and -- importantly -- its own Connected
 * flag, since ASCOM clients connect to each device independently.
 */
typedef struct {
	const char *description;
	const char *driverinfo;
	int interface_version; /* e.g. 3 for ICameraV3, 2 for ISwitchV2 */
	const char *name;
	int *connected;             /* guarded by *lock */
	pthread_mutex_t *lock;
} common_device_t;

/*
 * Handles the Alpaca "common device" members shared by every device type:
 * connected, connecting, description, driverinfo, driverversion,
 * interfaceversion, name, supportedactions. Writes the HTTP response
 * directly via `conn` and returns 1 if `member` was recognized; returns 0
 * (writing nothing) if not, so the caller can fall through to
 * device-type-specific members.
 */
int common_dispatch(struct mg_connection *conn, const common_device_t *dev,
                     const char *member, const char *method,
                     const params_t *params, long client_txn_id);

#endif
