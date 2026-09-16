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

/* Current heater state (1 = on), for the setup page to render without going
 * back through HTTP to its own API. */
int switch_api_dew_state(void);

/*
 * Whether a real GPIO is behind the switch, for the setup page to tell the
 * user which of the three situations they're in. "No pin configured" and
 * "pin configured but unavailable" have to stay distinguishable -- conflating
 * them sends whoever wires up the PCB looking in the wrong place. `pin`, if
 * non-NULL, receives the configured sysfs GPIO number (-1 when unset).
 */
typedef enum {
	DEW_GPIO_UNSET = 0,   /* OAG_DEWHEATER_GPIO not set: logical switch only */
	DEW_GPIO_READY = 1,   /* exported and driving a real pin */
	DEW_GPIO_FAILED = 2,  /* a pin was asked for and could not be claimed */
} dew_gpio_status_t;

dew_gpio_status_t switch_api_gpio_status(int *pin);

#endif
