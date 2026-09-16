#include "common_api.h"
#include "device_state.h"
#include "http_util.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

int common_dispatch(struct mg_connection *conn, const common_device_t *dev,
                     const char *member, const char *method,
                     const params_t *params, long client_txn_id) {
	char buf[512];

	if (strcasecmp(member, "connected") == 0) {
		if (strcasecmp(method, "PUT") == 0) {
			int val = params_get_bool(params, "connected", 0);
			pthread_mutex_lock(dev->lock);
			*dev->connected = val;
			pthread_mutex_unlock(dev->lock);
			alpaca_response(buf, sizeof(buf), NULL, client_txn_id, 0, "");
		} else {
			pthread_mutex_lock(dev->lock);
			int val = *dev->connected;
			pthread_mutex_unlock(dev->lock);
			alpaca_response_bool(buf, sizeof(buf), val, client_txn_id);
		}
		send_json(conn, buf);
		return 1;
	}
	if (strcasecmp(member, "connecting") == 0) {
		/* Connect()/Disconnect() async lifecycle isn't implemented --
		 * connected is set synchronously by the "connected" PUT above --
		 * so this is always false. */
		alpaca_response_bool(buf, sizeof(buf), 0, client_txn_id);
		send_json(conn, buf);
		return 1;
	}
	if (strcasecmp(member, "description") == 0) {
		alpaca_response_string(buf, sizeof(buf), dev->description,
		                        client_txn_id);
		send_json(conn, buf);
		return 1;
	}
	if (strcasecmp(member, "driverinfo") == 0) {
		alpaca_response_string(buf, sizeof(buf), dev->driverinfo,
		                        client_txn_id);
		send_json(conn, buf);
		return 1;
	}
	if (strcasecmp(member, "driverversion") == 0) {
		/* Reads /etc/openastroguider-version, written at build time by
		 * luckfox-astroguider-oem-pre.sh (built=<UTC timestamp>
		 * git=<short hash>) -- lets "which build is this" be answered by
		 * asking the running device instead of trusting which dated
		 * IMAGE/..._RELEASE_TEST folder got flashed. Falls back to "0.1
		 * (no version file)" for a rootfs built before this existed. */
		char version[128] = "0.1 (no version file)";
		FILE *f = fopen("/etc/openastroguider-version", "r");
		if (f != NULL) {
			if (fgets(version, sizeof(version), f) != NULL)
				version[strcspn(version, "\n")] = '\0';
			fclose(f);
		}
		alpaca_response_string(buf, sizeof(buf), version, client_txn_id);
		send_json(conn, buf);
		return 1;
	}
	if (strcasecmp(member, "interfaceversion") == 0) {
		alpaca_response_int(buf, sizeof(buf), dev->interface_version,
		                     client_txn_id);
		send_json(conn, buf);
		return 1;
	}
	if (strcasecmp(member, "name") == 0) {
		alpaca_response_string(buf, sizeof(buf), dev->name, client_txn_id);
		send_json(conn, buf);
		return 1;
	}
	if (strcasecmp(member, "supportedactions") == 0) {
		alpaca_response(buf, sizeof(buf), "[]", client_txn_id, 0, "");
		send_json(conn, buf);
		return 1;
	}

	return 0;
}
