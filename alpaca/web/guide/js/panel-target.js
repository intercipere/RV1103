/*
 * Target: a port of PHD2's TargetWindow / TargetClient (target.cpp).
 * Four rings at radius_max * i/4 labelled i/2/zoom (arc-seconds when the image
 * scale is known), tick marks every quarter ring, axes labelled RA and Dec;
 * points plotted as -RA (east positive) and -Dec offset (north up), in
 * (127,127,255), the newest as a red cross. Length 50..400 (default 100),
 * zoom x2 steps from 0.25 to 4 (default 1), optional reference circle,
 * radius 0.1..10 (default 2.0, off).
 */
(function () {
  'use strict';
  var OAG = window.OAG, el = OAG.el, P = OAG.prefs;

  OAG.panel({ id: 'target', title: 'Target', resizable: true, build: function (body) {
    var cv = el('canvas', { class: 'target' });
    var clearSeq = -1;
    var len = el('select', { class: 'gsel', 'aria-label': 'Target length' });
    [50, 100, 200, 400].forEach(function (v) {
      var o = el('option', { value: v, text: String(v) }); if (v === P.get('target.length', 100)) o.selected = true; len.appendChild(o);
    });
    len.addEventListener('change', function () { P.set('target.length', +len.value); draw(); });
    var zin = el('button', { text: '+', 'aria-label': 'Zoom in', onclick: function () {
      var z = P.get('target.zoom', 1); if (z < 3) P.set('target.zoom', z * 2); draw();
    } });
    var zout = el('button', { text: '−', 'aria-label': 'Zoom out', onclick: function () {
      var z = P.get('target.zoom', 1); if (z > 0.25) P.set('target.zoom', z / 2); draw();
    } });
    var refOn = el('input', { type: 'checkbox' });
    refOn.checked = P.get('target.refCircleEnabled', false);
    refOn.addEventListener('change', function () { P.set('target.refCircleEnabled', refOn.checked); draw(); });
    var refR = el('input', { type: 'number', min: 0.1, max: 10, step: 0.1, value: P.get('target.refCircleRadius', 2.0), class: 'num', 'aria-label': 'Reference circle radius' });
    refR.addEventListener('change', function () {
      var v = Math.min(10, Math.max(0.1, +refR.value || 2)); refR.value = v; P.set('target.refCircleRadius', v); draw();
    });
    var side = el('div', { class: 'tside' }, [len,
      el('div', { class: 'tzoom' }, [zin, zout]),
      el('button', { text: 'Clear', onclick: function () { clearSeq = OAG.hist.length ? OAG.hist[OAG.hist.length - 1].seq : -1; draw(); } }),
      el('label', { class: 'gchk' }, [refOn, ' Reference circle']),
      el('label', { class: 'gchk' }, ['Radius: ', refR])]);
    body.appendChild(el('div', { class: 'twrap' }, [side, el('div', { class: 'tcv' }, [cv])]));

    function draw() {
      var f = OAG.fit(cv), ctx = f.ctx, W = f.w, H = f.h;
      var zoom = P.get('target.zoom', 1), length = P.get('target.length', 100);
      var sampling = OAG.scale() || 1, hasScale = OAG.scale() > 0;
      ctx.fillStyle = OAG.color('--g-bg'); ctx.fillRect(0, 0, W, H);
      var cx = W / 2, cy = H / 2;
      var rmax = (Math.min(W, H) - 6) / 2, leftEdge = cx - rmax;
      rmax -= 18;
      if (rmax < 10) rmax = 10;
      var scale = rmax / 2 * sampling, i;
      var grey = OAG.color('--t-ring');

      if (P.get('target.refCircleEnabled', false)) {
        ctx.fillStyle = OAG.color('--t-ref');
        ctx.beginPath(); ctx.arc(cx, cy, P.get('target.refCircleRadius', 2) * scale * zoom / sampling, 0, 2 * Math.PI); ctx.fill();
      }
      ctx.strokeStyle = grey; ctx.lineWidth = 1; ctx.fillStyle = OAG.color('--g-text');
      ctx.font = '10px ' + OAG.color('--sans'); ctx.textAlign = 'right'; ctx.textBaseline = 'bottom';
      for (i = 1; i <= 4; i++) {
        var rr = rmax * i / 4;
        ctx.beginPath(); ctx.arc(cx, cy, rr, 0, 2 * Math.PI); ctx.stroke();
        ctx.fillText(+(i / 2 / zoom).toFixed(3) + (hasScale ? "''" : ''), cx - 1, cy - rr);
      }
      ctx.beginPath();
      ctx.moveTo(3, cy + 0.5); ctx.lineTo(W - 3, cy + 0.5); ctx.moveTo(cx + 0.5, 3); ctx.lineTo(cx + 0.5, H - 3);
      var r = rmax / (2 / zoom), g = Math.max(2, W / 100), x;
      for (x = r / 4; x < W; x += r / 4) {
        ctx.moveTo(cx + x, cy - g); ctx.lineTo(cx + x, cy + g); ctx.moveTo(cx - x, cy - g); ctx.lineTo(cx - x, cy + g);
        ctx.moveTo(cx - g, cy + x); ctx.lineTo(cx + g, cy + x); ctx.moveTo(cx - g, cy - x); ctx.lineTo(cx + g, cy - x);
      }
      ctx.stroke();
      ctx.textAlign = 'left';
      var samples = OAG.hist.filter(function (h) { return h.seq > clearSeq; }).slice(-length);
      var radec = samples.length && (samples[samples.length - 1].flags & OAG.CALIBRATED);
      ctx.fillText(radec ? 'RA' : 'dx', leftEdge, cy - 3);
      ctx.fillText(radec ? 'Dec' : 'dy', cx + 5, cy - rmax);

      /* PHD2: RA offset is distance W of lock, plot -dRA (east positive);
       * Dec offset is distance S of lock, plot -dDec (north up). Without a
       * calibration there are only camera axes, plotted as dx right, dy down. */
      ctx.strokeStyle = OAG.color('--t-point');
      samples.forEach(function (h, j) {
        var px = radec ? cx - h.ra * scale * zoom : cx + h.dx * scale * zoom;
        /* yimpact = cy - dec * s * decSign, decSign = -1 */
        var py = radec ? cy + h.dec * scale * zoom : cy + h.dy * scale * zoom;
        if (j === samples.length - 1) {
          var lc = 4;
          ctx.strokeStyle = OAG.color('--t-last');
          ctx.beginPath(); ctx.moveTo(px + lc, py + lc); ctx.lineTo(px - lc - 1, py - lc - 1);
          ctx.moveTo(px + lc, py - lc); ctx.lineTo(px - lc - 1, py + lc + 1); ctx.stroke();
        } else {
          ctx.beginPath(); ctx.arc(px, py, 1, 0, 2 * Math.PI); ctx.stroke();
        }
      });
    }
    OAG.on('hist', draw);
    OAG.on('hist-reset', function () { clearSeq = -1; draw(); });
    OAG.on('scheme', draw);
    return { redraw: draw, contentHeight: function () { return cv.clientHeight; } };
  } });
})();
