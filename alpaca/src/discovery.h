#ifndef DISCOVERY_H
#define DISCOVERY_H

/* Starts the Alpaca discovery UDP responder in a detached background
 * thread: listens on port 32227 for the literal broadcast message
 * "alpacadiscovery1" and replies via unicast with {"AlpacaPort": tcp_port}.
 * See alpaca/README.md, "Discovery (UDP, port 32227)". */
void discovery_start(int tcp_port);

#endif
