#include "guide_api.h"

#include "civetweb.h"
#include "guide_loop.h"
#include "http_util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The page is one self-contained document: no external stylesheet, script or
 * font, for the same reason setup_api.c's page is. The only link to this device
 * is a link-local USB network (or, on the custom board, its own WiFi AP) with
 * no route to the internet -- anything fetched from a CDN would simply hang.
 *
 * Single theme on purpose. This is opened at the telescope in the dark, where a
 * light UI costs the user their dark adaptation for minutes, so the palette is
 * the same warm/red set as the setup page and the two error traces are
 * separated by hue AND brightness rather than the usual blue-versus-red.
 *
 * Attributes are single-quoted throughout -- legal HTML, and it keeps the C
 * string literal free of backslash escapes.
 */
static const char PAGE[] =
"<!doctype html>\n<html lang='en'><head><meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>OpenAstroGuider</title>\n<style>\n"
":root{--bg:#131110;--card:#1e1b19;--card2:#191615;--line:#37312d;--fg:#ece7e3;"
"--dim:#9c938d;--faint:#6d645f;--accent:#e0523a;--ra:#e8623f;--dec:#c08a3c;"
"--ok:#6f9e6a;--crit:#d1493f;"
"--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}\n"
"*{box-sizing:border-box}html,body{background:var(--bg)}\n"
"body{margin:0;color:var(--fg);font:15px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif}\n"
"main{max-width:560px;margin:0 auto;padding:22px 16px 40px}\n"
"header{display:flex;align-items:baseline;justify-content:space-between;gap:12px;flex-wrap:wrap}\n"
"h1{font-size:19px;margin:0;font-weight:600}\n"
".addr{font:12px/1.4 var(--mono);color:var(--faint)}\n"
".card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px;margin-bottom:14px}\n"
"h2{font-size:11px;letter-spacing:.09em;text-transform:uppercase;color:var(--dim);"
"font-weight:600;margin:0 0 14px;display:flex;justify-content:space-between;gap:10px}\n"
"h2 .meta{text-transform:none;letter-spacing:0;font-weight:400;font-family:var(--mono);font-size:11px;color:var(--faint)}\n"
".state{display:flex;align-items:center;gap:10px;margin-bottom:14px;flex-wrap:wrap}\n"
".pill{display:inline-flex;align-items:center;gap:7px;padding:5px 11px;border-radius:999px;"
"font-size:13px;font-weight:600;border:1px solid transparent;background:#2b2523;color:var(--dim)}\n"
".pill.go{background:rgba(111,158,106,.14);border-color:rgba(111,158,106,.4);color:var(--ok)}\n"
".pill.bad{background:rgba(209,73,63,.14);border-color:rgba(209,73,63,.45);color:var(--crit)}\n"
".dot{width:7px;height:7px;border-radius:50%;background:currentColor;flex:none}\n"
".since{font:12px/1 var(--mono);color:var(--faint)}\n"
".tiles{display:grid;grid-template-columns:repeat(3,1fr);gap:1px;background:var(--line);"
"border:1px solid var(--line);border-radius:8px;overflow:hidden}\n"
".tile{background:var(--card2);padding:11px 12px}\n"
".tile .k{font-size:10px;letter-spacing:.07em;text-transform:uppercase;color:var(--faint)}\n"
".tile .v{font:600 19px/1.25 var(--mono);font-variant-numeric:tabular-nums;margin-top:3px}\n"
".tile .u{font-size:11px;color:var(--dim);font-weight:400}\n"
".frame{position:relative;border:1px solid var(--line);border-radius:8px;overflow:hidden;background:#000;line-height:0}\n"
".empty{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;\n"
"justify-content:center;gap:6px;line-height:1.5;color:var(--dim);font-size:14px;\n"
"background:#0d0c0b;text-align:center;padding:0 16px}\n"
".empty span{color:var(--faint);font-size:12px}\n"
".empty[hidden]{display:none}\n"
"canvas{display:block;width:100%;height:auto;max-width:100%}\n"
".legend{display:flex;gap:16px;margin-top:10px;font-size:12px;color:var(--dim);flex-wrap:wrap}\n"
".legend span{display:inline-flex;align-items:center;gap:6px}\n"
".legend i{width:14px;height:2px;border-radius:1px;display:block}\n"
".legend b{font-family:var(--mono);font-weight:600;color:var(--fg)}\n"
".btns{display:flex;gap:8px;flex-wrap:wrap}\n"
"button{font:inherit;color:var(--fg);background:#2b2523;border:1px solid var(--line);"
"border-radius:7px;padding:9px 14px;cursor:pointer}\n"
"button:hover{background:#352d2a}button:focus-visible{outline:2px solid var(--accent);outline-offset:2px}\n"
"button.primary{background:var(--accent);border-color:var(--accent);color:#1a0d09;font-weight:600}\n"
"button.grow{flex:1 1 auto}\n"
".seg{display:flex;border:1px solid var(--line);border-radius:7px;overflow:hidden}\n"
".seg button{border:0;border-radius:0;flex:1 1 0;background:#221d1b}\n"
".seg button[aria-pressed='true']{background:var(--accent);color:#1a0d09;font-weight:600}\n"
".field{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:8px 0}\n"
".field label{color:var(--dim);font-size:14px}\n"
"select{font:inherit;color:var(--fg);background:#2b2523;border:1px solid var(--line);"
"border-radius:6px;padding:6px 9px}\n"
".kv{display:flex;justify-content:space-between;gap:14px;padding:7px 0;border-bottom:1px solid var(--line);font-size:14px}\n"
".kv:last-child{border-bottom:0;padding-bottom:0}\n"
".kv dt{color:var(--dim);margin:0}\n"
".kv dd{margin:0;font-family:var(--mono);font-variant-numeric:tabular-nums;text-align:right}\n"
"dl{margin:0}\n"
".warn{color:var(--crit);font-size:13px;margin-top:10px;min-height:1.2em}\n"
".gnote{color:var(--faint);font-size:12px;margin:8px 0 0}\n"
".gnote[hidden]{display:none}\n"
"footer{color:var(--faint);font-size:12px;text-align:center;margin-top:22px;line-height:1.8}\n"
"@media(max-width:420px){.tiles{grid-template-columns:repeat(2,1fr)}.tile:last-child{grid-column:1/-1}}\n"
"</style></head><body><main>\n"
"<header><h1>Guiding</h1><span class='addr' id='addr'></span></header>\n"

"<section class='card'>\n"
"<div class='state'><span class='pill' id='pill'><i class='dot'></i><span id='stateText'>connecting</span></span>\n"
"<span class='since' id='meta'></span></div>\n"
"<div class='tiles'>\n"
"<div class='tile'><div class='k'>RMS total</div><div class='v' id='rmsT'>--<span class='u'>px</span></div></div>\n"
"<div class='tile'><div class='k'>RMS RA</div><div class='v' id='rmsR'>--<span class='u'>px</span></div></div>\n"
"<div class='tile'><div class='k'>RMS Dec</div><div class='v' id='rmsD'>--<span class='u'>px</span></div></div>\n"
"</div><div class='warn' id='warn'></div></section>\n"

"<section class='card'><h2>Frame source</h2>\n"
"<div class='seg' id='seg'>\n"
"<button id='srcCam' aria-pressed='false'>Camera</button>\n"
"<button id='srcSim' aria-pressed='false'>Simulated</button></div>\n"
"<p style='color:var(--dim);font-size:13px;margin:12px 0 0'>Simulated draws stars at known "
"positions over a synthetic frame &mdash; it needs no sky, no mount and no light, and it is "
"the only mode with a known correct answer to check against.</p></section>\n"

"<section class='card'><h2>Guide star <span class='meta' id='starMeta'></span></h2>\n"
"<div class='frame'><canvas id='prev' width='576' height='324'></canvas>\n"
"<div class='empty' id='empty'>No image yet<span>Press <b>Start guiding</b> to begin acquiring frames</span></div></div>\n"
"<div class='btns' style='margin-top:12px'>\n"
"<button class='grow' id='btnSel'>Next star</button></div></section>\n"

"<section class='card'><h2>Guide error <span class='meta' id='gmeta'>last 100 frames</span></h2>\n"
"<canvas id='graph' width='1024' height='400'></canvas>\n"
"<p class='gnote' id='gnote' hidden>Looping only &mdash; no corrections are being computed.</p>\n"
"<div class='legend'><span><i style='background:var(--ra)'></i>RA <b id='curRa'>--</b></span>\n"
"<span><i style='background:var(--dec)'></i>Dec <b id='curDec'>--</b></span>\n"
"<span><i style='background:var(--line)'></i>min-move <b id='mm'>--</b></span></div></section>\n"

"<section class='card'><h2>Control</h2>\n"
"<div class='btns'><button class='grow' id='btnLoop'>Loop exposures</button>\n"
"<button class='primary grow' id='btnRun'>Start guiding</button></div>\n"
"<p class='hint' style='margin:12px 0 0;color:var(--dim);font-size:13px'>Loop acquires "
"frames for framing and focus without correcting. Guiding selects a star if none is "
"chosen, then tracks it.</p>\n"
"<div class='field'><label for='exp'>Exposure</label><select id='exp'>\n"
"<option value='0.1'>0.1 s</option><option value='0.25'>0.25 s</option>\n"
"<option value='0.5' selected>0.5 s</option><option value='1'>1 s</option>\n"
"<option value='2'>2 s</option></select></div>\n"
"<div class='field'><label for='mmSel'>Minimum move</label><select id='mmSel'>\n"
"<option value='0'>0 px</option><option value='0.1'>0.10 px</option>\n"
"<option value='0.15' selected>0.15 px</option><option value='0.3'>0.30 px</option>\n"
"</select></div>\n"
"<div class='field'><label for='algoRa'>RA algorithm</label><select id='algoRa'>\n"
"<option value='0'>Hysteresis</option><option value='1'>Lowpass</option>\n"
"<option value='2'>Resist switch</option><option value='3'>Identity</option></select></div>\n"
"<div class='field'><label for='algoDec'>Dec algorithm</label><select id='algoDec'>\n"
"<option value='0'>Hysteresis</option><option value='1'>Lowpass</option>\n"
"<option value='2' selected>Resist switch</option><option value='3'>Identity</option></select></div>\n"
"</section>\n"

"<section class='card'><h2>Detail</h2><dl>\n"
"<div class='kv'><dt>Star position</dt><dd id='dPos'>--</dd></div>\n"
"<div class='kv'><dt>SNR / HFD</dt><dd id='dSnr'>--</dd></div>\n"
"<div class='kv'><dt>Frame acquire</dt><dd id='dFrame'>--</dd></div>\n"
"<div class='kv'><dt>Centroid</dt><dd id='dFind'>--</dd></div>\n"
"<div class='kv'><dt>Frames / lost</dt><dd id='dCnt'>--</dd></div>\n"
"</dl></section>\n"

"<footer>Corrections are computed and shown, but not yet sent to a mount.<br>"
"ST-4 output and calibration are not implemented.</footer>\n"
"</main>\n<script>\n"
"(function(){'use strict';\n"
"var $=function(i){return document.getElementById(i);};\n"
"$('addr').textContent=location.host;\n"
"var running=false,guiding=false,looping=false,hist={ra:[],dec:[]};\n"
"function put(body){return fetch('/guide/control',{method:'PUT',\n"
" headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});}\n"
"$('btnRun').onclick=function(){put(guiding?'action=stop':'action=start');};\n"
"$('btnLoop').onclick=function(){put(looping?'action=stop':'action=loop');};\n"
"$('btnSel').onclick=function(){put('action=reselect');};\n"
"$('srcCam').onclick=function(){put('action=source&value=0');};\n"
"$('srcSim').onclick=function(){put('action=source&value=1');};\n"
"$('exp').onchange=function(){put('action=exposure&value='+this.value);};\n"
"$('mmSel').onchange=function(){put('action=minmove&value='+this.value);};\n"
"$('algoRa').onchange=function(){put('action=algo&axis=ra&value='+this.value);};\n"
"$('algoDec').onchange=function(){put('action=algo&axis=dec&value='+this.value);};\n"

"var g=$('graph'),gx=g.getContext('2d'),RANGE=4;\n"
"function css(n){return getComputedStyle(document.documentElement).getPropertyValue(n).trim();}\n"
"function drawGraph(mm){\n"
" var W=g.width,H=g.height,pl=52,pr=10,pt=14,pb=44,iw=W-pl-pr,ih=H-pt-pb,i,v,y;\n"
" gx.clearRect(0,0,W,H);gx.font='22px ui-monospace,Menlo,monospace';gx.textBaseline='middle';\n"
" var ticks=[4,2,0,-2,-4];\n"
" for(i=0;i<ticks.length;i++){v=ticks[i];y=pt+ih/2-(v/RANGE)*(ih/2);\n"
"  gx.strokeStyle=v===0?css('--line'):css('--card2');gx.lineWidth=v===0?2:1;\n"
"  gx.beginPath();gx.moveTo(pl,y);gx.lineTo(W-pr,y);gx.stroke();\n"
"  gx.fillStyle=css('--faint');gx.textAlign='right';\n"
"  gx.fillText((v>0?'+':'')+v+'px',pl-10,y);}\n"
" if(mm>0){var y0=pt+ih/2-(mm/RANGE)*(ih/2),y1=pt+ih/2+(mm/RANGE)*(ih/2);\n"
"  gx.fillStyle='rgba(156,147,141,.10)';gx.fillRect(pl,y0,iw,y1-y0);}\n"
" function tr(a,c){if(!a.length)return;gx.strokeStyle=c;gx.lineWidth=2.5;\n"
"  gx.lineJoin='round';gx.lineCap='round';gx.beginPath();\n"
"  for(var j=0;j<a.length;j++){var x=pl+(a.length<2?iw:(j/(a.length-1))*iw);\n"
"   var yy=pt+ih/2-(Math.max(-RANGE,Math.min(RANGE,a[j]))/RANGE)*(ih/2);\n"
"   j?gx.lineTo(x,yy):gx.moveTo(x,yy);}gx.stroke();\n"
"  var lx=pl+iw,ly=pt+ih/2-(Math.max(-RANGE,Math.min(RANGE,a[a.length-1]))/RANGE)*(ih/2);\n"
"  gx.fillStyle=c;gx.beginPath();gx.arc(lx,ly,4.5,0,6.284);gx.fill();}\n"
" tr(hist.dec,css('--dec'));tr(hist.ra,css('--ra'));\n"
" gx.fillStyle=css('--faint');gx.textAlign='left';gx.fillText('oldest',pl,H-13);\n"
" gx.textAlign='right';gx.fillText('now',W-pr,H-13);}\n"

"var pc=$('prev'),px=pc.getContext('2d'),pimg=null;\n"
"function drawPreview(buf,w,h){\n"
" if(pc.width!==w||pc.height!==h||!pimg){pc.width=w;pc.height=h;pimg=px.createImageData(w,h);}\n"
" var d=pimg.data,n=w*h,i,v;\n"
" for(i=0;i<n;i++){v=buf[i];d[i*4]=v;d[i*4+1]=v;d[i*4+2]=v;d[i*4+3]=255;}\n"
" px.putImageData(pimg,0,0);}\n"

"function fmt(v){return (v<0?'\\u2212':'+')+Math.abs(v).toFixed(2)+' px';}\n"
"function poll(){\n"
" fetch('/guide/state').then(function(r){return r.json();}).then(function(s){\n"
"  guiding=(s.state==='guiding'||s.state==='lost');\n"
"  looping=(s.state==='looping'||s.state==='selected');\n"
"  running=guiding||looping||s.state==='selecting';\n"
"  var p=$('pill');p.className='pill'+(s.state==='guiding'?' go':\n"
"    ((s.state==='stopped'||s.state==='looping'||s.state==='selected')?'':' bad'));\n"
"  $('stateText').textContent=s.state;\n"
"  $('btnRun').textContent=guiding?'Stop guiding':'Start guiding';\n"
"  $('btnLoop').textContent=looping?'Stop looping':'Loop exposures';\n"
"  $('meta').textContent='frame '+s.frame+' \\u00b7 '+s.source+' \\u00b7 '+s.exposure.toFixed(2)+'s';\n"
"  $('warn').textContent=s.error||'';\n"
"  $('srcCam').setAttribute('aria-pressed',String(s.source==='camera'));\n"
"  $('srcSim').setAttribute('aria-pressed',String(s.source==='simulated'));\n"
"  $('rmsT').innerHTML=s.rms_total.toFixed(2)+\"<span class='u'>px</span>\";\n"
"  $('rmsR').innerHTML=s.rms_ra.toFixed(2)+\"<span class='u'>px</span>\";\n"
"  $('rmsD').innerHTML=s.rms_dec.toFixed(2)+\"<span class='u'>px</span>\";\n"
"  hist.ra=s.ra;hist.dec=s.dec;\n"
"  $('curRa').textContent=s.ra.length?fmt(s.ra[s.ra.length-1]):'--';\n"
"  $('curDec').textContent=s.dec.length?fmt(s.dec[s.dec.length-1]):'--';\n"
"  $('mm').textContent=s.min_move.toFixed(2)+' px';\n"
"  $('gmeta').textContent=s.ra.length+' frames \\u00b7 \\u00b14 px';\n"
"  drawGraph(s.min_move);\n"
"  $('gnote').hidden=(s.state!=='looping'&&s.state!=='selected');\n"
"  $('starMeta').textContent=s.star_found?('star '+(s.star_index+1)+' of '+s.star_count+\n"
"    ' \\u00b7 SNR '+s.snr.toFixed(1)+' \\u00b7 HFD '+s.hfd.toFixed(2)+' px'):'no star';\n"
"  $('dPos').textContent=s.star_found?(s.star_x.toFixed(2)+', '+s.star_y.toFixed(2)):'--';\n"
"  $('dSnr').textContent=s.star_found?(s.snr.toFixed(1)+' / '+s.hfd.toFixed(2)+' px'):'--';\n"
"  $('dFrame').textContent=s.frame_ms.toFixed(0)+' ms';\n"
"  $('dFind').textContent=s.find_ms.toFixed(3)+' ms';\n"
"  $('dCnt').textContent=s.frame+' / '+s.lost;\n"
" }).catch(function(){$('stateText').textContent='no connection';$('pill').className='pill bad';});\n"
" fetch('/guide/preview').then(function(r){\n"
"  if(!r.ok)return null;\n"
"  var w=+r.headers.get('X-Preview-Width'),h=+r.headers.get('X-Preview-Height');\n"
"  return r.arrayBuffer().then(function(b){return {b:new Uint8Array(b),w:w,h:h};});\n"
" }).then(function(p){\n"
" if(p&&p.w>0&&p.b.length>=p.w*p.h){drawPreview(p.b,p.w,p.h);$('empty').hidden=true;}\n"
" else {$('empty').hidden=false;}\n"
"}).catch(function(){$('empty').hidden=false;});\n"
"}\n"
"drawGraph(0.15);poll();setInterval(poll,1000);\n"
"})();\n</script></body></html>\n";

static int h_page(struct mg_connection *conn, void *cbdata) {
	(void)cbdata;
	mg_response_header_start(conn, 200);
	mg_response_header_add(conn, "Content-Type", "text/html; charset=utf-8", -1);
	mg_response_header_add(conn, "Cache-Control", "no-store", -1);
	{
		char len[32];
		snprintf(len, sizeof(len), "%zu", sizeof(PAGE) - 1);
		mg_response_header_add(conn, "Content-Length", len, -1);
	}
	mg_response_header_send(conn);
	mg_write(conn, PAGE, sizeof(PAGE) - 1);
	return 200;
}

static const char *state_name(guide_state_t s) {
	switch (s) {
	case GUIDE_STATE_LOOPING:   return "looping";
	case GUIDE_STATE_SELECTING: return "selecting";
	case GUIDE_STATE_SELECTED:  return "selected";
	case GUIDE_STATE_GUIDING:   return "guiding";
	case GUIDE_STATE_LOST:      return "lost";
	case GUIDE_STATE_ERROR:     return "error";
	default:                    return "stopped";
	}
}

/* Escapes the few characters that can appear in an error string and would
 * break the JSON. Everything else here is numeric or a fixed token. */
static void json_escape(const char *in, char *out, size_t cap) {
	size_t o = 0;
	for (; *in && o + 2 < cap; in++) {
		if (*in == '"' || *in == '\\') {
			out[o++] = '\\';
			out[o++] = *in;
		} else if ((unsigned char)*in >= 0x20) {
			out[o++] = *in;
		}
	}
	out[o] = '\0';
}

static int h_state(struct mg_connection *conn, void *cbdata) {
	static guide_status_t st; /* large; not worth a worker stack */
	static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
	char *buf;
	size_t cap = 8192, n = 0;
	int i;
	char esc[192];
	(void)cbdata;

	buf = malloc(cap);
	if (!buf)
		return 500;

	pthread_mutex_lock(&st_lock);
	guide_loop_snapshot(&st);
	json_escape(st.err, esc, sizeof(esc));

	n += (size_t)snprintf(buf + n, cap - n,
	    "{\"state\":\"%s\",\"source\":\"%s\",\"frame\":%ld,\"lost\":%ld,"
	    "\"exposure\":%.4f,\"width\":%d,\"height\":%d,"
	    "\"star_found\":%s,\"star_x\":%.3f,\"star_y\":%.3f,"
	    "\"star_index\":%d,\"star_count\":%d,"
	    "\"snr\":%.2f,\"hfd\":%.3f,\"mass\":%.0f,"
	    "\"rms_ra\":%.4f,\"rms_dec\":%.4f,\"rms_total\":%.4f,"
	    "\"min_move\":%.3f,\"frame_ms\":%.1f,\"find_ms\":%.3f,"
	    "\"algo_ra\":\"%s\",\"algo_dec\":\"%s\",\"error\":\"%s\",",
	    state_name(st.state), st.source_name, st.frame, st.lost_frames,
	    st.exposure_s, st.width, st.height,
	    st.star_found ? "true" : "false", st.star_x, st.star_y,
	    st.star_index, st.star_count,
	    st.snr, st.hfd, st.mass,
	    st.rms_ra, st.rms_dec, st.rms_total,
	    st.min_move, st.last_frame_ms, st.last_find_ms,
	    st.algo_ra, st.algo_dec, esc);

	n += (size_t)snprintf(buf + n, cap - n, "\"ra\":[");
	for (i = 0; i < st.count && n < cap - 32; i++)
		n += (size_t)snprintf(buf + n, cap - n, "%s%.3f", i ? "," : "",
		                      (double)st.history[i].ra);
	n += (size_t)snprintf(buf + n, cap - n, "],\"dec\":[");
	for (i = 0; i < st.count && n < cap - 32; i++)
		n += (size_t)snprintf(buf + n, cap - n, "%s%.3f", i ? "," : "",
		                      (double)st.history[i].dec);
	n += (size_t)snprintf(buf + n, cap - n, "]}");
	pthread_mutex_unlock(&st_lock);

	mg_response_header_start(conn, 200);
	mg_response_header_add(conn, "Content-Type", "application/json", -1);
	mg_response_header_add(conn, "Cache-Control", "no-store", -1);
	{
		char len[32];
		snprintf(len, sizeof(len), "%zu", n);
		mg_response_header_add(conn, "Content-Length", len, -1);
	}
	mg_response_header_send(conn);
	mg_write(conn, buf, n);
	free(buf);
	return 200;
}

static int h_preview(struct mg_connection *conn, void *cbdata) {
	/* Exactly one downsampled frame (576x324 = 182 kB at the current factor),
	 * allocated per request and freed immediately. Asking the loop for the size
	 * rather than guessing a megabyte matters here: this runs while a capture
	 * may be allocating a full 6 MB frame, and the slack is not available. */
	size_t cap = guide_loop_preview_size(), n;
	unsigned char *buf;
	int w = 0, h = 0;
	char v[32];
	(void)cbdata;

	if (cap == 0) {
		mg_send_http_error(conn, 503, "no preview yet");
		return 503;
	}
	buf = malloc(cap);
	if (!buf)
		return 500;
	n = guide_loop_preview(buf, cap, &w, &h);
	if (n == 0) {
		free(buf);
		mg_send_http_error(conn, 503, "no preview yet");
		return 503;
	}

	mg_response_header_start(conn, 200);
	mg_response_header_add(conn, "Content-Type", "application/octet-stream", -1);
	mg_response_header_add(conn, "Cache-Control", "no-store", -1);
	snprintf(v, sizeof(v), "%d", w);
	mg_response_header_add(conn, "X-Preview-Width", v, -1);
	snprintf(v, sizeof(v), "%d", h);
	mg_response_header_add(conn, "X-Preview-Height", v, -1);
	snprintf(v, sizeof(v), "%zu", n);
	mg_response_header_add(conn, "Content-Length", v, -1);
	mg_response_header_send(conn);
	mg_write(conn, buf, n);
	free(buf);
	return 200;
}

static int h_control(struct mg_connection *conn, void *cbdata) {
	params_t params;
	const char *action;
	(void)cbdata;

	/* Reads the PUT body and the query string both, same as every Alpaca
	 * dispatcher here. */
	parse_request_params(conn, &params);

	action = params_get(&params, "action");
	if (!action) {
		mg_send_http_error(conn, 400, "missing action");
		return 400;
	}

	if (strcasecmp(action, "loop") == 0) {
		guide_loop_loop();
	} else if (strcasecmp(action, "start") == 0) {
		guide_loop_start();
	} else if (strcasecmp(action, "stop") == 0) {
		guide_loop_stop();
	} else if (strcasecmp(action, "reselect") == 0) {
		guide_loop_reselect();
	} else if (strcasecmp(action, "source") == 0) {
		/* 0 = the real camera, 1 = simulated. The page sends the same two
		 * values the loop's enum uses for V4L2 and SIM. */
		int v = (int)params_get_double(&params, "value", 0.0);
		if (guide_loop_set_source(v == 1 ? GUIDE_SRC_SIM : GUIDE_SRC_V4L2) != 0) {
			mg_send_http_error(conn, 400, "bad source");
			return 400;
		}
	} else if (strcasecmp(action, "exposure") == 0) {
		guide_loop_set_exposure(params_get_double(&params, "value", 0.5));
	} else if (strcasecmp(action, "minmove") == 0) {
		guide_loop_set_min_move(params_get_double(&params, "value", 0.15));
	} else if (strcasecmp(action, "algo") == 0) {
		const char *axis = params_get(&params, "axis");
		int v = (int)params_get_double(&params, "value", 0.0);
		if (v < 0 || v > GUIDE_ALGO_IDENTITY) {
			mg_send_http_error(conn, 400, "bad algorithm");
			return 400;
		}
		guide_loop_set_algo(axis && strcasecmp(axis, "dec") == 0,
		                    (guide_algo_kind_t)v);
	} else {
		mg_send_http_error(conn, 400, "unknown action");
		return 400;
	}

	send_json(conn, "{\"ok\":true}");
	return 200;
}

void guide_api_register(struct mg_context *ctx) {
	/* Most specific first: civetweb tries an exact match, then
	 * "<handler>/anything", so /guide alone would otherwise swallow these. */
	mg_set_request_handler(ctx, "/guide/state", h_state, NULL);
	mg_set_request_handler(ctx, "/guide/preview", h_preview, NULL);
	mg_set_request_handler(ctx, "/guide/control", h_control, NULL);
	mg_set_request_handler(ctx, "/guide", h_page, NULL);
}
