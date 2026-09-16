#ifndef SWITCH_API_H
#define SWITCH_API_H

struct mg_context;

/*
 * ASCOM ISwitchV2 device exposing the OpenAstroGuider's lens dew heater as a
 * single on/off switch (switch id 0).
 *
 * Why a separate Alpaca device rather than something on the camera: ASCOM's
 * Camera interface has no dew-heater member, and Switch is the interface
 * ASCOM provides for exactly this kind of auxiliary control. One Alpaca
 * server can host several devices, so this appears alongside the camera as
 * /api/v1/switch/0/ and both SharpCap and N.I.N.A. surface it in their
 * existing Switch UI without any custom support.
 *
 * Must be called before the HTTP server starts: restores the persisted
 * heater state and drives the GPIO to match.
 */
void switch_api_init(void);

void switch_api_register(struct mg_context *ctx);

#endif
