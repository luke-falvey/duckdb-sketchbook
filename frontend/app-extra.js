// Parts of the editor around the cells: the catalog sidebar, the notebook picker, the dependency
// graph, Ctrl+Space completion and the chart switcher under query results.
var AppExtra = (function () {
  'use strict';
  var X = { tables: [] };
  var $ = function (id) { return document.getElementById(id); };

  X.catalog = function () {
    App.api('GET', '/api/catalog').then(function (r) {
      X.tables = r.tables || [];
      var side = $('sidebar');
      side.innerHTML = '';
      side.appendChild(NB.el('h3', { text: 'Cells' }));
      App.cells.forEach(function (c) {
        if (!c.name || c.kind === 'markdown') return;
        side.appendChild(NB.el('div', { 'class': 'nb-col', style: 'padding-left:4px', text: (c.kind === 'input' ? '$' : '') + c.name, title: c.kind, onclick: function () {
          var el = document.querySelector('.nb-cell[data-idx="' + App.cells.indexOf(c) + '"]');
          if (el) el.scrollIntoView({ behavior: 'smooth', block: 'center' });
        } }));
      });
      side.appendChild(NB.el('h3', { text: 'Catalog' }));
      var groups = {};
      X.tables.forEach(function (t) { var g = t.database + '.' + t.schema; (groups[g] = groups[g] || []).push(t); });
      Object.keys(groups).sort().forEach(function (g) {
        side.appendChild(NB.el('div', { 'class': 'nb-coltype', text: g }));
        groups[g].forEach(function (t) {
          // A click on a table opens it to show its columns, so a double-click inserts its name.
          var d = NB.el('details', {}, [NB.el('summary', { text: t.name + (t.kind === 'view' ? ' (view)' : ''), title: 'Double-click to insert the name, or drag it into a cell', draggable: 'true' })]);
          d.querySelector('summary').addEventListener('dragstart', function (e) { e.dataTransfer.setData('text/plain', t.qualified); });
          (t.columns || []).forEach(function (col) {
            var row = NB.el('div', { 'class': 'nb-col', title: 'Click to insert the name, or drag it into a cell', onclick: function () { X.insert(col.name); }, draggable: 'true' }, [NB.el('span', { text: col.name }), NB.el('span', { 'class': 'nb-coltype', text: col.type })]);
            row.addEventListener('dragstart', function (e) { e.dataTransfer.setData('text/plain', col.name); });
            d.appendChild(row);
          });
          d.querySelector('summary').addEventListener('dblclick', function () { X.insert(t.qualified); });
          side.appendChild(d);
        });
      });
    }).catch(function () {});
  };

  X.insert = function (text) {
    var ta = X.lastTextarea;
    if (!ta || !document.body.contains(ta)) { App.status('Click into a cell first'); return; }
    ta.setRangeText(text, ta.selectionStart, ta.selectionEnd, 'end');
    ta.dispatchEvent(new Event('input'));
    ta.focus();
  };
  document.addEventListener('focusin', function (e) { if (e.target.tagName === 'TEXTAREA') X.lastTextarea = e.target; });

  X.picker = function () {
    App.api('GET', '/api/notebooks').then(function (r) {
      var sel = $('open');
      sel.innerHTML = '';
      sel.appendChild(NB.el('option', { value: '', text: 'Open…' }));
      (r.files || []).concat((r.stored || []).map(function (n) { return 'db:' + n; })).forEach(function (s) {
        sel.appendChild(NB.el('option', { value: s, text: s, selected: s === App.source ? 'selected' : null }));
      });
      sel.appendChild(NB.el('option', { value: '__new', text: 'New notebook…' }));
      sel.onchange = function () {
        var v = sel.value;
        if (v === '__new') v = prompt('Path of the new notebook', 'untitled.sql');
        if (!v) return;
        location.search = '?token=' + encodeURIComponent(App.token) + '&nb=' + encodeURIComponent(v) +
          (App.database ? '&db=' + encodeURIComponent(App.database) : '');
      };
    }).catch(function () {});
  };

  // ---- Graph view: cells in file order, edges from what they read. -------------------------------
  X.graph = function () {
    var box = $('graph');
    box.innerHTML = '';
    if (!App.graph) return;
    var cells = App.cells, rowH = 30, w = 760, h = cells.length * rowH + 10;
    box.className = 'nb-graph';
    var svg = NB.svg('svg', { viewBox: '0 0 ' + w + ' ' + h, width: '100%' }, box);
    cells.forEach(function (c, i) {
      var y = 5 + i * rowH;
      var r = NB.svg('rect', { x: 200, y: y, width: 340, height: rowH - 8, rx: 4, 'class': c.state || 'stale' }, svg);
      r.style.cursor = 'pointer';
      r.addEventListener('click', function () { var el = document.querySelector('.nb-cell[data-idx="' + i + '"]'); if (el) el.scrollIntoView({ behavior: 'smooth', block: 'center' }); });
      var t = NB.svg('text', { x: 208, y: y + 16 }, svg);
      t.textContent = NB.kindName(c) + '  ' + (c.name || '(line ' + c.start_line + ')') + '  [' + (c.state || 'stale') + ']';
      (c.depends_on || []).forEach(function (d, k) {
        // The curve bends left by three quarters of its control points' offset: at most 120 of the 200px
        // to the left of the boxes, so the deepest stays in the view box.
        var y0 = 5 + d * rowH + (rowH - 8) / 2, y1 = y + (rowH - 8) / 2, depth = 20 + ((i + d + k) % 8) * 20;
        var forward = d > i, bend = 200 - depth;
        NB.svg('path', { d: 'M200,' + y0 + 'C' + bend + ',' + y0 + ' ' + bend + ',' + y1 + ' 200,' + y1, 'class': 'edge', 'stroke-dasharray': forward ? '4 3' : null }, svg);
        NB.svg('circle', { cx: 200, cy: y1, r: 2.5, fill: 'var(--nb-muted)' }, svg);
      });
      if ((c.errors || []).length) { var e = NB.svg('text', { x: 548, y: y + 16, fill: 'var(--nb-error)' }, svg); e.textContent = c.errors[0].slice(0, 30); }
    });
  };

  // ---- Completion: cell names, tables, columns, variables. ---------------------------------------
  X.complete = function (ta, cell) {
    var before = ta.value.slice(0, ta.selectionStart), m = /[$\w.]*$/.exec(before), word = m ? m[0] : '';
    var words = {};
    App.cells.forEach(function (c) { if (c.name) words[c.kind === 'input' ? '$' + c.name : c.name] = c.kind; (c.defines || []).forEach(function (d) { words[d] = 'defined'; }); });
    X.tables.forEach(function (t) { words[t.name] = t.kind; (t.columns || []).forEach(function (col) { words[col.name] = col.type; }); });
    var options = Object.keys(words).filter(function (k) { return k.toLowerCase().indexOf(word.toLowerCase()) === 0 && k !== word; }).sort().slice(0, 12);
    var old = document.querySelector('.nb-complete');
    if (old) old.remove();
    if (!options.length) return;
    var list = NB.el('select', { 'class': 'nb-complete', size: Math.min(8, options.length), style: 'position:absolute;z-index:10' });
    options.forEach(function (o) { list.appendChild(NB.el('option', { value: o, text: o + '  ' + words[o] })); });
    var rect = ta.getBoundingClientRect(), lines = before.split('\n').length;
    list.style.left = (rect.left + window.scrollX + 12) + 'px';
    list.style.top = (rect.top + window.scrollY + Math.min(rect.height, lines * 18) + 4) + 'px';
    var pick = function () {
      ta.setRangeText(list.value, ta.selectionStart - word.length, ta.selectionStart, 'end');
      ta.dispatchEvent(new Event('input'));
      list.remove();
      ta.focus();
    };
    list.addEventListener('keydown', function (e) { if (e.key === 'Enter' || e.key === 'Tab') { e.preventDefault(); pick(); } else if (e.key === 'Escape') { list.remove(); ta.focus(); } });
    list.addEventListener('click', pick);
    document.body.appendChild(list);
    list.selectedIndex = 0;
    list.focus();
  };

  // ---- Chart picker: build a chart from a result and write it into the cell as a /* chart */ comment. ----
  X.switcher = function (cell, columns) {
    var box = NB.el('div', { 'class': 'nb-switcher nb-hide-reading' });
    var marks = ['suggested', 'line', 'bar', 'area', 'point', 'histogram', 'rect', 'kpi', 'table'];
    var mark = NB.el('select', {}), x = NB.el('select', {}), y = NB.el('select', {}), color = NB.el('select', {});
    marks.forEach(function (m) { mark.appendChild(NB.el('option', { value: m, text: m })); });
    [x, y, color].forEach(function (sel, i) {
      sel.appendChild(NB.el('option', { value: '', text: ['x', 'y', 'color'][i] + '…' }));
      columns.forEach(function (c) { sel.appendChild(NB.el('option', { value: c.name, text: c.name })); });
    });
    var spec = function () {
      var m = mark.value;
      if (m === 'suggested' && !x.value && !y.value) return NB.suggestChart(columns);
      if (m === 'kpi') return 'type: kpi\nvalue: {field: ' + (y.value || x.value || columns[0].name) + ', agg: sum}';
      if (m === 'table') return 'type: table';
      var lines = ['mark: ' + (m === 'suggested' ? 'bar' : m)];
      if (x.value) lines.push('x: ' + x.value);
      if (y.value) lines.push('y: ' + y.value);
      else if (m === 'bar' && x.value) lines.push('y: {agg: count}');
      if (color.value) lines.push('color: ' + color.value);
      return lines.join('\n');
    };
    box.appendChild(NB.el('span', { text: 'Chart:' }));
    [mark, x, y, color].forEach(function (s) { box.appendChild(s); });
    box.appendChild(NB.el('button', { 'class': 'nb-add-chart', text: 'Add chart', onclick: function () { App.addChart(cell, spec()); } }));
    box.appendChild(NB.el('button', { 'class': 'nb-switcher-close', text: '✕', title: 'Close', onclick: function () { cell.picking = false; App.render(); } }));
    return box;
  };

  return X;
})();
