/*
 * Toolbar, calibration, equipment, frame source, algorithms and stats panels,
 * plus PHD2's status bar.
 *
 * Toolbar: PHD2's main toolbar -- Loop, Auto-select, Guide, Stop, the exposure
 * list (myframe.cpp dur_values: 0.01..15 s) and the display gamma slider
 * (10..300, default 100). "Next star" is ours: PHD2 picks by clicking the image.
 *
 * The algorithm panel is interim. Every guiding parameter will come from the
 * settings registry generated from PHD2's source (Guiding/UI_PORTING_PLAN.md);
 * until then it exposes the handful wired so far.
 */
(function () {
  'use strict';
  var OAG = window.OAG, el = OAG.el, P = OAG.prefs;
  var DUR = [10, 20, 50, 100, 200, 500, 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000, 6000, 7000, 8000, 9000, 10000, 15000];
  function durLabel(ms) { /* MyFrame::ExposureDurationLabel */
    if (ms >= 10000) return (ms / 1000) + ' s';
    return (ms / 1000).toFixed(ms < 100 ? 2 : 1) + ' s';
  }
  function kv(label) {
    var dd = el('dd', { text: '--' });
    return { row: el('div', { class: 'kv' }, [el('dt', { text: label }), dd]), dd: dd };
  }

  /* ------------------------------------------------------------ toolbar -- */
  OAG.panel({ id: 'toolbar', title: 'Toolbar', build: function (body) {
    var bLoop = el('button', { class: 'tb', text: 'Loop', title: 'Begin looping exposures for frame and focus' });
    var bAuto = el('button', { class: 'tb', text: 'Auto-select', title: 'Auto-select the best guide star' });
    var bNext = el('button', { class: 'tb', text: 'Next star', title: 'Select the next candidate star' });
    var bGuide = el('button', { class: 'tb primary', text: 'Guide', title: 'Begin guiding (calibrate if needed)' });
    var bStop = el('button', { class: 'tb stop', text: 'Stop', title: 'Stop looping and guiding' });
    bLoop.onclick = function () { OAG.control('action=loop'); };
    bAuto.onclick = function () { OAG.control('action=autoselect'); };
    bNext.onclick = function () { OAG.control('action=reselect'); };
    bGuide.onclick = function () { OAG.control('action=start'); };
    bStop.onclick = function () { OAG.control('action=stop'); };

    var exp = el('select', { class: 'exp', 'aria-label': 'Exposure', title: 'Camera exposure duration' });
    function fillExp() {
      var cur = OAG.state ? Math.round(OAG.state.exposure * 1000) : 500, max = OAG.exposureMax;
      exp.innerHTML = '';
      DUR.forEach(function (ms) {
        var o = el('option', { value: ms / 1000, text: durLabel(ms) });
        if (max && ms / 1000 > max + 1e-9) { o.disabled = true; o.textContent += ' (over ' + max.toFixed(2) + ' s max)'; }
        if (ms === cur) o.selected = true;
        exp.appendChild(o);
      });
    }
    exp.addEventListener('change', function () { OAG.control('action=exposure&value=' + exp.value); });
    var gam = el('input', { type: 'range', min: 10, max: 300, value: P.get('gamma', 100), 'aria-label': 'Screen gamma', title: 'Screen gamma (brightness)' });
    gam.addEventListener('input', function () { P.set('gamma', +gam.value); OAG.emit('gamma'); });
    gam.addEventListener('dblclick', function () { gam.value = 100; P.set('gamma', 100); OAG.emit('gamma'); });

    body.appendChild(el('div', { class: 'tbar' }, [bLoop, bAuto, bNext, bGuide, bStop]));
    body.appendChild(el('div', { class: 'tbar2' }, [el('label', { class: 'fl' }, ['Exposure ', exp]), el('label', { class: 'fl grow' }, ['Gamma ', gam])]));
    fillExp();
    OAG.on('expmax', fillExp);
    var lastExp = -1;
    OAG.on('state', function (s) {
      if (s.exposure !== lastExp && document.activeElement !== exp) { lastExp = s.exposure; fillExp(); }
      var running = s.state !== 'stopped' && s.state !== 'error';
      bStop.disabled = !running;
      bGuide.classList.toggle('on', s.state === 'guiding' || s.state === 'calibrating');
      bLoop.classList.toggle('on', s.state === 'looping' || s.state === 'selected');
    });
  } });

  /* -------------------------------------------------------- calibration -- */
  OAG.panel({ id: 'calibration', title: 'Mount & calibration', build: function (body, actions) {
    var meta = el('span', { class: 'meta' }); actions.appendChild(meta);
    var line = el('p', { class: 'calline', text: '--' }), adv = el('div', { class: 'warn' });
    var ra = kv('RA axis'), dec = kv('Dec axis'), pulse = kv('Last pulse');
    var mode = el('select', { 'aria-label': 'Dec guide mode' });
    [['0', 'Off'], ['1', 'Auto'], ['2', 'North'], ['3', 'South']].forEach(function (o) { mode.appendChild(el('option', { value: o[0], text: o[1] })); });
    mode.addEventListener('change', function () { OAG.control('action=decmode&value=' + mode.value); });
    var bCal = el('button', { text: 'Calibrate', onclick: function () { OAG.control('action=calibrate'); } });
    var bClr = el('button', { text: 'Clear calibration', onclick: function () { OAG.control('action=clearcal'); } });
    var none = el('p', { class: 'note', text: 'No mount is connected, so guiding measures in camera pixels and sends nothing. The simulated source brings a simulated mount.' });
    body.appendChild(line); body.appendChild(adv);
    body.appendChild(el('dl', {}, [ra.row, dec.row, pulse.row]));
    body.appendChild(el('div', { class: 'field' }, [el('label', { text: 'Dec guide mode' }), mode]));
    body.appendChild(el('div', { class: 'btns' }, [bCal, bClr]));
    body.appendChild(none);
    function axis(a, r, n) { return a.toFixed(1) + '° · ' + r.toFixed(2) + ' px/s · ' + n + ' steps'; }
    function pl(p) { return p.ms > 0 ? p.dir + ' ' + p.ms + ' ms' : '—'; }
    OAG.on('state', function (s) {
      var mc = s.mount_connected;
      meta.textContent = s.mount;
      none.hidden = mc; bCal.disabled = !mc; bClr.disabled = !s.calibrated;
      line.textContent = !mc ? 'No mount' : s.state === 'calibrating' ? (s.cal_status || 'Calibrating')
        : s.calibrated ? 'Calibrated' : 'Not calibrated — Guide calibrates first';
      adv.textContent = s.cal_advisory || '';
      ra.dd.textContent = s.calibrated ? axis(s.cal.ra_angle, s.cal.ra_rate, s.cal.ra_steps) : '--';
      dec.dd.textContent = !s.calibrated ? '--' : s.cal.dec_rate === null ? 'not calibrated' : axis(s.cal.dec_angle, s.cal.dec_rate, s.cal.dec_steps);
      pulse.dd.textContent = mc && s.calibrated && s.state === 'guiding' ? pl(s.pulse_ra) + ' · ' + pl(s.pulse_dec) : '--';
      if (document.activeElement !== mode) mode.value = String(s.dec_mode);
    });
  } });

  /* ---------------------------------------------------------- equipment -- */
  OAG.panel({ id: 'equipment', title: 'Equipment', build: function (body) {
    var fl = el('input', { type: 'number', class: 'num', min: 1, max: 10000, step: 1, 'aria-label': 'Guide scope focal length' });
    var msg = el('div', { class: 'warn' });
    var px = kv('Pixel size'), sc = kv('Image scale'), fov = kv('Field of view');
    fl.addEventListener('change', function () {
      msg.textContent = '';
      OAG.setSetting('/frame/focalLength', fl.value).catch(function (e) { msg.textContent = e.message; });
    });
    body.appendChild(el('div', { class: 'field' }, [el('label', { text: 'Guide scope focal length, mm' }), fl]));
    body.appendChild(msg);
    body.appendChild(el('dl', {}, [px.row, sc.row, fov.row]));
    OAG.on('settings', function (j) {
      var f = j['/frame/focalLength'];
      if (f && document.activeElement !== fl) fl.value = f.value;
    });
    OAG.on('state', function (s) {
      px.dd.textContent = s.pixel_um ? s.pixel_um.toFixed(2) + ' µm' : '--';
      sc.dd.textContent = s.pixel_scale ? s.pixel_scale.toFixed(2) + "''/px" : 'unknown';
      fov.dd.textContent = s.pixel_scale
        ? (s.width * s.pixel_scale / 60).toFixed(1) + "' × " + (s.height * s.pixel_scale / 60).toFixed(1) + "'" : '--';
    });
  } });

  /* ------------------------------------------------------------- source -- */
  OAG.panel({ id: 'source', title: 'Frame source', build: function (body) {
    var cam = el('button', { text: 'Camera', 'aria-pressed': 'false', onclick: function () { OAG.control('action=source&value=0'); } });
    var sim = el('button', { text: 'Simulated', 'aria-pressed': 'false', onclick: function () { OAG.control('action=source&value=1'); } });
    body.appendChild(el('div', { class: 'seg' }, [cam, sim]));
    body.appendChild(el('p', { class: 'note', text: 'Simulated draws stars at known positions over a synthetic frame and moves them with a simulated mount. It needs no sky, no mount and no light, and it is the only mode with a known correct answer to check against.' }));
    OAG.on('state', function (s) {
      cam.setAttribute('aria-pressed', String(s.source === 'camera'));
      sim.setAttribute('aria-pressed', String(s.source === 'simulated'));
    });
  } });

  /* --------------------------------------------------------- algorithms -- */
  OAG.panel({ id: 'algorithms', title: 'Guide algorithms', build: function (body) {
    function sel(label, opts, onch) {
      var s = el('select', { 'aria-label': label });
      opts.forEach(function (o) { s.appendChild(el('option', { value: o[0], text: o[1] })); });
      s.addEventListener('change', function () { onch(s.value); });
      body.appendChild(el('div', { class: 'field' }, [el('label', { text: label }), s]));
      return s;
    }
    var ALG = [['0', 'Hysteresis'], ['1', 'Lowpass'], ['2', 'Resist Switch'], ['3', 'None']];
    var mm = sel('Minimum move, px', [['0', '0'], ['0.1', '0.10'], ['0.15', '0.15'], ['0.2', '0.20'], ['0.25', '0.25'], ['0.3', '0.30'], ['0.5', '0.50']],
      function (v) { OAG.control('action=minmove&value=' + v); });
    var ra = sel('RA algorithm', ALG, function (v) { OAG.control('action=algo&axis=ra&value=' + v); });
    var dec = sel('Dec algorithm', ALG, function (v) { OAG.control('action=algo&axis=dec&value=' + v); });
    body.appendChild(el('p', { class: 'note', text: 'Interim: the full set of PHD2 guiding parameters will appear here once the settings registry is ported.' }));
    var NAMES = { hysteresis: '0', lowpass: '1', 'resist switch': '2', identity: '3' };
    OAG.on('state', function (s) {
      if (document.activeElement !== mm) mm.value = String(+s.min_move.toFixed(2));
      if (document.activeElement !== ra && NAMES[s.algo_ra]) ra.value = NAMES[s.algo_ra];
      if (document.activeElement !== dec && NAMES[s.algo_dec]) dec.value = NAMES[s.algo_dec];
    });
  } });

  /* -------------------------------------------------------------- stats -- */
  OAG.panel({ id: 'stats', title: 'Stats', build: function (body) {
    var rows = ['Star position', 'SNR / HFD', 'Star mass', 'Frames / lost', 'Frame acquire', 'Preview build', 'Centroid'].map(kv);
    body.appendChild(el('dl', {}, rows.map(function (r) { return r.row; })));
    OAG.on('state', function (s) {
      rows[0].dd.textContent = s.star_found ? s.star_x.toFixed(2) + ', ' + s.star_y.toFixed(2) : '--';
      rows[1].dd.textContent = s.star_found ? s.snr.toFixed(1) + ' / ' + s.hfd.toFixed(2) + ' px' : '--';
      rows[2].dd.textContent = s.star_found ? String(Math.round(s.mass)) : '--';
      rows[3].dd.textContent = s.frame + ' / ' + s.lost;
      rows[4].dd.textContent = s.frame_ms.toFixed(0) + ' ms';
      rows[5].dd.textContent = s.preview_ms.toFixed(0) + ' ms';
      rows[6].dd.textContent = s.find_ms.toFixed(3) + ' ms';
    });
  } });

  /* --------------------------------------------------------- status bar -- */
  OAG.statusBar = function (bar) {
    var star = el('span'), pulse = el('span', { class: 'grow' });
    var cam = el('span', { class: 'ind', text: 'Camera' }), mnt = el('span', { class: 'ind', text: 'Mount' }), cal = el('span', { class: 'ind', text: 'Cal' });
    [star, pulse, cam, mnt, cal].forEach(function (x) { bar.appendChild(x); });
    function fmtPulse(h) {
      if (!h) return '';
      var parts = [], names = ['North', 'South', 'East', 'West'];
      if (h.raMs) parts.push(names[h.raDir] + ' ' + Math.abs(h.ra).toFixed(2) + ' px ' + h.raMs + ' ms');
      if (h.decMs) parts.push(names[h.decDir] + ' ' + Math.abs(h.dec).toFixed(2) + ' px ' + h.decMs + ' ms');
      return parts.join(', ');
    }
    OAG.on('state', function (s) {
      star.textContent = s.star_found ? 'm=' + Math.round(s.mass) + ' SNR=' + s.snr.toFixed(1) : (s.error || '');
      pulse.textContent = s.state === 'guiding' ? fmtPulse(OAG.hist[OAG.hist.length - 1]) : (s.state === 'calibrating' ? s.cal_status : '');
      cam.classList.toggle('on', s.state !== 'error');
      mnt.classList.toggle('on', s.mount_connected);
      cal.classList.toggle('on', s.calibrated);
    });
    OAG.on('offline', function () { star.textContent = 'no connection'; cam.classList.remove('on'); });
  };
})();
