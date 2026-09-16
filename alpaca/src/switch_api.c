#include "switch_api.h"

#include "civetweb.h"
#include "common_api.h"
#include "http_util.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* Only one switch today. Kept as a constant rather than a literal so adding a
 * second auxiliary control later is a table change, not a hunt for 1s and 0s. */
#define SWITCH_COUNT 1
#define SWITCH_DEW_HEATER 0

/* Persisted on /userdata, the board's small ext4 partition reserved for exactly
 * this (config/calibration; /tmp is a RAM disk and would lose it on every power
 * cut). Restored and re-applied to the GPIO at daemon start, so the heater
 * survives unplugging the camera -- an unattended imaging rig should not need
 * someone to re-tick a box after every power cycle. */
#define DEW_STATE_PATH "/userdata/dewheater.state"

/* The heater GPIO is board wiring, not a user setting: it is unknown on the
 * Luckfox prototype and will be fixed by the custom PCB. Set the
 * OAG_DEWHEATER_GPIO environment variable (see S60alpacad) to the sysfs GPIO
 * number to bind it. Left unset, the switch still works as a logical, persisted
 * control and simply drives nothing -- which is what lets the Alpaca side be
 * finished and tested before the hardware exists. */
#define DEW_GPIO_ENV "OAG_DEWHEATER_GPIO"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_connected;
static int g_dew_on;
static int g_gpio = -1;      /* sysfs GPIO number, -1 = not configured */
static int g_gpio_ready;     /* exported + direction set */

static int write_file(const char *path, const char *val) {
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	size_t len = strlen(val);
	ssize_t n = write(fd, val, len);
	close(fd);
	return (n == (ssize_t)len) ? 0 : -1;
}

/* Exports the pin and sets it to output. Re-exporting an already-exported pin
 * fails with EBUSY, which is not an error for us -- it just means a previous
 * run (or something else) already claimed it. */
static int gpio_prepare(int pin) {
	char buf[64], path[128];
	snprintf(buf, sizeof(buf), "%d", pin);
	if (write_file("/sys/class/gpio/export", buf) != 0 && errno != EBUSY) {
		snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", pin);
		if (access(path, F_OK) != 0) {
			fprintf(stderr, "[switch] gpio %d: export failed (%s)\n", pin,
			        strerror(errno));
			return -1;
		}
	}
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
	if (write_file(path, "out") != 0) {
		fprintf(stderr, "[switch] gpio %d: direction failed (%s)\n", pin,
		        strerror(errno));
		return -1;
	}
	return 0;
}

static void gpio_apply(int on) {
	if (g_gpio < 0 || !g_gpio_ready)
		return;
	char path[128];
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", g_gpio);
	if (write_file(path, on ? "1" : "0") != 0)
		fprintf(stderr, "[switch] gpio %d: write failed (%s)\n", g_gpio,
		        strerror(errno));
}

/* fsync before close: the board is USB-bus-powered, so "unplug" is an unclean
 * power cut. Without this the new state can sit in page cache and be lost
 * exactly when persistence is the point. */
static void dew_state_save(int on) {
	int fd = open(DEW_STATE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "[switch] cannot write %s (%s)\n", DEW_STATE_PATH,
		        strerror(errno));
		return;
	}
	const char *v = on ? "1\n" : "0\n";
	if (write(fd, v, strlen(v)) < 0)
		fprintf(stderr, "[switch] write %s failed (%s)\n", DEW_STATE_PATH,
		        strerror(errno));
	fsync(fd);
	close(fd);
}

static int dew_state_load(void) {
	FILE *f = fopen(DEW_STATE_PATH, "r");
	if (f == NULL)
		return 0; /* never set -> off */
	int v = 0;
	if (fscanf(f, "%d", &v) != 1)
		v = 0;
	fclose(f);
	return v ? 1 : 0;
}

void switch_api_init(void) {
	const char *env = getenv(DEW_GPIO_ENV);
	if (env != NULL && *env != '\0') {
		char *end = NULL;
		long pin = strtol(env, &end, 10);
		if (end != env && *end == '\0' && pin >= 0) {
			g_gpio = (int)pin;
			g_gpio_ready = (gpio_prepare(g_gpio) == 0);
		} else {
			fprintf(stderr, "[switch] ignoring malformed %s=\"%s\"\n",
			        DEW_GPIO_ENV, env);
		}
	}

	g_dew_on = dew_state_load();
	gpio_apply(g_dew_on);

	/* Distinguish "no pin was asked for" from "a pin was asked for and could
	 * not be claimed" -- conflating them would send whoever wires up the PCB
	 * looking in the wrong place. */
	char gpio_status[64];
	if (g_gpio < 0)
		snprintf(gpio_status, sizeof(gpio_status), "no pin set");
	else if (g_gpio_ready)
		snprintf(gpio_status, sizeof(gpio_status), "gpio %d ready", g_gpio);
	else
		snprintf(gpio_status, sizeof(gpio_status), "gpio %d UNAVAILABLE",
		         g_gpio);
	fprintf(stderr, "[switch] dew heater %s at startup (%s)\n",
	        g_dew_on ? "ON" : "off", gpio_status);
	if (!g_gpio_ready)
		fprintf(stderr,
		        "[switch] heater state is tracked and persisted but drives no "
		        "pin (set %s to the sysfs GPIO number)\n",
		        DEW_GPIO_ENV);
}

static void set_dew(int on) {
	pthread_mutex_lock(&g_lock);
	if (on != g_dew_on) {
		g_dew_on = on;
		gpio_apply(on);
		dew_state_save(on);
		fprintf(stderr, "[switch] dew heater -> %s\n", on ? "ON" : "off");
	}
	pthread_mutex_unlock(&g_lock);
}

static int get_dew(void) {
	pthread_mutex_lock(&g_lock);
	int v = g_dew_on;
	pthread_mutex_unlock(&g_lock);
	return v;
}

/* Every per-switch member takes an Id; anything outside 0..SWITCH_COUNT-1 must
 * be rejected with InvalidValue (0x401) rather than silently treated as 0 --
 * ASCOM's Conform checker probes exactly this. Returns 1 if it answered with
 * an error (caller should stop), 0 if the id is valid. */
static int bad_id(struct mg_connection *conn, const params_t *params,
                   long client_txn_id) {
	long id = params_get_int(params, "id", -1);
	if (id >= 0 && id < SWITCH_COUNT)
		return 0;
	char buf[256];
	alpaca_response_error(buf, sizeof(buf), client_txn_id, 0x401,
	                       "Invalid switch id");
	send_json(conn, buf);
	return 1;
}

static int switch_dispatch(struct mg_connection *conn, void *cbdata) {
	(void)cbdata;
	const struct mg_request_info *ri = mg_get_request_info(conn);
	char buf[512];

	const char *member = strrchr(ri->local_uri, '/');
	member = (member != NULL) ? member + 1 : ri->local_uri;

	params_t params;
	parse_request_params(conn, &params);
	long client_txn_id = params_get_int(&params, "clienttransactionid", 0);

	static const common_device_t dev = {
	    .description = "OpenAstroGuider lens dew heater",
	    .driverinfo = "OpenAstroGuider Alpaca switch driver (alpacad)",
	    .interface_version = 2, /* ISwitchV2 */
	    .name = "OpenAstroGuider Dew Heater",
	    .connected = &g_connected,
	    .lock = &g_lock,
	};
	if (common_dispatch(conn, &dev, member, ri->request_method, &params,
	                     client_txn_id))
		return 200;

	if (strcasecmp(member, "maxswitch") == 0) {
		alpaca_response_int(buf, sizeof(buf), SWITCH_COUNT, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "canwrite") == 0) {
		if (bad_id(conn, &params, client_txn_id)) return 200;
		alpaca_response_bool(buf, sizeof(buf), 1, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "getswitchname") == 0) {
		if (bad_id(conn, &params, client_txn_id)) return 200;
		alpaca_response_string(buf, sizeof(buf), "Dew Heater", client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "getswitchdescription") == 0) {
		if (bad_id(conn, &params, client_txn_id)) return 200;
		alpaca_response_string(buf, sizeof(buf),
		                        "Lens dew heater strip (on/off)",
		                        client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "getswitch") == 0) {
		if (bad_id(conn, &params, client_txn_id)) return 200;
		alpaca_response_bool(buf, sizeof(buf), get_dew(), client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "setswitch") == 0) {
		if (bad_id(conn, &params, client_txn_id)) return 200;
		set_dew(params_get_bool(&params, "state", 0));
		alpaca_response(buf, sizeof(buf), NULL, client_txn_id, 0, "");
		send_json(conn, buf);
		return 200;
	}
	/* The value members mirror the boolean ones over a 0..1 range: ASCOM
	 * clients are free to drive a switch either way, and a boolean device is
	 * just the degenerate case of a value device with step == range. */
	if (strcasecmp(member, "getswitchvalue") == 0) {
		if (bad_id(conn, &params, client_txn_id)) return 200;
		alpaca_response_double(buf, sizeof(buf), get_dew() ? 1.0 : 0.0,
		                        client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "setswitchvalue") == 0) {
		if (bad_id(conn, &params, client_txn_id)) return 200;
		double v = params_get_double(&params, "value", 0.0);
		if (v < 0.0 || v > 1.0) {
			alpaca_response_error(buf, sizeof(buf), client_txn_id, 0x401,
			                       "Value out of range (0..1)");
			send_json(conn, buf);
			return 200;
		}
		set_dew(v >= 0.5);
		alpaca_response(buf, sizeof(buf), NULL, client_txn_id, 0, "");
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "minswitchvalue") == 0) {
		if (bad_id(conn, &params, client_txn_id)) return 200;
		alpaca_response_double(buf, sizeof(buf), 0.0, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "maxswitchvalue") == 0) {
		if (bad_id(conn, &params, client_txn_id)) return 200;
		alpaca_response_double(buf, sizeof(buf), 1.0, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "switchstep") == 0) {
		if (bad_id(conn, &params, client_txn_id)) return 200;
		/* step == range, i.e. only the two endpoints are reachable: this is
		 * how ISwitchV2 describes a purely on/off device. */
		alpaca_response_double(buf, sizeof(buf), 1.0, client_txn_id);
		send_json(conn, buf);
		return 200;
	}
	if (strcasecmp(member, "setswitchname") == 0) {
		if (bad_id(conn, &params, client_txn_id)) return 200;
		alpaca_response_error(buf, sizeof(buf), client_txn_id, 0x400,
		                       "Switch names are fixed");
		send_json(conn, buf);
		return 200;
	}

	alpaca_response_error(buf, sizeof(buf), client_txn_id, 0x400,
	                       "Not implemented");
	send_json(conn, buf);
	return 200;
}

void switch_api_register(struct mg_context *ctx) {
	mg_set_request_handler(ctx, "/api/v1/switch/0/*", switch_dispatch, NULL);
}
