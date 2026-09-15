#ifndef COMMON_API_H
#define COMMON_API_H

#include "util.h"

struct mg_connection;

/*
 * Handles the seven Alpaca "common device" members shared by every device
 * type: connected, connecting, description, driverinfo, driverversion,
 * interfaceversion, name, supportedactions. Writes the HTTP response
 * directly via `conn` and returns 1 if `member` was recognized; returns 0
 * (writing nothing) if not, so the caller can fall through to
 * device-type-specific members.
 */
int common_dispatch(struct mg_connection *conn, const char *member,
                     const char *method, const params_t *params,
                     long client_txn_id);

#endif
