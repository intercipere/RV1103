#include "setup_api.h"

#include "civetweb.h"
#include "switch_api.h"

#include <stdio.h>
#include <string.h>

/*
 * The page is a single self-contained document: no external stylesheet,
 * script or font. It has to render on a machine whose only link to the
 * camera is the USB gadget's link-local network, with no route to the
 * internet -- anything fetched from a CDN would simply hang.
 *
 * It is split in two halves so a small server-rendered <script> block can be
 * injected between them (see h_setup). Everything the page shows about the
 * camera is fetched from the existing Alpaca API by the script below rather
 * than baked in here, so there is exactly one implementation of "what is the
 * exposure range" and this page cannot drift away from what clients are told.
 * The heater is the exception: its state and GPIO status come from
 * switch_api.c directly, since the page is rendered on the same machine.
 *
 * Attributes are single-quoted throughout -- legal HTML, and it keeps the C
 * string literal free of backslash escapes.
 */
static const char PAGE_HEAD[] =
    "<!doctype html>\n"
    "<html lang='en'>\n"
    "<head>\n"
    "<meta charset='utf-8'>\n"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
    "<title>OpenAstroGuider</title>\n"
    "<style>\n"
    /* Dark by default, with a warm/red accent rather than the usual blue:
     * this page is opened at the telescope, in the dark, where a bright
     * blue-white UI costs the user their dark adaptation for minutes. */
    ":root{--bg:#131110;--card:#1e1b19;--line:#37312d;--fg:#ece7e3;"
    "--dim:#9c938d;--accent:#e0523a}\n"
    "*{box-sizing:border-box}\n"
    "body{margin:0;background:var(--bg);color:var(--fg);"
    "font:15px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif}\n"
    "main{max-width:560px;margin:0 auto;padding:28px 16px 40px}\n"
    "h1{font-size:20px;margin:0;letter-spacing:.01em}\n"
    ".sub{color:var(--dim);font-size:13px;margin:4px 0 24px}\n"
    ".card{background:var(--card);border:1px solid var(--line);"
    "border-radius:10px;padding:18px;margin-bottom:16px}\n"
    "h2{font-size:12px;letter-spacing:.09em;text-transform:uppercase;"
    "color:var(--dim);font-weight:600;margin:0 0 16px}\n"
    ".row{display:flex;align-items:center;gap:14px}\n"
    ".sw{flex:none;width:54px;height:30px;padding:0;border-radius:15px;"
    "border:1px solid var(--line);background:#2b2523;position:relative;"
    "cursor:pointer;transition:background .15s,border-color .15s}\n"
    ".sw[aria-checked='true']{background:var(--accent);border-color:var(--accent)}\n"
    ".sw:disabled{opacity:.45;cursor:default}\n"
    ".sw:focus-visible{outline:2px solid var(--accent);outline-offset:3px}\n"
    ".knob{position:absolute;top:3px;left:3px;width:22px;height:22px;"
    "border-radius:50%;background:#efe9e6;transition:transform .15s}\n"
    ".sw[aria-checked='true'] .knob{transform:translateX(24px)}\n"
    ".state{font-weight:600}\n"
    ".hint{color:var(--dim);font-size:13px}\n"
    ".hint.warn{color:var(--accent)}\n"
    ".note{margin:16px 0 0;padding-top:14px;border-top:1px solid var(--line);"
    "color:var(--dim);font-size:12px}\n"
    "dl{display:grid;grid-template-columns:auto 1fr;gap:7px 16px;margin:0;"
    "font-size:13px}\n"
    "dt{color:var(--dim)}\n"
    "dd{margin:0;text-align:right;font-variant-numeric:tabular-nums}\n"
    ".foot{color:var(--dim);font-size:12px;text-align:center;margin:24px 0 0}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<main>\n"
    "<h1>OpenAstroGuider</h1>\n"
    "<p class='sub'>Device setup &middot; <span id='ver'>&hellip;</span></p>\n"

    "<section class='card'>\n"
    "<h2>Dew heater</h2>\n"
    "<div class='row'>\n"
    "<button id='sw' class='sw' type='button' role='switch' aria-checked='false'"
    " aria-label='Dew heater'><span class='knob'></span></button>\n"
    "<div><div id='st' class='state'>&hellip;</div>\n"
    "<div id='hint' class='hint'>Lens dew heater strip</div></div>\n"
    "</div>\n"
    "<p class='note' id='pin'></p>\n"
    "</section>\n"

    "<section class='card'>\n"
    "<h2>Camera</h2>\n"
    "<dl>\n"
    "<dt>Sensor</dt><dd id='i_sensor'>&hellip;</dd>\n"
    "<dt>Resolution</dt><dd id='i_res'>&hellip;</dd>\n"
    "<dt>Pixel size</dt><dd id='i_px'>&hellip;</dd>\n"
    "<dt>Exposure</dt><dd id='i_exp'>&hellip;</dd>\n"
    "<dt>Full well</dt><dd id='i_adu'>&hellip;</dd>\n"
    "</dl>\n"
    "</section>\n"

    "<p class='foot'>Changes apply immediately and survive a power cycle.<br>\n"
    "Served by the camera itself &mdash; no internet connection is used.</p>\n"
    "</main>\n";

static const char PAGE_TAIL[] =
    "<script>\n"
    /* ES5 only, no arrow functions or template literals: this page is opened
     * by whatever the host OS considers the default browser, which is not
     * necessarily a recent one. */
    "function el(i){return document.getElementById(i)}\n"
    "var sw=el('sw'),st=el('st'),hint=el('hint');\n"

    "function get(m){return fetch('/api/v1/'+m).then(function(r){"
    "return r.json()})}\n"
    "function show(m,id,fmt){get(m).then(function(j){"
    "if(j.ErrorNumber)return;"
    "el(id).textContent=fmt?fmt(j.Value):j.Value}).catch(function(){})}\n"
    /* The exposure minimum is one sensor row (~27us here), so a fixed
     * millisecond format would render the low end of the range as a
     * meaningless "0.0 ms". */
    "function secs(v){\n"
    " if(v<1e-3)return(v*1e6).toFixed(0)+' \\u00b5s';\n"
    " if(v<1)return(v*1e3).toFixed(v<0.01?2:1)+' ms';\n"
    " return v.toFixed(3)+' s';\n"
    "}\n"

    "function paint(on){sw.setAttribute('aria-checked',on?'true':'false');"
    "st.textContent=on?'On':'Off'}\n"

    /* The toggle drives the very same Alpaca endpoint a Switch-aware client
     * would use, so a client with the heater already connected sees the
     * change on its next poll and the two can never disagree. */
    "sw.onclick=function(){\n"
    " var next=sw.getAttribute('aria-checked')!=='true';\n"
    " sw.disabled=true;\n"
    " fetch('/api/v1/switch/0/setswitch',{method:'PUT',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'Id=0&State='+next})\n"
    "  .then(function(r){return r.json()})\n"
    "  .then(function(j){if(j.ErrorNumber){hint.className='hint warn';"
    "hint.textContent=j.ErrorMessage||'The camera rejected the change.'}"
    "else{paint(next);note()}})\n"
    "  .catch(function(){hint.className='hint warn';"
    "hint.textContent='Could not reach the camera.'})\n"
    "  .then(function(){sw.disabled=false});\n"
    "};\n"

    /* Says which of the three GPIO situations we are in, in the user's terms
     * rather than the log's. "No pin bound" is the expected state on the
     * prototype board and must not read like a fault; a pin that was asked
     * for and could not be claimed must not read like everything is fine. */
    "function note(){\n"
    " var p=el('pin');\n"
    " if(OAG.pinState===1){p.textContent='Driving GPIO '+OAG.pin+'.';"
    "hint.className='hint';hint.textContent='Lens dew heater strip';return}\n"
    " if(OAG.pinState===2){p.textContent='GPIO '+OAG.pin+' could not be "
    "claimed \\u2014 check that no other driver is using it.';"
    "hint.className='hint warn';"
    "hint.textContent='Not reaching the hardware';return}\n"
    " p.textContent='No heater pin is bound on this board, so the setting is "
    "remembered but drives nothing yet. Set OAG_DEWHEATER_GPIO in "
    "/etc/init.d/S60alpacad to the heater\\u2019s sysfs GPIO number.';\n"
    " hint.className='hint';hint.textContent='Not wired up yet';\n"
    "}\n"

    "paint(OAG.heater);note();\n"
    "show('camera/0/driverversion','ver');\n"
    "show('camera/0/sensorname','i_sensor');\n"
    "show('camera/0/maxadu','i_adu',function(v){return v+' ADU'});\n"
    "Promise.all([get('camera/0/cameraxsize'),get('camera/0/cameraysize')])\n"
    " .then(function(r){el('i_res').textContent=r[0].Value+' \\u00d7 '"
    "+r[1].Value}).catch(function(){});\n"
    "Promise.all([get('camera/0/pixelsizex'),get('camera/0/pixelsizey')])\n"
    " .then(function(r){el('i_px').textContent=r[0].Value+' \\u00d7 '"
    "+r[1].Value+' \\u00b5m'}).catch(function(){});\n"
    "Promise.all([get('camera/0/exposuremin'),get('camera/0/exposuremax')])\n"
    " .then(function(r){el('i_exp').textContent=secs(r[0].Value)+' \\u2013 '"
    "+secs(r[1].Value)}).catch(function(){});\n"

    /* Re-read the heater rather than trusting the value rendered into the
     * page: a Switch-aware client (or a second browser tab) may have changed
     * it since, and a stale toggle is worse than a slow one. */
    "get('switch/0/getswitch?Id=0').then(function(j){"
    "if(!j.ErrorNumber)paint(j.Value===true)}).catch(function(){});\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static int h_setup(struct mg_connection *conn, void *cbdata) {
	(void)cbdata;

	int pin = -1;
	dew_gpio_status_t gpio = switch_api_gpio_status(&pin);

	/* Rendered into the page rather than fetched, so the toggle is already in
	 * the right position on first paint instead of visibly flipping once the
	 * script's request comes back. */
	char state[192];
	int n = snprintf(state, sizeof(state),
	                  "<script>var OAG={heater:%s,pinState:%d,pin:%d};</script>\n",
	                  switch_api_dew_state() ? "true" : "false", (int)gpio, pin);
	if (n < 0 || (size_t)n >= sizeof(state))
		n = 0;

	size_t total = (sizeof(PAGE_HEAD) - 1) + (size_t)n + (sizeof(PAGE_TAIL) - 1);
	mg_printf(conn,
	           "HTTP/1.1 200 OK\r\n"
	           "Content-Type: text/html; charset=utf-8\r\n"
	           "Content-Length: %lu\r\n"
	           /* The heater state is baked into the body, so a cached copy
	            * would show a stale toggle after a power cycle. */
	           "Cache-Control: no-store\r\n"
	           "\r\n",
	           (unsigned long)total);

	mg_write(conn, PAGE_HEAD, sizeof(PAGE_HEAD) - 1);
	mg_write(conn, state, (size_t)n);
	mg_write(conn, PAGE_TAIL, sizeof(PAGE_TAIL) - 1);
	return 200;
}

void setup_api_register(struct mg_context *ctx) {
	/* civetweb tries an exact match first, then accepts "<handler>/anything",
	 * then falls back to pattern matching. So "/setup" alone covers both the
	 * server page and every per-device /setup/v1/<type>/<n>/setup URL. */
	mg_set_request_handler(ctx, "/setup", h_setup, NULL);

	/* "/$" and not "/": a bare "/" is also a valid *pattern*, and as a pattern
	 * it prefix-matches every URL on the server -- an unrecognised /api/v1/...
	 * path would then quietly return this HTML page instead of a JSON error,
	 * which is a miserable thing to debug. The "$" anchors it to the root. */
	mg_set_request_handler(ctx, "/$", h_setup, NULL);
}
