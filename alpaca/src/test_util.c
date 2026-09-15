#include <stdio.h>
#include <string.h>
#include "util.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
	else printf("ok: %s\n", msg); \
} while (0)

int main(void) {
	params_t p;
	params_parse("ClientID=231&ClientTransactionID=23&Duration=2.5&Light=true", &p);
	CHECK(strcmp(params_get(&p, "clientid"), "231") == 0, "clientid parsed + lowercased key");
	CHECK(params_get_int(&p, "clienttransactionid", -1) == 23, "int param");
	CHECK(params_get_double(&p, "duration", -1) == 2.5, "double param");
	CHECK(params_get_bool(&p, "light", 0) == 1, "bool param true");
	CHECK(params_get(&p, "missing") == NULL, "missing key returns NULL");

	params_t p2;
	params_parse("name=hello%20world&x=a%2Bb", &p2);
	CHECK(strcmp(params_get(&p2, "name"), "hello world") == 0, "url-decode %20");
	CHECK(strcmp(params_get(&p2, "x"), "a+b") == 0, "url-decode %2B literal plus");

	char buf[256];
	alpaca_response_bool(buf, sizeof(buf), 1, 42);
	printf("bool response: %s\n", buf);
	CHECK(strstr(buf, "\"Value\":true") != NULL, "bool response has Value:true");
	CHECK(strstr(buf, "\"ClientTransactionID\":42") != NULL, "echoes ClientTransactionID");
	CHECK(strstr(buf, "\"ErrorNumber\":0") != NULL, "ErrorNumber 0 on success");

	alpaca_response_string(buf, sizeof(buf), "SC3336", 1);
	printf("string response: %s\n", buf);
	CHECK(strstr(buf, "\"Value\":\"SC3336\"") != NULL, "string response quoted");

	alpaca_response_error(buf, sizeof(buf), 5, 0x400, "Not connected");
	printf("error response: %s\n", buf);
	CHECK(strstr(buf, "\"ErrorNumber\":1024") != NULL, "error number present");
	CHECK(strstr(buf, "Not connected") != NULL, "error message present");
	CHECK(strstr(buf, "\"Value\"") == NULL, "no Value field when value_json is NULL");

	printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
	return failures;
}
