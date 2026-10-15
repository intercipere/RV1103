/*
 * Boot: colour scheme, View menu, header, status bar, then start polling.
 * Panels have registered themselves by the time this runs (script order).
 *
 * Colour schemes change the page chrome. The graph, target, star profile and
 * image overlay keep PHD2's colours in every scheme except Night, which turns
 * everything red to protect dark adaptation.
 */
(function () {
  'use strict';
  var OAG = window.OAG, el = OAG.el;
  var SCHEMES = [['dark', 'Dark'], ['oled', 'OLED'], ['light', 'Light'], ['night', 'Night (red)']];

  function setScheme(s) {
    document.documentElement.setAttribute('data-scheme', s);
    OAG.prefs.set('scheme', s);
    OAG.emit('scheme', s);
  }

  document.addEventListener('DOMContentLoaded', function () {
    setScheme(OAG.prefs.get('scheme', 'dark'));
    var sch = OAG.$('#scheme');
    SCHEMES.forEach(function (o) {
      var op = el('option', { value: o[0], text: o[1] });
      if (o[0] === OAG.prefs.get('scheme', 'dark')) op.selected = true;
      sch.appendChild(op);
    });
    sch.addEventListener('change', function () { setScheme(sch.value); });

    var viewBtn = OAG.$('#viewBtn'), viewMenu = OAG.$('#viewMenu');
    viewBtn.addEventListener('click', function (e) {
      e.stopPropagation();
      viewMenu.hidden = !viewMenu.hidden;
      viewBtn.setAttribute('aria-expanded', String(!viewMenu.hidden));
    });
    document.addEventListener('click', function (e) {
      if (!viewMenu.hidden && !viewMenu.contains(e.target)) { viewMenu.hidden = true; viewBtn.setAttribute('aria-expanded', 'false'); }
    });

    OAG.layoutInit(OAG.$('#dock'), viewMenu);
    OAG.statusBar(OAG.$('#status'));

    var pill = OAG.$('#pill'), stateText = OAG.$('#stateText'), meta = OAG.$('#meta');
    OAG.on('state', function (s) {
      stateText.textContent = s.state;
      pill.className = 'pill ' + (s.state === 'guiding' ? 'go' : s.state === 'calibrating' ? 'cal'
        : (s.state === 'lost' || s.state === 'error') ? 'bad' : '');
      meta.textContent = s.source + ' · ' + s.exposure.toFixed(2) + ' s · frame ' + s.frame;
    });
    OAG.on('offline', function () { stateText.textContent = 'no connection'; pill.className = 'pill bad'; });
    OAG.$('#addr').textContent = location.host;
    OAG.start();
  });
})();
