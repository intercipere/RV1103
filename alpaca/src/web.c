#include "web.h"

#include "civetweb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_dev_dir;

void web_set_dev_dir(const char *dir) { g_dev_dir = dir; }

static const char *mime_of(const char *path) {
	const char *dot = strrchr(path, '.');
	if (!dot) return "application/octet-stream";
	if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
	if (!strcmp(dot, ".css")) return "text/css; charset=utf-8";
	if (!strcmp(dot, ".js")) return "text/javascript; charset=utf-8";
	if (!strcmp(dot, ".json")) return "application/json";
	if (!strcmp(dot, ".svg")) return "image/svg+xml";
	return "application/octet-stream";
}

static int send_from_dir(struct mg_connection *conn, const char *path) {
	char full[512];
	FILE *f;
	long n;
	char *buf;
	char len[32];

	/* Dev only, but still refuse to walk out of the directory. */
	if (strstr(path, "..")) {
		mg_send_http_error(conn, 400, "bad path");
		return 400;
	}
	snprintf(full, sizeof(full), "%s/%s", g_dev_dir, path);
	f = fopen(full, "rb");
	if (!f) {
		mg_send_http_error(conn, 404, "not found");
		return 404;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(n > 0 ? (size_t)n : 1);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(buf);
		return 500;
	}
	fclose(f);
	mg_response_header_start(conn, 200);
	mg_response_header_add(conn, "Content-Type", mime_of(path), -1);
	mg_response_header_add(conn, "Cache-Control", "no-store", -1);
	snprintf(len, sizeof(len), "%ld", n);
	mg_response_header_add(conn, "Content-Length", len, -1);
	mg_response_header_send(conn);
	mg_write(conn, buf, (size_t)n);
	free(buf);
	return 200;
}

int web_send(struct mg_connection *conn, const char *path) {
	const web_asset_t *a;
	const char *inm;
	char len[32];

	if (g_dev_dir)
		return send_from_dir(conn, path);

	for (a = web_assets; a->path; a++)
		if (!strcmp(a->path, path))
			break;
	if (!a->path) {
		mg_send_http_error(conn, 404, "not found");
		return 404;
	}

	/* no-cache means "revalidate", not "do not store": with the ETag the
	 * browser keeps its copy and a reload costs one small 304. */
	inm = mg_get_header(conn, "If-None-Match");
	if (inm && !strcmp(inm, a->etag)) {
		mg_response_header_start(conn, 304);
		mg_response_header_add(conn, "ETag", a->etag, -1);
		mg_response_header_add(conn, "Cache-Control", "no-cache", -1);
		mg_response_header_send(conn);
		return 304;
	}

	mg_response_header_start(conn, 200);
	mg_response_header_add(conn, "Content-Type", a->mime, -1);
	/* Always gzip: every browser accepts it, and there is no zlib here to
	 * produce anything else. (curl needs --compressed to show it.) */
	mg_response_header_add(conn, "Content-Encoding", "gzip", -1);
	mg_response_header_add(conn, "Vary", "Accept-Encoding", -1);
	mg_response_header_add(conn, "ETag", a->etag, -1);
	mg_response_header_add(conn, "Cache-Control", "no-cache", -1);
	snprintf(len, sizeof(len), "%zu", a->gz_len);
	mg_response_header_add(conn, "Content-Length", len, -1);
	mg_response_header_send(conn);
	mg_write(conn, a->gz, a->gz_len);
	return 200;
}

static int h_ui(struct mg_connection *conn, void *cbdata) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	const char *uri = ri->local_uri ? ri->local_uri : ri->request_uri;
	(void)cbdata;
	if (strncmp(uri, "/ui/", 4) != 0) {
		mg_send_http_error(conn, 404, "not found");
		return 404;
	}
	return web_send(conn, uri + 4);
}

void web_register(struct mg_context *ctx) {
	/* civetweb matches "/ui" itself and "/ui/anything" */
	mg_set_request_handler(ctx, "/ui", h_ui, NULL);
}
