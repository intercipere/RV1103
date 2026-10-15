/*
 * Star profile: a port of PHD2's ProfileWindow (star_profile.cpp).
 *   left:  the profile of a 21x21 subframe around the star -- mid row, average
 *          row or average column (click to cycle) -- in red, with PHD2's FWHM;
 *   right: the star zoomed, green dotted crosshair at the lock position, red
 *          cross at the centroid; click it for raw (nearest) / interpolated.
 *          "Interpolated" is PHD2's wxIMAGE_QUALITY_HIGH, which for enlarging
 *          is wxWidgets' ResampleBicubic -- a cubic B-spline, ported below
 *          rather than left to the browser's bilinear scaling, which is
 *          visibly sharper than what PHD2 shows;
 *   text:  Peak, FWHM, HFD in pixels and arc-seconds; a large HFD readout in
 *          "focusing mode", when the panel is taller than about half its width.
 *
 * One deliberate difference: PHD2 zooms the *displayed* image, at least 15
 * display pixels around the lock -- on a big sensor shown small that is a
 * large, blurry region (PHD2's own comment calls it "unusable"). Here the
 * zoom is taken from full-resolution pixels, +/-15 frame pixels, which is what
 * PHD2 shows when its display scale is 1.
 */
(function () {
  'use strict';
  var OAG = window.OAG, el = OAG.el, P = OAG.prefs;
  var FULLW = 21, HALFW = 10, ZOOM = 15, BS_MAX = 256;

  /* wxWidgets image.cpp: spline_cube / spline_weight. */
  function splineCube(v) { return v <= 0 ? 0 : v * v * v; }
  function splineWeight(v) {
    return (splineCube(v + 2) - 4 * splineCube(v + 1) + 6 * splineCube(v) - 4 * splineCube(v - 1)) / 6;
  }
  /* wxWidgets ResampleBicubic's per-axis precalculation: destination d maps to
   * source d * (old-1)/(new-1), taps at -1..+2 clamped to the edge. Cached:
   * it depends only on the two sizes. */
  var pcCache = {};
  function precalc(oldDim, newDim) {
    var key = oldDim + 'x' + newDim;
    if (pcCache[key]) return pcCache[key];
    var off = new Int32Array(newDim * 4), w = new Float64Array(newDim * 4);
    var sf = newDim > 1 ? (oldDim - 1) / (newDim - 1) : 0, d, k;
    for (d = 0; d < newDim; d++) {
      var src = d * sf, dd = src - Math.trunc(src);
      for (k = -1; k <= 2; k++) {
        off[d * 4 + k + 1] = src + k < 0 ? 0 : src + k >= oldDim ? oldDim - 1 : Math.trunc(src + k);
        w[d * 4 + k + 1] = splineWeight(k - dd);
      }
    }
    return (pcCache[key] = { off: off, w: w });
  }
  var bsOut = null;
  function resampleBSpline(v, S, D) {
    var pc = precalc(S, D), off = pc.off, w = pc.w, x, y, i, j;
    if (!bsOut || bsOut.length !== D * D) bsOut = new Float64Array(D * D);
    for (y = 0; y < D; y++) {
      for (x = 0; x < D; x++) {
        var sum = 0;
        for (j = 0; j < 4; j++) {
          var row = off[y * 4 + j] * S, wy = w[y * 4 + j];
          for (i = 0; i < 4; i++) sum += v[row + off[x * 4 + i]] * wy * w[x * 4 + i];
        }
        bsOut[y * D + x] = sum;
      }
    }
    return bsOut;
  }
  /* Grey values to pixels, tinted for the Night scheme. */
  function paint(c, v, n) {
    var im = c.createImageData(n, n), d = im.data, tint = OAG.color('--p-tint').split(',').map(Number), i;
    for (i = 0; i < n * n; i++) {
      var t = Math.max(0, Math.min(255, Math.round(v[i])));
      d[i * 4] = t * tint[0]; d[i * 4 + 1] = t * tint[1]; d[i * 4 + 2] = t * tint[2]; d[i * 4 + 3] = 255;
    }
    c.putImageData(im, 0, 0);
  }

  OAG.panel({ id: 'profile', title: 'Star Profile', resizable: true, build: function (body) {
    var cv = el('canvas', { class: 'profile' });
    body.appendChild(el('div', { class: 'pcv' }, [cv]));
    var crop = null, meta = null, mode = 0, imgBox = null;
    var zc = document.createElement('canvas'), zctx = zc.getContext('2d');

    cv.addEventListener('click', function (e) {
      var r = cv.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
      if (imgBox && x > imgBox.x && y <= imgBox.y + imgBox.w) P.set('profile.raw', !P.get('profile.raw', false));
      else mode = (mode + 1) % 3;
      draw();
    });

    function draw() {
      var f = OAG.fit(cv), ctx = f.ctx, xs = f.w, ys = f.h, st = OAG.state;
      ctx.fillStyle = OAG.color('--p-bg'); ctx.fillRect(0, 0, xs, ys);
      imgBox = null;
      if (!st || !crop) return;

      var red = OAG.color('--p-fg'), small = 11, sans = OAG.color('--sans');
      var focusing = ys > xs / 2 + 20;
      var hfd = st.hfd || 0, scale = OAG.scale();
      var hfdArc = hfd * (scale || 1);
      var largeH = 0, labelH;
      if (focusing) {
        largeH = Math.min(ys / 2, Math.max(small, (xs - 20) / 6));
        labelH = 5 + small + largeH + 5;
      } else labelH = 5 + small + 5;

      /* The 21x21 subframe, as PHD2's UpdateData takes it: centred on the
       * rounded star position, clamped to the crop. */
      var N = crop.n, sx0 = Math.round(st.star_x) - HALFW - crop.x0, sy0 = Math.round(st.star_y) - HALFW - crop.y0;
      sx0 = Math.max(0, Math.min(N - FULLW, sx0)); sy0 = Math.max(0, Math.min(N - FULLW, sy0));
      var mid = [], horiz = [], vert = [], x, y;
      for (x = 0; x < FULLW; x++) { horiz[x] = 0; vert[x] = 0; }
      for (y = 0; y < FULLW; y++) for (x = 0; x < FULLW; x++) {
        var v = crop.px[(sy0 + y) * N + sx0 + x];
        horiz[x] += v; vert[y] += v;
        if (y === HALFW) mid[x] = v;
      }
      var prof = [mid, horiz, vert][mode], label = ['Mid row', 'Avg row', 'Avg col'][mode];

      var pmin = prof[0], pmax = prof[0], i;
      for (i = 1; i < FULLW; i++) { if (prof[i] < pmin) pmin = prof[i]; else if (prof[i] > pmax) pmax = prof[i]; }
      var fwhm = 0;
      if (pmin < pmax) {
        var pmid = Math.floor((pmax - pmin) / 2) + pmin;
        var range = (pmax - pmin) / (ys - labelH - 5) || 1;
        var wprof = Math.floor(Math.floor((xs - 15) / 2 - 5) / 20);
        /* PHD2's FWHM: last upward and downward crossings of the half
         * maximum, linearly interpolated -- kept exactly, edge cases too. */
        var x1 = 0, x2 = 0, pv, pp;
        for (i = 1; i < FULLW; i++) {
          pv = prof[i]; pp = prof[i - 1];
          if (pp <= pmid && pv >= pmid) x1 = i; else if (pp >= pmid && pv <= pmid) x2 = i;
        }
        var f1 = x1 - (prof[x1] - pmid) / (prof[x1] - prof[x1 - 1]);
        var f2 = x2 - (prof[x2 - 1] - pmid) / (prof[x2 - 1] - prof[x2]);
        fwhm = f2 - f1;
        /* With no crossing PHD2 reads one element before the array (undefined
         * behaviour in C); here that is NaN, shown as 0. */
        if (!isFinite(fwhm)) fwhm = 0;
        ctx.strokeStyle = red; ctx.lineWidth = 1;
        ctx.beginPath();
        for (i = 0; i < FULLW; i++) {
          var px = 5 + i * wprof, py = ys - labelH - (prof[i] - pmin) / range;
          if (i) ctx.lineTo(px, py); else ctx.moveTo(px, py);
        }
        ctx.stroke();
      }

      /* Zoomed star. */
      var left = (xs - 15) / 2, width = xs - left - 5;
      if (width > ys + 5) width = ys - 5;
      var imgTop = 30, half = width / 2;
      var lx = meta && meta.lock ? meta.lx : st.star_x, ly = meta && meta.lock ? meta.ly : st.star_y;
      var cx0 = Math.round(lx) - ZOOM - crop.x0, cy0 = Math.round(ly) - ZOOM - crop.y0, S = 2 * ZOOM;
      cx0 = Math.max(0, Math.min(N - S, cx0)); cy0 = Math.max(0, Math.min(N - S, cy0));
      if (width > 10) {
        /* 8-bit display values first (stretch + gamma), as PHD2 resamples its
         * displayed image, not the raw data. */
        var lo = crop.lo, hi = Math.max(crop.lo + 1, crop.hi), g = 100 / P.get('gamma', 100);
        var v8 = new Float64Array(S * S);
        for (y = 0; y < S; y++) for (x = 0; x < S; x++) {
          var t = (crop.px[(cy0 + y) * N + cx0 + x] - lo) / (hi - lo);
          v8[y * S + x] = Math.round(Math.pow(Math.max(0, Math.min(1, t)), g) * 255);
        }
        var raw = P.get('profile.raw', false);
        if (raw) {
          /* wxIMAGE_QUALITY_NEAREST */
          zc.width = S; zc.height = S;
          paint(zctx, v8, S);
          ctx.imageSmoothingEnabled = false;
        } else {
          /* wxIMAGE_QUALITY_HIGH, enlarging: wxWidgets' ResampleBicubic, a cubic
           * B-spline -- a smoothing, bell-shaped kernel, which is why PHD2's
           * "interpolated" star looks so soft.
           *
           * Capped at BS_MAX px a side, and the browser scales the (already
           * smooth) result the rest of the way. Resampling straight to device
           * pixels cost ~48 ms a frame in a large panel at 1x DPI and ~4x that
           * on a high-DPI screen -- every frame, at 7.5 frames/s, which pegged
           * a CPU core and made the whole PC sluggish. A 30-pixel source has no
           * detail beyond this size; the cap changes nothing visible. */
          var D = Math.max(2, Math.min(BS_MAX, Math.round(width * (window.devicePixelRatio || 1))));
          zc.width = D; zc.height = D;
          paint(zctx, resampleBSpline(v8, S, D), D);
          ctx.imageSmoothingEnabled = true;
        }
        ctx.drawImage(zc, left, imgTop, width, width);
        imgBox = { x: left, y: imgTop, w: width };
        ctx.fillStyle = red; ctx.font = small + 'px ' + sans; ctx.textBaseline = 'bottom';
        ctx.fillText(raw ? 'Click image for interpolated view' : 'Click image for raw view', left, imgTop - 2);
        ctx.strokeStyle = OAG.color('--p-lock'); ctx.setLineDash([1, 2]);
        ctx.beginPath(); ctx.moveTo(left, imgTop + half); ctx.lineTo(left + width, imgTop + half);
        ctx.moveTo(left + half, imgTop); ctx.lineTo(left + half, imgTop + width); ctx.stroke(); ctx.setLineDash([]);
        /* centroid relative to the box centre (the lock, or the star) */
        var k = width / S;
        var bx = left + (lx - (cx0 + crop.x0)) * k, by = imgTop + (ly - (cy0 + crop.y0)) * k;
        var scx = bx + (st.star_x - lx) * k, scy = by + (st.star_y - ly) * k;
        ctx.strokeStyle = red;
        ctx.beginPath(); ctx.moveTo(scx - 5, scy); ctx.lineTo(scx + 5, scy); ctx.moveTo(scx, scy - 5); ctx.lineTo(scx, scy + 5); ctx.stroke();
      }

      ctx.fillStyle = red; ctx.font = small + 'px ' + sans; ctx.textBaseline = 'top';
      if (st.star_found) { ctx.fillText('Peak', 3, 3); ctx.fillText(String(st.peak), 3, 3 + small + 2); }
      ctx.textBaseline = 'alphabetic';
      if (hfd) {
        var arcTxt = scale ? hfdArc.toFixed(2) + '"' : '';
        if (focusing) {
          ctx.fillText(label + ' FWHM: ' + fwhm.toFixed(2), 5, ys - labelH + 5 + small);
          var xpos = 5;
          ctx.fillText('HFD: ', xpos, ys - largeH / 2 + small / 2 - 4);
          xpos += ctx.measureText('HFD: ').width;
          ctx.font = largeH + 'px ' + sans;
          ctx.fillText(hfd.toFixed(2), xpos, ys - 6);
          xpos += ctx.measureText(hfd.toFixed(2)).width;
          ctx.font = small + 'px ' + sans;
          if (arcTxt) ctx.fillText('  ' + arcTxt, xpos, ys - largeH / 2 + small / 2 - 4);
        } else {
          ctx.fillText(label + ' FWHM: ' + fwhm.toFixed(2) + ', HFD: ' + hfd.toFixed(2) + (arcTxt ? ' (' + arcTxt + ')' : ''), 5, ys - 6);
        }
      } else {
        ctx.fillText(label + ' FWHM: ' + fwhm.toFixed(2), 5, ys - 6);
      }
    }
    OAG.on('crop', function (c) { crop = c; draw(); });
    OAG.on('preview', function (p) { if (p) meta = p.m; });
    OAG.on('scheme', draw);
    OAG.on('gamma', draw);
    return { redraw: draw, contentHeight: function () { return cv.clientHeight; } };
  } });
})();
