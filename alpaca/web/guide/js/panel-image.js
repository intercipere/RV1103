/*
 * The guide image with PHD2's overlay (guider.cpp / guider_multistar.cpp):
 *   star box, search-region sized: (100,255,90) selected, (32,196,32) while
 *     calibrating or guiding, dotted (230,130,30) when the star is lost;
 *   lock position, full-width crosshair: dotted (255,255,0) while calibrating,
 *     solid (0,255,0) while guiding.
 *
 * Two stacked canvases. The image canvas holds the preview at its own
 * resolution (the device sends it downsampled 4x, 576x324) and the browser
 * scales it up. The overlay canvas sits on top at the *screen's* resolution, so
 * the box and crosshair stay one crisp line at any size -- drawing them into the
 * preview pixels and scaling the result up is what made them look smeared.
 *
 * The preview arrives already stretched to 8 bits; the gamma here is PHD2's
 * display gamma (toolbar slider, 10..300, default 100 = linear), display only.
 */
(function () {
  'use strict';
  var OAG = window.OAG, el = OAG.el;

  OAG.panel({ id: 'image', title: 'Guide camera', resizable: true, build: function (body, actions) {
    var cv = el('canvas', { class: 'img', width: 576, height: 324 });
    var ov = el('canvas', { class: 'ov' });
    var empty = el('div', { class: 'empty' }, ['No image yet', el('span', { text: 'Press Loop or Guide to start taking frames' })]);
    var meta = el('span', { class: 'meta' });
    actions.appendChild(meta);
    var frame = el('div', { class: 'frame' }, [cv, ov, empty]);
    body.appendChild(frame);
    var ctx = cv.getContext('2d'), img = null, last = null, lut = new Uint8Array(256), lutGamma = -1;

    function buildLut(g) {
      for (var i = 0; i < 256; i++) lut[i] = Math.round(255 * Math.pow(i / 255, 100 / g));
      lutGamma = g;
    }

    /* Fit the image to the panel width, and to a height that leaves the rest
     * of the default desktop layout on screen -- or to the height the user
     * dragged the panel to. */
    function size() {
      var aspect = cv.width / cv.height, W = body.clientWidth - 22;
      var ph = parseFloat(getComputedStyle(body.parentNode).getPropertyValue('--ph'));
      var maxH = ph > 0 ? ph : (document.getElementById('dock').classList.contains('phone') ? Infinity
        : Math.max(240, window.innerHeight - 570));
      var w = Math.max(50, Math.min(W, maxH * aspect)), h = w / aspect;
      frame.style.width = w + 'px'; frame.style.height = h + 'px';
    }

    function draw() {
      if (last) {
        var p = last, w = p.w, h = p.h, i, v, d;
        if (cv.width !== w || cv.height !== h || !img) { cv.width = w; cv.height = h; img = ctx.createImageData(w, h); }
        var g = OAG.prefs.get('gamma', 100);
        if (g !== lutGamma) buildLut(g);
        d = img.data;
        for (i = 0; i < w * h; i++) { v = lut[p.px[i]]; d[i * 4] = v; d[i * 4 + 1] = v; d[i * 4 + 2] = v; d[i * 4 + 3] = 255; }
        ctx.putImageData(img, 0, 0);
      }
      size();
      overlay();
    }

    function overlay() {
      var f = OAG.fit(ov), o = f.ctx;
      o.clearRect(0, 0, f.w, f.h);
      if (!last) return;
      var m = last.m, cal = m.st === 'calibrating';
      var s = f.w / (last.w * m.ds); /* CSS px per full-frame pixel */
      var crisp = function (x) { return Math.round(x) + 0.5; };
      o.lineWidth = 1;
      if (m.lock) {
        var lx = crisp(m.lx * s), ly = crisp(m.ly * s);
        o.setLineDash(cal ? [3, 3] : []);
        o.strokeStyle = OAG.color(cal ? '--o-lock-cal' : '--o-lock');
        o.beginPath(); o.moveTo(0, ly); o.lineTo(f.w, ly); o.moveTo(lx, 0); o.lineTo(lx, f.h); o.stroke();
      }
      if (m.sel) {
        /* True scale, 2 * search region + 1 frame pixels, with a 5 px floor so
         * it cannot vanish on a small screen. */
        var hb = Math.max((m.sr + 0.5) * s, 5);
        var x0 = crisp(m.sx * s - hb), y0 = crisp(m.sy * s - hb), side = Math.round(2 * hb);
        o.setLineDash(m.found ? [] : [3, 3]);
        o.strokeStyle = OAG.color(!m.found ? '--o-box-lost' : (m.st === 'selected' || m.st === 'looping') ? '--o-box-sel' : '--o-box');
        o.strokeRect(x0, y0, side, side);
      }
      o.setLineDash([]);
    }

    OAG.on('preview', function (p) {
      if (p) { last = p; empty.hidden = true; draw(); } else if (!last) empty.hidden = false;
    });
    OAG.on('gamma', draw);
    OAG.on('scheme', draw);
    OAG.on('state', function (s) {
      meta.textContent = s.star_found ? ('star ' + (s.star_index + 1) + ' of ' + s.star_count) : '';
    });
    window.addEventListener('resize', draw);
    return { redraw: draw, contentHeight: function () { return frame.clientHeight; } };
  } });
})();
