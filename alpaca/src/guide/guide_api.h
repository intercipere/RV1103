#ifndef GUIDE_API_H
#define GUIDE_API_H

struct mg_context;

/*
 * Serves the guider page and its data endpoints:
 *
 *   GET  /guide            the page itself
 *   GET  /guide/state      JSON: state, star, error history, RMS, timings
 *   GET  /guide/preview    raw 8-bit greyscale, geometry in X-Preview-* headers
 *   PUT  /guide/control    start|stop|reselect|source|exposure|minmove|algo
 *
 * Deliberately outside /api/v1: this is not an ASCOM interface and pretending
 * otherwise would be misleading. ASCOM has no guider device type (see
 * Guiding/README.md), so there is nothing to conform to here.
 */
void guide_api_register(struct mg_context *ctx);

#endif
