#include "util.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_val(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Decodes a URL-encoded string in place (%XX and '+' as space); result is
 * never longer than the input, so in-place is safe. */
static void url_decode(char *s) {
	char *r = s, *w = s;
	while (*r) {
		if (*r == '%' && r[1] && r[2]) {
			int hi = hex_val(r[1]), lo = hex_val(r[2]);
			if (hi >= 0 && lo >= 0) {
				*w++ = (char)((hi << 4) | lo);
				r += 3;
				continue;
			}
		}
		if (*r == '+') {
			*w++ = ' ';
			r++;
			continue;
		}
		*w++ = *r++;
	}
	*w = '\0';
}

static void to_lower_inplace(char *s) {
	for (; *s; s++)
		*s = (char)tolower((unsigned char)*s);
}

void params_parse(const char *qs, params_t *out) {
	out->count = 0;
	if (qs == NULL)
		return;

	const char *p = qs;
	while (*p && out->count < PARAMS_MAX) {
		const char *amp = strchr(p, '&');
		size_t seglen = amp ? (size_t)(amp - p) : strlen(p);

		const char *eq = memchr(p, '=', seglen);
		kv_t *kv = &out->params[out->count];
		kv->key[0] = '\0';
		kv->value[0] = '\0';

		if (eq != NULL) {
			size_t klen = (size_t)(eq - p);
			size_t vlen = seglen - klen - 1;
			if (klen >= PARAMS_KEY_LEN) klen = PARAMS_KEY_LEN - 1;
			if (vlen >= PARAMS_VAL_LEN) vlen = PARAMS_VAL_LEN - 1;
			memcpy(kv->key, p, klen);
			kv->key[klen] = '\0';
			memcpy(kv->value, eq + 1, vlen);
			kv->value[vlen] = '\0';
		} else {
			size_t klen = seglen;
			if (klen >= PARAMS_KEY_LEN) klen = PARAMS_KEY_LEN - 1;
			memcpy(kv->key, p, klen);
			kv->key[klen] = '\0';
		}

		url_decode(kv->key);
		url_decode(kv->value);
		to_lower_inplace(kv->key);
		out->count++;

		if (!amp)
			break;
		p = amp + 1;
	}
}

const char *params_get(const params_t *p, const char *key) {
	for (int i = 0; i < p->count; i++)
		if (strcmp(p->params[i].key, key) == 0)
			return p->params[i].value;
	return NULL;
}

long params_get_int(const params_t *p, const char *key, long def) {
	const char *v = params_get(p, key);
	if (v == NULL || v[0] == '\0')
		return def;
	return strtol(v, NULL, 10);
}

double params_get_double(const params_t *p, const char *key, double def) {
	const char *v = params_get(p, key);
	if (v == NULL || v[0] == '\0')
		return def;
	return strtod(v, NULL);
}

int params_get_bool(const params_t *p, const char *key, int def) {
	const char *v = params_get(p, key);
	if (v == NULL || v[0] == '\0')
		return def;
	return (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
}

static pthread_mutex_t txn_lock = PTHREAD_MUTEX_INITIALIZER;
static long next_server_txn_id = 1;

static long next_server_txn(void) {
	pthread_mutex_lock(&txn_lock);
	long id = next_server_txn_id++;
	pthread_mutex_unlock(&txn_lock);
	return id;
}

void alpaca_response(char *buf, size_t buflen, const char *value_json,
                      long client_txn_id, int error_number,
                      const char *error_message) {
	long server_txn_id = next_server_txn();
	if (value_json != NULL) {
		snprintf(buf, buflen,
		         "{\"Value\":%s,\"ClientTransactionID\":%ld,"
		         "\"ServerTransactionID\":%ld,\"ErrorNumber\":%d,"
		         "\"ErrorMessage\":\"%s\"}",
		         value_json, client_txn_id, server_txn_id, error_number,
		         error_message ? error_message : "");
	} else {
		snprintf(buf, buflen,
		         "{\"ClientTransactionID\":%ld,\"ServerTransactionID\":%ld,"
		         "\"ErrorNumber\":%d,\"ErrorMessage\":\"%s\"}",
		         client_txn_id, server_txn_id, error_number,
		         error_message ? error_message : "");
	}
}

void alpaca_response_bool(char *buf, size_t buflen, int value,
                           long client_txn_id) {
	alpaca_response(buf, buflen, value ? "true" : "false", client_txn_id, 0,
	                 "");
}

void alpaca_response_int(char *buf, size_t buflen, long value,
                          long client_txn_id) {
	char v[32];
	snprintf(v, sizeof(v), "%ld", value);
	alpaca_response(buf, buflen, v, client_txn_id, 0, "");
}

void alpaca_response_double(char *buf, size_t buflen, double value,
                             long client_txn_id) {
	char v[48];
	snprintf(v, sizeof(v), "%.10g", value);
	alpaca_response(buf, buflen, v, client_txn_id, 0, "");
}

/* Escapes '"' and '\\' for embedding in a JSON string literal -- sufficient
 * for the fixed, ASCII device-info strings this server emits. */
static void json_escape(char *dst, size_t dstlen, const char *src) {
	size_t i = 0;
	for (; *src && i + 2 < dstlen; src++) {
		if (*src == '"' || *src == '\\') {
			if (i + 3 >= dstlen) break;
			dst[i++] = '\\';
		}
		dst[i++] = *src;
	}
	dst[i] = '\0';
}

void alpaca_response_string(char *buf, size_t buflen, const char *value,
                             long client_txn_id) {
	char escaped[512];
	char v[560];
	json_escape(escaped, sizeof(escaped), value ? value : "");
	snprintf(v, sizeof(v), "\"%s\"", escaped);
	alpaca_response(buf, buflen, v, client_txn_id, 0, "");
}

void alpaca_response_error(char *buf, size_t buflen, long client_txn_id,
                            int error_number, const char *error_message) {
	alpaca_response(buf, buflen, NULL, client_txn_id, error_number,
	                 error_message);
}
