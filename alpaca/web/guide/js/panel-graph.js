/*
 * History graph: a port of PHD2's GraphLogWindow / GraphLogClientWindow
 * (graph.cpp). Constants, scales, colours, sign conventions and statistics
 * are PHD2's:
 *   x: 50/100/200/400 steps (default 100); y: +/-1,2,4,8,16 (default 4) in
 *   arc-seconds when the image scale is known, else pixels; 3 dotted divisions
 *   per half; RA/Dec mode plots +RA downward and -Dec ("north corrections up,
 *   north offsets down"); corrections are outlined bars, West and North up;
 *   trend lines are least-squares fits drawn long-dash across the full width;
 *   RMS is the population sigma over the plotted window, and RA Osc is the
 *   fraction of consecutive RA steps that changed sign.
 * Choices are kept in this browser, as PHD2 keeps them in its global config.
 */
(function () {
  'use strict';
  var OAG = window.OAG, el = OAG.el, P = OAG.prefs;
  var LENGTHS = [50, 100, 200, 400], HEIGHTS = [1, 2, 4, 8, 16];

  function shade(rgb, pct) {
    /* wxColour::ChangeLightness(pct < 100): towards black. */
    var m = /rgba?\(([^)]+)\)/.exec(rgb), c;
    if (m) c = m[1].split(',').map(Number);
    else if (/^#[0-9a-f]{6}$/i.test(rgb)) c = [1, 3, 5].map(function (i) { return parseInt(rgb.substr(i, 2), 16); });
    else return rgb;
    return 'rgb(' + c.slice(0, 3).map(function (v) { return Math.round(v * pct / 100); }).join(',') + ')';
  }

  /* PHD2 trendline(): least squares over x = 0..n-1. */
  function trend(ys) {
    var n = ys.length, sy = 0, sxy = 0, i;
    for (i = 0; i < n; i++) { sy += ys[i]; sxy += i * ys[i]; }
    var sx = 0.5 * n * (n - 1), denom = n * n * (n - 1) * ((2 * n - 1) / 6 - 0.25 * (n - 1));
    var a = (n * sxy - sx * sy) / denom;
    return [a, (sy - a * sx) / n];
  }
  function sigma(v) {
    var n = v.length, s = 0, s2 = 0;
    if (!n) return 0;
    v.forEach(function (x) { s += x; s2 += x * x; });
    return Math.sqrt(Math.max(0, n * s2 - s * s)) / n;
  }

  OAG.panel({ id: 'graph', title: 'History', resizable: true, build: function (body) {
    var cv = el('canvas', { class: 'graph' });
    var clearSeq = -1;

    function pick(label, list, cur, fmt, key) {
      var s = el('select', { class: 'gsel', 'aria-label': label });
      list.forEach(function (v) { var o = el('option', { value: v, text: fmt(v) }); if (v === cur) o.selected = true; s.appendChild(o); });
      s.addEventListener('change', function () { P.set(key, +s.value); draw(); });
      return s;
    }
    function check(label, key, def) {
      var cb = el('input', { type: 'checkbox' });
      cb.checked = P.get(key, def);
      cb.addEventListener('change', function () { P.set(key, cb.checked); draw(); });
      return el('label', { class: 'gchk' }, [cb, ' ' + label]);
    }
    var xSel = pick('Graph length', LENGTHS, P.get('graph.length', 100), function (v) { return 'x:' + v; }, 'graph.length');
    var ySel = el('select', { class: 'gsel', 'aria-label': 'Graph height' });
    function yLabels() {
      var arc = units() === 'arcsec', cur = P.get('graph.height', 4);
      ySel.innerHTML = '';
      HEIGHTS.forEach(function (v) {
        var o = el('option', { value: v, text: 'y: +/-' + v + (arc ? "''" : ' px') });
        if (v === cur) o.selected = true;
        ySel.appendChild(o);
      });
    }
    ySel.addEventListener('change', function () { P.set('graph.height', +ySel.value); draw(); });

    /* PHD2's "Settings" button menu. */
    function radio(name, key, val, def, text) {
      var r = el('input', { type: 'radio', name: name });
      r.checked = P.get(key, def) === val;
      r.addEventListener('change', function () { P.set(key, val); yLabels(); draw(); });
      return el('label', { class: 'gmi' }, [r, ' ' + text]);
    }
    var cs = el('input', { type: 'checkbox' });
    cs.checked = P.get('graph.corrScale', false);
    cs.addEventListener('change', function () { P.set('graph.corrScale', cs.checked); draw(); });
    var settings = el('details', { class: 'gset' }, [el('summary', { text: 'Settings' }),
      el('div', { class: 'gmenu' }, [
        radio('gmode', 'graph.mode', 'radec', 'radec', 'RA / Dec'),
        radio('gmode', 'graph.mode', 'dxdy', 'radec', 'dx / dy'),
        el('hr'),
        radio('gunits', 'graph.units', 'arcsec', 'arcsec', 'Arc-seconds'),
        radio('gunits', 'graph.units', 'px', 'arcsec', 'Pixels'),
        el('hr'),
        el('label', { class: 'gmi' }, [cs, ' Corrections to scale'])])]);

    var lRa = el('span', { class: 'lra', text: 'RA' }), lDec = el('span', { class: 'ldec', text: 'Dec' });
    var rRa = el('span'), rDec = el('span'), rTot = el('span'), osc = el('div', { class: 'osc' });
    var side = el('div', { class: 'gside' }, [
      el('div', { class: 'gbtns' }, [xSel, ySel, settings,
        el('button', { class: 'gclr', text: 'Clear', onclick: function () {
          clearSeq = OAG.hist.length ? OAG.hist[OAG.hist.length - 1].seq : -1; draw();
        } })]),
      el('div', { class: 'gchks' }, [check('Trendlines', 'graph.trend', false), check('Corrections', 'graph.corr', true)]),
      el('div', { class: 'gleg' }, [lRa, lDec]),
      el('div', { class: 'grms' }, [el('div', { text: 'RMS Error:' }),
        el('div', {}, [el('span', { class: 'k', text: 'RA ' }), rRa]),
        el('div', {}, [el('span', { class: 'k', text: 'Dec ' }), rDec]),
        el('div', {}, [el('span', { class: 'k', text: 'Tot ' }), rTot])]),
      osc]);
    body.appendChild(el('div', { class: 'gwrap' }, [side, el('div', { class: 'gcv' }, [cv])]));
    yLabels();

    function units() { return P.get('graph.units', 'arcsec') === 'arcsec' && OAG.scale() > 0 ? 'arcsec' : 'px'; }

    function draw() {
      var f = OAG.fit(cv), ctx = f.ctx, W = f.w, H = f.h;
      var length = P.get('graph.length', 100), height = P.get('graph.height', 4);
      var scale = OAG.scale(), arc = units() === 'arcsec';
      var samples = OAG.hist.filter(function (h) { return h.seq > clearSeq; }).slice(-length);
      var calibrated = samples.length && (samples[samples.length - 1].flags & OAG.CALIBRATED);
      var radec = P.get('graph.mode', 'radec') === 'radec' && calibrated;
      var cRa = OAG.color('--g-ra'), cDec = OAG.color('--g-dec');
      lRa.textContent = radec ? 'RA' : 'dx'; lDec.textContent = radec ? 'Dec' : 'dy';

      var B = 5, top = B, bottom = H - B, left = 0, right = W - B, cx = W / 2, cy = H / 2;
      var yDiv = 3, xDiv = length / 50 - 1;
      var xPPD = W / 2 / (xDiv + 1), yPPD = H / 2 / (yDiv + 1), i;
      ctx.fillStyle = OAG.color('--g-bg'); ctx.fillRect(0, 0, W, H);
      ctx.strokeStyle = OAG.color('--g-axis'); ctx.lineWidth = 1; ctx.setLineDash([]);
      ctx.beginPath();
      ctx.moveTo(cx + 0.5, top); ctx.lineTo(cx + 0.5, bottom); ctx.moveTo(left, cy + 0.5); ctx.lineTo(right, cy + 0.5);
      ctx.rect(left + 0.5, top + 0.5, right - left - 1, bottom - top - 1);
      ctx.stroke();
      ctx.strokeStyle = OAG.color('--g-grid'); ctx.setLineDash([1, 2]);
      ctx.fillStyle = OAG.color('--g-text'); ctx.font = '11px ' + OAG.color('--sans'); ctx.textBaseline = 'alphabetic';
      ctx.beginPath();
      for (i = 1; i <= yDiv; i++) {
        var v = i * height / (yDiv + 1), yu = cy - i * yPPD, yd = cy + i * yPPD;
        ctx.moveTo(left, yu + 0.5); ctx.lineTo(right, yu + 0.5); ctx.moveTo(left, yd + 0.5); ctx.lineTo(right, yd + 0.5);
        ctx.fillText(v + (arc ? "''" : (i === 3 ? ' px' : '')), left + 3, yu - 2);
        ctx.fillText(-v + (arc ? "''" : ''), left + 3, yd - 2);
      }
      for (i = 1; i <= xDiv; i++) {
        ctx.moveTo(cx - i * xPPD + 0.5, top); ctx.lineTo(cx - i * xPPD + 0.5, bottom);
        ctx.moveTo(cx + i * xPPD + 0.5, top); ctx.lineTo(cx + i * xPPD + 0.5, bottom);
      }
      ctx.stroke(); ctx.setLineDash([]);

      var xmag = W / length, ymag = yPPD * (yDiv + 1) / height * (arc ? scale : 1);
      var X = function (j) { return j * xmag; }, Y = function (v, m) { return cy + v * m; };

      if (P.get('graph.corr', true)) {
        ctx.font = '10px ' + OAG.color('--sans');
        ctx.fillStyle = shade(cDec, 75); ctx.textAlign = 'right'; ctx.textBaseline = 'top';
        ctx.fillText('GuideNorth', right - 4, top + 2);
        ctx.fillStyle = shade(cRa, 75); ctx.textBaseline = 'bottom';
        ctx.fillText('GuideEast', right - 4, bottom - 2);
        ctx.textAlign = 'left'; ctx.textBaseline = 'alphabetic';
        if (samples.length) {
          var toScale = P.get('graph.corrScale', false);
          var st = OAG.state || {}, cal = st.cal || {};
          var xRate = (cal.ra_rate || 1000) / 1000, yRate = (cal.dec_rate || 1000) / 1000;
          var maxDur = 1;
          samples.forEach(function (h) { maxDur = Math.max(maxDur, h.raMs, h.decMs); });
          var ymagc = toScale ? ymag : (H - 10) * 0.5 / maxDur;
          var bar = function (j, dur, dx, c) {
            var y = Y(dur, ymagc), x = X(j) + dx;
            ctx.strokeStyle = c;
            if (dur < 0) ctx.strokeRect(x + 0.5, y + 0.5, 4, cy - y); else ctx.strokeRect(x + 0.5, cy + 0.5, 4, y - cy);
          };
          samples.forEach(function (h, j) {
            if (h.raMs) bar(j, (h.raDir === OAG.DIR.WEST ? -h.raMs : h.raMs) * (toScale ? xRate : 1), 0, shade(cRa, 60));
            if (h.decMs) bar(j, (h.decDir === OAG.DIR.SOUTH ? h.decMs : -h.decMs) * (toScale ? yRate : 1), 5, shade(cDec, 60));
          });
        }
      }

      var v1 = samples.map(function (h) { return radec ? h.ra : h.dx; });
      var v2 = samples.map(function (h) { return radec ? -h.dec : h.dy; });
      function line(vals, c, dash) {
        if (vals.length < 1) return;
        ctx.strokeStyle = c; ctx.lineWidth = 2; ctx.setLineDash(dash || []); ctx.lineJoin = 'round';
        ctx.beginPath();
        vals.forEach(function (v, j) { if (j) ctx.lineTo(X(j), Y(v, ymag)); else ctx.moveTo(X(j), Y(v, ymag)); });
        ctx.stroke(); ctx.setLineDash([]);
      }
      line(v1, cRa); line(v2, cDec);
      if (P.get('graph.trend', false) && samples.length >= 5) {
        [[v1, cRa], [v2, cDec]].forEach(function (q) {
          var t = trend(q[0]);
          ctx.strokeStyle = q[1]; ctx.lineWidth = 2; ctx.setLineDash([8, 5]);
          ctx.beginPath(); ctx.moveTo(X(0), Y(t[1], ymag)); ctx.lineTo(X(length), Y(t[0] * length + t[1], ymag)); ctx.stroke();
          ctx.setLineDash([]);
        });
      }

      /* Statistics over the same window. PHD2 always measures them in mount
       * axes; without a calibration there are only camera axes. */
      var sr = sigma(samples.map(function (h) { return radec ? h.ra : h.dx; }));
      var sd = sigma(samples.map(function (h) { return radec ? h.dec : h.dy; }));
      var lbl = function (v) { return v.toFixed(2) + (scale ? ' (' + (v * scale).toFixed(2) + "'')" : ''); };
      rRa.textContent = lbl(sr); rDec.textContent = lbl(sd); rTot.textContent = lbl(Math.hypot(sr, sd));
      var n = samples.length, same = 0;
      for (i = 1; i < n; i++) if ((radec ? samples[i].ra * samples[i - 1].ra : samples[i].dx * samples[i - 1].dx) > 0) same++;
      var oi = n >= 2 ? 1 - same / (n - 1) : 0;
      osc.textContent = (radec ? 'RA' : 'dx') + ' Osc: ' + oi.toFixed(2);
      osc.classList.toggle('alert', n >= 2 && (oi > 0.6 || oi < 0.15));
    }

    OAG.on('hist', draw);
    OAG.on('hist-reset', function () { clearSeq = -1; draw(); });
    OAG.on('scheme', draw);
    var lastScale = -1;
    OAG.on('state', function () { var s = OAG.scale(); if (s !== lastScale) { lastScale = s; yLabels(); draw(); } });
    return { redraw: draw, contentHeight: function () { return cv.clientHeight; } };
  } });
})();
