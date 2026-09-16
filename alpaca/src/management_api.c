#include "management_api.h"
#include "http_util.h"
#include "util.h"

static long client_txn_from(struct mg_connection *conn) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	params_t p;
	params_parse(ri->query_string, &p);
	return params_get_int(&p, "clienttransactionid", 0);
}

static int h_apiversions(struct mg_connection *conn, void *cbdata) {
	(void)cbdata;
	char buf[256];
	alpaca_response(buf, sizeof(buf), "[1]", client_txn_from(conn), 0, "");
	send_json(conn, buf);
	return 200;
}

static int h_description(struct mg_connection *conn, void *cbdata) {
	(void)cbdata;
	char buf[512];
	alpaca_response(buf, sizeof(buf),
	                 "{\"ServerName\":\"OpenAstroGuider Alpaca\","
	                 "\"Manufacturer\":\"OpenAstroGuider\","
	                 "\"ManufacturerVersion\":\"0.1\",\"Location\":\"RV1103\"}",
	                 client_txn_from(conn), 0, "");
	send_json(conn, buf);
	return 200;
}

static int h_configureddevices(struct mg_connection *conn, void *cbdata) {
	(void)cbdata;
	char buf[768];
	/* Two devices on one Alpaca server: the camera, and a Switch device for
	 * the lens dew heater (ASCOM's Camera interface has nowhere to put a
	 * heater, and Switch is the interface intended for auxiliary controls).
	 * Clients enumerate and connect to each independently. */
	alpaca_response(
	    buf, sizeof(buf),
	    "[{\"DeviceName\":\"OpenAstroGuider Camera\",\"DeviceType\":\"Camera\","
	    "\"DeviceNumber\":0,\"UniqueID\":\"openastroguider-camera-0\"},"
	    "{\"DeviceName\":\"OpenAstroGuider Dew Heater\",\"DeviceType\":\"Switch\","
	    "\"DeviceNumber\":0,\"UniqueID\":\"openastroguider-switch-0\"}]",
	    client_txn_from(conn), 0, "");
	send_json(conn, buf);
	return 200;
}

void management_api_register(struct mg_context *ctx) {
	mg_set_request_handler(ctx, "/management/apiversions", h_apiversions, NULL);
	mg_set_request_handler(ctx, "/management/v1/description", h_description,
	                        NULL);
	mg_set_request_handler(ctx, "/management/v1/configureddevices",
	                        h_configureddevices, NULL);
}
