#ifndef HTTP_UTIL_H
#define HTTP_UTIL_H

#include <string.h>
#include "civetweb.h"

static inline void send_json(struct mg_connection *conn, const char *body) {
	mg_send_http_ok(conn, "application/json", (long long)strlen(body));
	mg_write(conn, body, strlen(body));
}

#endif
