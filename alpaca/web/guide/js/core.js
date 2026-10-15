/*
 * Shared plumbing for the guide page: device API, polling, history, events and
 * per-browser preferences. Views live in their own files and only talk to this.
 *
 * Two kinds of state, deliberately kept apart:
 *   - device settings (focal length, guiding parameters) live on the camera,
 *     under PHD2's config keys, via /guide/settings -- every browser sees them;
 *   - display preferences (layout, colour scheme, graph scale) live in this
 *     browser's localStorage, because a phone and a laptop want different ones.
 *     PHD2 keeps the latter in its "Global" config for the same reason.
 */
(function () {
  'use strict';
  var OAG = (window.OAG = {});

  OAG.$ = function (sel, root) { return (root || document).querySelector(sel); };

  OAG.el = function (tag, attrs, kids) {
    var e = document.createElement(tag), k;
    for (k in attrs || {}) {
      if (k === 'text') e.textContent = attrs[k];
      else if (k === 'class') e.className = attrs[k];
      else if (k.slice(0, 2) === 'on') e.addEventListener(k.slice(2), attrs[k]);
      else e.setAttribute(k, attrs[k]);
    }
    (kids || []).forEach(function (c) { if (c) e.appendChild(typeof c === 'string' ? document.createTextNode(c) : c); });
    return e;
  };

  /* localStorage can be missing or throw (private windows); nothing here may
   * depend on it working. */
  OAG.prefs = {
    get: function (k, d) {
      try { var v = localStorage.getItem('oag.' + k); return v == null ? d : JSON.parse(v); } catch (e) { return d; }
    },
    set: function (k, v) { try { localStorage.setItem('oag.' + k, JSON.stringify(v)); } catch (e) { /* ignore */ } }
  };

  OAG.put = function (path, body) {
    return fetch(path, { method: 'PUT', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: body });
  };
  OAG.control = function (body) { return OAG.put('/guide/control', body); };

  var subs = {};
  OAG.on = function (ev, fn) { (subs[ev] = subs[ev] || []).push(fn); };
  OAG.emit = function (ev, a) {
    (subs[ev] || []).forEach(function (f) { try { f(a); } catch (e) { console.error(e); } });
  };

  /* Colours come from CSS variables so the colour schemes reach the canvases. */
  OAG.color = function (name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); };

  /* Sizes a canvas to its box at device resolution; returns a context scaled
   * so views draw in CSS pixels. */
  OAG.fit = function (cv) {
    var dpr = window.devicePixelRatio || 1, w = Math.max(1, cv.clientWidth), h = Math.max(1, cv.clientHeight);
    if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
      cv.width = Math.round(w * dpr);
      cv.height = Math.round(h * dpr);
    }
    var ctx = cv.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { ctx: ctx, w: w, h: h };
  };

  /* ---------------------------------------------------------------- data -- */

  OAG.state = null;
  /* Every guide step this page holds, oldest first, at most HIST_MAX: the
   * device keeps 400 (PHD2's graph maximum) and sends only new ones. */
  OAG.hist = [];
  var HIST_MAX = 400;
  var epoch = null, lastSeq = -1, lastFrame = -1, busy = false;
  OAG.exposureMax = null;

  function row(r) {
    return { seq: r[0], t: r[1], dx: r[2], dy: r[3], ra: r[4], dec: r[5], raMs: r[6], raDir: r[7],
      decMs: r[8], decDir: r[9], flags: r[10], mass: r[11], snr: r[12] };
  }
  OAG.DIR = { NONE: -1, NORTH: 0, SOUTH: 1, EAST: 2, WEST: 3 };
  OAG.CALIBRATED = 1;

  function fetchFrame() {
    var pv = fetch('/guide/preview').then(function (r) {
      if (!r.ok) return null;
      var H = function (n) { return r.headers.get(n) || ''; };
      var st = H('X-Star').split(','), lk = H('X-Lock').split(',');
      var m = { frame: +H('X-Frame'), ds: +H('X-Preview-Scale') || 4, sr: +H('X-Search'),
        sx: +st[0], sy: +st[1], sel: st[2] === '1', found: st[3] === '1',
        lock: lk.length === 2, lx: +lk[0], ly: +lk[1], st: H('X-State') };
      var w = +H('X-Preview-Width'), h = +H('X-Preview-Height');
      return r.arrayBuffer().then(function (b) { return { px: new Uint8Array(b), w: w, h: h, m: m }; });
    }).then(function (p) {
      if (p && p.w > 0 && p.px.length >= p.w * p.h) { lastFrame = p.m.frame; OAG.emit('preview', p); }
      else OAG.emit('preview', null);
    }).catch(function () { OAG.emit('preview', null); });

    var star = fetch('/guide/star').then(function (r) {
      if (!r.ok) return null;
      var c = (r.headers.get('X-Crop') || '').split(','), s = (r.headers.get('X-Stretch') || '').split(',');
      return r.arrayBuffer().then(function (b) {
        return { px: new Uint16Array(b), x0: +c[0], y0: +c[1], n: +c[2], lo: +s[0], hi: +s[1] };
      });
    }).then(function (c) { if (c) OAG.emit('crop', c); }).catch(function () {});
    return Promise.all([pv, star]);
  }

  function tick() {
    if (busy) return;
    busy = true;
    fetch('/guide/state?since=' + lastSeq).then(function (r) { return r.json(); }).then(function (s) {
      if (epoch !== null && s.hist_epoch !== epoch) {
        /* The device discarded its history (new source): start over, and
         * ask for everything on the next tick. */
        OAG.hist = []; lastSeq = -1; epoch = s.hist_epoch;
        OAG.emit('hist-reset');
      } else {
        epoch = s.hist_epoch;
        if (s.hist.length) {
          s.hist.forEach(function (r) { OAG.hist.push(row(r)); });
          if (OAG.hist.length > HIST_MAX) OAG.hist.splice(0, OAG.hist.length - HIST_MAX);
          lastSeq = OAG.hist[OAG.hist.length - 1].seq;
          OAG.emit('hist');
        }
      }
      OAG.state = s;
      OAG.emit('state', s);
      if (s.frame !== lastFrame) return fetchFrame();
    }).catch(function () { OAG.emit('offline'); }).then(function () { busy = false; });
  }

  /* Image scale in arc-seconds per pixel, or 0 when unknown. */
  OAG.scale = function () { return (OAG.state && OAG.state.pixel_scale) || 0; };

  OAG.settings = {};
  OAG.loadSettings = function () {
    return fetch('/guide/settings').then(function (r) { return r.json(); }).then(function (j) {
      OAG.settings = j; OAG.emit('settings', j); return j;
    }).catch(function () {});
  };
  OAG.setSetting = function (key, value) {
    return OAG.put('/guide/settings', 'key=' + encodeURIComponent(key) + '&value=' + encodeURIComponent(value))
      .then(function (r) {
        if (!r.ok) return r.text().then(function (t) { throw new Error(t.replace(/^Error \d+: [^\n]*\n/, '')); });
        return r.json().then(function (j) { OAG.settings = j; OAG.emit('settings', j); return j; });
      });
  };

  OAG.start = function () {
    OAG.loadSettings();
    /* The sensor's exposure ceiling, from the Alpaca camera alongside. Absent
     * on the host test server, where there is no camera. */
    fetch('/api/v1/camera/0/exposuremax?ClientID=1&ClientTransactionID=1').then(function (r) { return r.json(); })
      .then(function (j) { if (j && !j.ErrorNumber) { OAG.exposureMax = j.Value; OAG.emit('expmax', j.Value); } })
      .catch(function () {});
    tick();
    /* 100 ms keeps up with the sensor's shortest frame (133 ms at hts_mult=4);
     * one request in flight at a time, so a slow link backs off. */
    setInterval(tick, 100);
  };
})();
