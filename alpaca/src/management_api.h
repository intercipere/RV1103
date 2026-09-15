#ifndef MANAGEMENT_API_H
#define MANAGEMENT_API_H

struct mg_context;

/* Registers /management/apiversions, /management/v1/description, and
 * /management/v1/configureddevices -- the endpoints Alpaca clients use to
 * discover what this server hosts before talking to a specific device. */
void management_api_register(struct mg_context *ctx);

#endif
