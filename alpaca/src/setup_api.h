#ifndef SETUP_API_H
#define SETUP_API_H

struct mg_context;

/*
 * Serves the browser-facing setup page, which is the only place a user can
 * reach the dew heater from a client that has no Switch UI of its own.
 *
 * Background: ASCOM clients expose a per-device "Settings"/"Properties"
 * button that calls the driver's SetupDialog(). For an Alpaca device the
 * ASCOM Platform's Alpaca-to-COM bridge implements that by opening the
 * system browser at the device's setup URL -- it cannot draw a native dialog
 * for a driver that lives on another machine. PHD2's camera Settings button
 * is exactly this path. Until now nothing answered those URLs, so the button
 * opened a 404 and the heater was unreachable from any client that doesn't
 * implement ASCOM Switch (PHD2 has no Switch support at all).
 *
 * Alpaca defines two setup URLs -- /setup for the server and
 * /setup/v1/<devicetype>/<devicenumber>/setup per device -- and clients
 * differ in which one they open (PHD2 lands on the camera's). All of them,
 * plus the bare root a user is most likely to type, serve the same page:
 * whichever door the client opens, the toggle has to be behind it.
 */
void setup_api_register(struct mg_context *ctx);

#endif
