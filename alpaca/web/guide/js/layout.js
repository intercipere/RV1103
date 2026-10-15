/*
 * Movable panels, PHD2-style: every view is a panel that can be dragged by its
 * title bar, collapsed, closed, and brought back from the View menu -- the web
 * equivalent of PHD2's dockable wxAUI panes and its View menu.
 *
 * Desktop: a full-width top area, 1-3 resizable columns, a full-width bottom
 *          area (PHD2's main window: graph across the top, image centre,
 *          target and star profile docked right).
 * Phone:   one column, in an order of its own.
 *
 * The two arrangements are stored separately in this browser, so arranging the
 * phone never disturbs the laptop. "Reset layout" restores the default.
 *
 * Panels that hold a drawing (graph, image, target, star profile) can also be
 * resized by dragging the grip on their bottom edge. The size is applied as
 * the CSS variable --ph on the panel, which each view uses for its canvas;
 * double-clicking the grip restores the default.
 */
(function () {
  'use strict';
  var OAG = window.OAG, el = OAG.el;
  var panels = {}, order = []; /* registration order = default order */
  var dock, mode, L;

  var DEFAULT_DESKTOP = {
    top: ['graph'],
    /* Toolbar under the image rather than in the bottom area: the bottom area
     * sits below the tallest column, which on a default layout is the right
     * one, so the toolbar would end up off screen. */
    cols: [['image', 'toolbar'], ['target', 'profile', 'calibration', 'equipment', 'source', 'algorithms', 'stats']],
    w: [2.2, 1],
    bottom: [],
    hidden: [],
    collapsed: ['source', 'algorithms', 'stats']
  };
  var DEFAULT_PHONE = {
    cols: [['image', 'toolbar', 'graph', 'target', 'profile', 'calibration', 'equipment', 'source', 'algorithms', 'stats']],
    hidden: [],
    collapsed: ['target', 'profile', 'equipment', 'source', 'algorithms', 'stats']
  };

  function clone(o) { return JSON.parse(JSON.stringify(o)); }

  OAG.panel = function (def) {
    /* def: {id, title, build(body, actions) -> {redraw?}} */
    var body = el('div', { class: 'pb' });
    var actions = el('span', { class: 'pa' });
    var btnCollapse = el('button', { class: 'pbtn', title: 'Collapse', 'aria-label': 'Collapse ' + def.title, text: '▾' });
    var btnClose = el('button', { class: 'pbtn', title: 'Close (re-open from View)', 'aria-label': 'Close ' + def.title, text: '×' });
    var head = el('header', { class: 'ph' }, [el('span', { class: 'pt', text: def.title }), actions, btnCollapse, btnClose]);
    var sec = el('section', { class: 'panel', 'data-id': def.id }, [head, body]);
    var p = { def: def, el: sec, body: body, head: head };
    if (def.resizable) sec.appendChild(grip(p));
    panels[def.id] = p;
    order.push(def.id);
    p.view = def.build(body, actions) || {};
    btnCollapse.addEventListener('click', function () { toggle(L.collapsed, def.id); apply(); save(); });
    btnClose.addEventListener('click', function () { L.hidden.push(def.id); apply(); save(); });
    head.addEventListener('pointerdown', function (e) { startDrag(e, p); });
    if (window.ResizeObserver)
      new ResizeObserver(function () { if (p.view.redraw) p.view.redraw(); }).observe(body);
    return p;
  };

  function grip(p) {
    var g = el('div', { class: 'grip', title: 'Drag to resize, double-click to reset', 'aria-hidden': 'true' });
    g.addEventListener('pointerdown', function (e) {
      e.preventDefault();
      g.setPointerCapture(e.pointerId);
      /* Start from what the panel shows now, whether default or dragged. */
      var y0 = e.clientY, h0 = p.view.contentHeight ? p.view.contentHeight() : p.body.clientHeight;
      function mv(ev) {
        var h = Math.max(80, Math.min(4000, Math.round(h0 + ev.clientY - y0)));
        p.el.style.setProperty('--ph', h + 'px');
        L.sizes[p.def.id] = h;
        if (p.view.redraw) p.view.redraw();
      }
      function up() {
        g.removeEventListener('pointermove', mv); g.removeEventListener('pointerup', up);
        OAG.prefs.set('layout.' + mode, L);
      }
      g.addEventListener('pointermove', mv); g.addEventListener('pointerup', up);
    });
    g.addEventListener('dblclick', function () {
      delete L.sizes[p.def.id];
      p.el.style.removeProperty('--ph');
      OAG.prefs.set('layout.' + mode, L);
      if (p.view.redraw) p.view.redraw();
    });
    return g;
  }

  function toggle(list, id) {
    var i = list.indexOf(id);
    if (i < 0) list.push(id); else list.splice(i, 1);
  }

  function currentMode() {
    var forced = OAG.prefs.get('layoutMode', 'auto');
    if (forced === 'phone' || forced === 'desktop') return forced;
    return window.innerWidth < 900 ? 'phone' : 'desktop';
  }

  function load() {
    var def = mode === 'phone' ? DEFAULT_PHONE : DEFAULT_DESKTOP;
    var l = OAG.prefs.get('layout.' + mode, null) || clone(def);
    l.top = l.top || []; l.bottom = l.bottom || []; l.cols = l.cols && l.cols.length ? l.cols : [[]];
    l.hidden = l.hidden || []; l.collapsed = l.collapsed || []; l.sizes = l.sizes || {};
    l.w = l.w || l.cols.map(function () { return 1; });
    /* A panel added in a newer build than the saved layout must still show up
     * somewhere: put it at the end of the last column. */
    var placed = [].concat(l.top, l.bottom, l.hidden);
    l.cols.forEach(function (c) { placed = placed.concat(c); });
    order.forEach(function (id) { if (placed.indexOf(id) < 0) l.cols[l.cols.length - 1].push(id); });
    return l;
  }

  function save() {
    /* Read the arrangement back from the DOM, which is the truth after a drag. */
    var areas = dock.querySelectorAll('.area');
    var cols = [], top = [], bottom = [];
    Array.prototype.forEach.call(areas, function (a) {
      var ids = Array.prototype.map.call(a.querySelectorAll(':scope > .panel'), function (x) { return x.getAttribute('data-id'); });
      if (a.dataset.area === 'top') top = ids;
      else if (a.dataset.area === 'bottom') bottom = ids;
      else cols.push(ids);
    });
    L.top = top; L.bottom = bottom; L.cols = cols;
    OAG.prefs.set('layout.' + mode, L);
    buildViewMenu();
  }

  function apply() {
    dock.innerHTML = '';
    dock.className = 'dock ' + mode;
    var mk = function (name, ids, cls) {
      var a = el('div', { class: 'area ' + (cls || ''), 'data-area': name });
      ids.forEach(function (id) {
        var p = panels[id];
        if (!p || L.hidden.indexOf(id) >= 0) return;
        p.el.classList.toggle('collapsed', L.collapsed.indexOf(id) >= 0);
        if (L.sizes[id]) p.el.style.setProperty('--ph', L.sizes[id] + 'px');
        else p.el.style.removeProperty('--ph');
        a.appendChild(p.el);
      });
      return a;
    };
    if (mode === 'desktop') dock.appendChild(mk('top', L.top, 'full'));
    var row = el('div', { class: 'cols' });
    L.cols.forEach(function (ids, i) {
      if (i > 0) row.appendChild(splitter(i - 1));
      var c = mk('c' + i, ids, 'col');
      c.style.flex = (L.w[i] || 1) + ' 1 0';
      row.appendChild(c);
    });
    dock.appendChild(row);
    if (mode === 'desktop') dock.appendChild(mk('bottom', L.bottom, 'full'));
    Object.keys(panels).forEach(function (id) { var v = panels[id].view; if (v.redraw) v.redraw(); });
    buildViewMenu();
  }

  /* ----------------------------------------------------------- splitters -- */
  function splitter(i) {
    var s = el('div', { class: 'split', title: 'Drag to resize columns' });
    s.addEventListener('pointerdown', function (e) {
      e.preventDefault();
      var cols = dock.querySelectorAll('.cols > .col');
      var a = cols[i], b = cols[i + 1], x0 = e.clientX;
      var wa = a.getBoundingClientRect().width, wb = b.getBoundingClientRect().width, sum = L.w[i] + L.w[i + 1];
      s.setPointerCapture(e.pointerId);
      function mv(ev) {
        var d = ev.clientX - x0, na = Math.max(160, wa + d), nb = Math.max(160, wb - d);
        L.w[i] = sum * na / (na + nb); L.w[i + 1] = sum * nb / (na + nb);
        a.style.flex = L.w[i] + ' 1 0'; b.style.flex = L.w[i + 1] + ' 1 0';
      }
      function up() {
        s.removeEventListener('pointermove', mv); s.removeEventListener('pointerup', up);
        OAG.prefs.set('layout.' + mode, L);
      }
      s.addEventListener('pointermove', mv); s.addEventListener('pointerup', up);
    });
    return s;
  }

  /* ---------------------------------------------------------------- drag -- */
  function startDrag(e, p) {
    if (e.button !== 0 || e.target.closest('button,select,input,label,a')) return;
    var x0 = e.clientX, y0 = e.clientY, dragging = false, marker = null, target = null;
    var head = p.head;
    head.setPointerCapture(e.pointerId);
    function mv(ev) {
      if (!dragging) {
        if (Math.abs(ev.clientX - x0) + Math.abs(ev.clientY - y0) < 8) return;
        dragging = true;
        dock.classList.add('dragging');
        p.el.classList.add('moving');
        marker = el('div', { class: 'drop' });
      }
      var under = document.elementsFromPoint(ev.clientX, ev.clientY);
      var area = null, k;
      for (k = 0; k < under.length; k++) if (under[k].classList && under[k].classList.contains('area')) { area = under[k]; break; }
      if (!area) return;
      var kids = Array.prototype.filter.call(area.children, function (c) { return c.classList.contains('panel') && c !== p.el; });
      var before = null;
      for (k = 0; k < kids.length; k++) {
        var r = kids[k].getBoundingClientRect();
        if (ev.clientY < r.top + r.height / 2) { before = kids[k]; break; }
      }
      area.insertBefore(marker, before);
      target = { area: area, before: before };
    }
    function up() {
      head.removeEventListener('pointermove', mv); head.removeEventListener('pointerup', up);
      if (!dragging) return;
      dock.classList.remove('dragging');
      p.el.classList.remove('moving');
      if (marker && marker.parentNode) marker.parentNode.removeChild(marker);
      if (target) { target.area.insertBefore(p.el, target.before); save(); if (p.view.redraw) p.view.redraw(); }
    }
    head.addEventListener('pointermove', mv); head.addEventListener('pointerup', up);
  }

  /* ----------------------------------------------------------- View menu -- */
  var menu;
  function buildViewMenu() {
    if (!menu) return;
    menu.innerHTML = '';
    menu.appendChild(el('div', { class: 'mh', text: 'Panels' }));
    order.forEach(function (id) {
      var on = L.hidden.indexOf(id) < 0;
      var cb = el('input', { type: 'checkbox' });
      cb.checked = on;
      cb.addEventListener('change', function () {
        if (cb.checked) { toggle(L.hidden, id); L.cols[L.cols.length - 1].push(id); dedupe(id); }
        else L.hidden.push(id);
        apply(); save();
      });
      menu.appendChild(el('label', { class: 'mi' }, [cb, ' ' + panels[id].def.title]));
    });
    if (mode === 'desktop') {
      menu.appendChild(el('div', { class: 'mh', text: 'Columns' }));
      var seg = el('div', { class: 'mseg' });
      [1, 2, 3].forEach(function (n) {
        var b = el('button', { text: String(n), 'aria-pressed': String(L.cols.length === n) });
        b.addEventListener('click', function () { setColumns(n); });
        seg.appendChild(b);
      });
      menu.appendChild(seg);
    }
    menu.appendChild(el('div', { class: 'mh', text: 'Layout' }));
    var sel = el('select', { 'aria-label': 'Layout mode' });
    [['auto', 'Automatic'], ['desktop', 'Desktop'], ['phone', 'Phone']].forEach(function (o) {
      var op = el('option', { value: o[0], text: o[1] });
      if (OAG.prefs.get('layoutMode', 'auto') === o[0]) op.selected = true;
      sel.appendChild(op);
    });
    sel.addEventListener('change', function () { OAG.prefs.set('layoutMode', sel.value); relayout(true); });
    menu.appendChild(sel);
    menu.appendChild(el('button', { class: 'mreset', text: 'Reset layout', onclick: function () {
      OAG.prefs.set('layout.' + mode, null); L = load(); apply();
    } }));
  }

  function dedupe(id) {
    /* Re-showing a panel appends it to the last column; drop any stale
     * position it still had elsewhere. */
    var seen = false;
    var strip = function (list) {
      for (var i = list.length - 1; i >= 0; i--) if (list[i] === id) { if (seen) list.splice(i, 1); seen = true; }
    };
    for (var c = L.cols.length - 1; c >= 0; c--) strip(L.cols[c]);
    strip(L.top); strip(L.bottom);
  }

  function setColumns(n) {
    while (L.cols.length > n) { var last = L.cols.pop(); L.w.pop(); L.cols[L.cols.length - 1] = L.cols[L.cols.length - 1].concat(last); }
    while (L.cols.length < n) { L.cols.push([]); L.w.push(1); }
    apply(); save();
  }

  function relayout(force) {
    var m = currentMode();
    if (m === mode && !force) return;
    mode = m; L = load(); apply();
  }

  OAG.layoutInit = function (dockEl, menuEl) {
    dock = dockEl; menu = menuEl;
    mode = currentMode(); L = load(); apply();
    var t;
    window.addEventListener('resize', function () { clearTimeout(t); t = setTimeout(function () { relayout(false); }, 150); });
  };
})();
