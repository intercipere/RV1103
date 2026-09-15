#include "discovery.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DISCOVERY_PORT 32227
#define DISCOVERY_MSG "alpacadiscovery1"

static int g_tcp_port;

static void *discovery_thread(void *arg) {
	(void)arg;
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("discovery: socket");
		return NULL;
	}

	int reuse = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(DISCOVERY_PORT);
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("discovery: bind");
		close(sock);
		return NULL;
	}

	fprintf(stderr, "discovery: listening on UDP :%d\n", DISCOVERY_PORT);

	char buf[256];
	for (;;) {
		struct sockaddr_in from;
		socklen_t fromlen = sizeof(from);
		ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
		                      (struct sockaddr *)&from, &fromlen);
		if (n <= 0)
			continue;
		buf[n] = '\0';

		/* Alpaca requires tolerating trailing bytes/newline, so match a
		 * prefix rather than the whole datagram. */
		if (strncmp(buf, DISCOVERY_MSG, strlen(DISCOVERY_MSG)) != 0)
			continue;

		char resp[64];
		int len = snprintf(resp, sizeof(resp), "{\"AlpacaPort\":%d}", g_tcp_port);
		sendto(sock, resp, (size_t)len, 0, (struct sockaddr *)&from, fromlen);
	}
	return NULL;
}

void discovery_start(int tcp_port) {
	g_tcp_port = tcp_port;
	pthread_t t;
	pthread_create(&t, NULL, discovery_thread, NULL);
	pthread_detach(t);
}
