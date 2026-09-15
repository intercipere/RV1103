#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

#define PARAMS_MAX 32
#define PARAMS_KEY_LEN 64
#define PARAMS_VAL_LEN 256

typedef struct {
	char key[PARAMS_KEY_LEN];
	char value[PARAMS_VAL_LEN];
} kv_t;

typedef struct {
	kv_t params[PARAMS_MAX];
	int count;
} params_t;

/*
 * Parses a query string or application/x-www-form-urlencoded body ("a=b&c=d")
 * into key/value pairs, URL-decoding both sides. Keys are lowercased on
 * the way in, since Alpaca parameter names are case-insensitive
 * (ClientID/clientid/CLIENTID are all valid per spec) -- look them up with
 * a lowercase key.
 */
void params_parse(const char *qs, params_t *out);

const char *params_get(const params_t *p, const char *key);
long params_get_int(const params_t *p, const char *key, long def);
double params_get_double(const params_t *p, const char *key, double def);
int params_get_bool(const params_t *p, const char *key, int def);

/*
 * Alpaca JSON response envelope: {Value, ClientTransactionID,
 * ServerTransactionID, ErrorNumber, ErrorMessage}. ServerTransactionID is a
 * server-wide monotonic counter, incremented on every call automatically.
 * `value_json` is a pre-formed bare JSON value (number/true/false/"string"/
 * [array]), or NULL to omit the Value field entirely (PUT acknowledgements
 * with no return value).
 */
void alpaca_response(char *buf, size_t buflen, const char *value_json,
                      long client_txn_id, int error_number,
                      const char *error_message);

void alpaca_response_bool(char *buf, size_t buflen, int value,
                           long client_txn_id);
void alpaca_response_int(char *buf, size_t buflen, long value,
                          long client_txn_id);
void alpaca_response_double(char *buf, size_t buflen, double value,
                             long client_txn_id);
/* `value` is escaped and wrapped in quotes automatically. */
void alpaca_response_string(char *buf, size_t buflen, const char *value,
                             long client_txn_id);
void alpaca_response_error(char *buf, size_t buflen, long client_txn_id,
                            int error_number, const char *error_message);

/* Exposes the same monotonic ServerTransactionID counter alpaca_response()
 * uses internally, for callers building a response by hand (e.g. the
 * ImageBytes binary format) that still need to participate in the same
 * global sequence real Alpaca clients expect. */
long alpaca_next_server_txn(void);

#endif
