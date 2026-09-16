#ifndef HTTP_UTIL_H
#define HTTP_UTIL_H

#include <string.h>
#include <strings.h>
#include "civetweb.h"
#include "util.h"

static inline void send_json(struct mg_connection *conn, const char *body) {
	mg_send_http_ok(conn, "application/json", (long long)strlen(body));
	mg_write(conn, body, strlen(body));
}

/* Reads the PUT body (application/x-www-form-urlencoded) into `out`, merging
 * in any query-string parameters too -- Alpaca clients are only required to
 * use the body for PUT, but tolerating both is harmless and matches how
 * permissive real-world clients tend to be. Shared by every device type's
 * dispatcher. */
static inline void parse_request_params(struct mg_connection *conn,
                                         params_t *out) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	params_parse(ri->query_string, out);

	if (strcasecmp(ri->request_method, "PUT") == 0) {
		char body[2048];
		int n = mg_read(conn, body, sizeof(body) - 1);
		if (n > 0) {
			body[n] = '\0';
			params_t body_params;
			params_parse(body, &body_params);
			for (int i = 0; i < body_params.count && out->count < PARAMS_MAX; i++)
				out->params[out->count++] = body_params.params[i];
		}
	}
}

#endif
