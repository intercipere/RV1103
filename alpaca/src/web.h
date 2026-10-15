#ifndef WEB_H
#define WEB_H

#include <stddef.h>

struct mg_connection;

/*
 * The browser-facing pages, kept as ordinary files under alpaca/web/ and
 * embedded at build time by tools/embed_web.py -- pre-gzipped, so the device
 * serves them with no zlib and a fraction of the size. Paths are relative to
 * web/, e.g. "guide/index.html".
 */
typedef struct {
	const char *path;
	const char *mime;
	const char *etag;
	const unsigned char *gz;
	size_t gz_len;
} web_asset_t;

extern const web_asset_t web_assets[]; /* generated; ends with a NULL path */

/*
 * Serves one asset. Answers 304 when the browser already holds this version
 * (ETag), so a reload costs a few hundred bytes. Returns the HTTP status sent.
 *
 * If web_set_dev_dir() was called, files are read from that directory instead,
 * uncompressed and uncached -- for editing the page without rebuilding, on a
 * host. alpacad never sets it.
 */
int web_send(struct mg_connection *conn, const char *path);

/* Serves /ui/<path> for every asset. */
struct mg_context;
void web_register(struct mg_context *ctx);

void web_set_dev_dir(const char *dir);

#endif
