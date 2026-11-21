// The notebook editor served by notebook_start. The .sql file is the source of truth: the page
// edits cells, rebuilds the file text from them, and asks the server to parse, run and save it.
var App = (function () {
  'use strict';
  var params = new URLSearchParams(location.search);
  var A = {
    // db: the database the page's stored notebooks are in (notebook_start puts its current database here).
    token: params.get('token') || '', source: params.get('nb') || '', database: params.get('db') || '', preamble: '', cells: [],
    dirty: false, autorun: true, live: false, reading: false, graph: false, selected: {}, running: false, lastText: '',
    // Rows per page for tables whose cell has no rows= option; remembered in this browser.
    pageSize: +(localStorage.getItem('nb.pageSize') || 25)
  };
  var $ = function (id) { return document.getElementById(id); };

  // The notebook every request is about: `nb=…`, and `&db=…` when the page has one.
  A.query = function () {
    return 'nb=' + encodeURIComponent(A.source) + (A.database ? '&db=' + encodeURIComponent(A.database) : '');
  };
  A.api = function (method, path, body) {
    var url = path + (path.indexOf('?') >= 0 ? '&' : '?') + A.query();
    var target = A.database ? { nb: A.source, db: A.database } : { nb: A.source };
    return fetch(url, {
      method: method, headers: { 'X-Notebook-Token': A.token, 'Content-Type': 'application/json' },
      body: body ? JSON.stringify(Object.assign(target, body)) : undefined
    }).then(function (r) {
      return r.json().then(function (j) { if (!r.ok || j.error) throw new Error(j.error || r.statusText); return j; });
    });
  };

  A.status = function (text) {
    $('status').textContent = text || '';
    $('status').title = text || '';
    if (window.Ink) Ink.present($('status'), 'toolbar/status', text || '', { update: 'now' });
  };
  // A problem the reader must see in full (the status line is cut short): a banner above the cells.
  A.problem = function (text) {
    var banner = $('banner');
    banner.innerHTML = '';
    banner.appendChild(NB.el('div', { 'class': 'nb-banner nb-problem' }, [
      NB.el('span', { 'class': 'nb-error', text: text }),
      NB.el('button', { text: 'Dismiss', onclick: function () { banner.innerHTML = ''; } })
    ]));
  };

  // ---- Text model ---------------------------------------------------------------------------
  A.text = function () {
    return A.preamble + A.cells.map(function (c) {
      return '-- %%' + (c.head ? ' ' + c.head : '') + '\n' + (c.body ? c.body.replace(/\s+$/, '') + '\n' : '');
    }).join('\n');
  };

  // Merges a server analysis into the cell list. Results stay with cells at the same position.
  A.merge = function (analysis) {
    analysis.forEach(function (a, i) {
      var c = A.cells[i] || (A.cells[i] = { state: 'stale' });
      ['idx', 'kind', 'name', 'widget', 'options', 'is_query', 'variables', 'defines', 'reads', 'depends_on', 'errors', 'warnings', 'content', 'start_line'].forEach(function (k) { c[k] = a[k]; });
      if (c.head === undefined) c.head = a.head;
      if (c.body === undefined) { c.body = a.body; c.sql = a.sql; c.specs = (a.chart_specs || []).slice(); }
      // A query cell is edited as its SQL plus its chart specs; the body is rebuilt from them.
      if (c.kind === 'sql' && c.sql === undefined) { c.sql = c.body || ''; c.specs = []; }
      if (c.kind === 'chart') c.specs = (a.chart_specs || []).slice();
    });
    A.cells.length = analysis.length;
  };

  A.load = function (data) {
    NB.setSqlWords(data.sql_words);
    A.preamble = data.preamble || '';
    A.cells = [];
    A.merge(data.cells);
    A.kept(data);
    // Whether the server will answer peeks: a cached result shown on opening is not a run.
    A.trusted = !!data.trusted;
    A.lastText = data.text;
    A.dirty = false;
    // Code is written when a notebook is opened or reloaded from disk, never while it is typed.
    A.generation = (A.generation || 0) + 1;
    $('title').textContent = data.title || 'Notebook';
    $('source').textContent = A.source;
    if (window.Ink) { Ink.present($('title'), 'toolbar/title'); Ink.present($('source'), 'toolbar/source'); }
    document.title = (data.title || A.source) + ' · DuckDB Notebook';
    A.render();
  };

  // The result cache (FORMAT §8): the last result of each cell, sent when a notebook is opened so the
  // page shows what it showed last time without running anything. A result made from inputs that
  // have changed since is stale. Until something runs in this session the page is a picture of the
  // last run (A.live is false): inputs and brushes do not run cells on their own.
  // A result arrives for a cell: kept (remembered from an earlier run, with when it was saved) or null.
  function show(c, result, kept) {
    c.result = result;
    c.kept = kept;
    c.offset = 0;
    if (result.choices) c.choices = result.choices;
    c.state = result.status === 'error' ? 'failed' : 'fresh';
  }
  // A remembered result goes: the cell waits to run again.
  function dropKept(c) { c.result = null; c.kept = null; c.state = 'stale'; }
  A.kept = function (data) {
    // Live once the reader has run something in this session and the text is still what they ran: text
    // reloaded from disk is not trusted (DESIGN §3), and the server pages and offers choices only when it is.
    A.live = !!data.ran && !!data.trusted;
    (data.cached || []).forEach(function (item) {
      var c = A.cells[item.idx];
      if (!c || !item.result) return;
      show(c, item.result, item.saved_at || true);
      if (item.stale) c.state = 'stale';
    });
    var shown = data.variables || {};
    A.cells.forEach(function (c) {
      if (c.kind !== 'input' || !(c.name in shown)) return;
      c.value = shown[c.name];
      if (c.result) c.result.value = shown[c.name];
      delete shown[c.name];
    });
    // What is left are chart selections: the brush or the picked marks are drawn again.
    Object.keys(shown).forEach(function (name) { A.selected[name] = shown[name]; });
    return (data.cached || []).length;
  };
  // A stale result that is still on the page: drawn fainter, with a note, until the cell runs.
  A.outdated = function (cell) {
    var r = cell.result;
    return cell.state === 'stale' && !!r && !!r.status && r.status !== 'skipped';
  };
  A.forget = function () {
    return A.api('POST', '/api/cache/clear', {}).then(function (r) {
      // Results this session computed stay on the page; ones only remembered go.
      A.cells.forEach(function (c) { if (c.kept) dropKept(c); });
      A.status(r.removed ? 'Forgot the results of ' + r.removed + ' cell' + (r.removed === 1 ? '' : 's') : 'No results were kept');
      A.render();
    }).catch(function (e) { A.status(e.message); });
  };

  // A query cell's body: its SQL, then one /* chart */ comment per chart.
  A.compose = function (cell) {
    cell.body = (cell.sql || '').replace(/\s+$/, '') + (cell.specs || []).map(function (spec) {
      return '\n\n/* chart\n' + spec.replace(/\s+$/, '') + '\n*/';
    }).join('');
  };

  A.analyse = function () {
    var text = A.text();
    return A.api('POST', '/api/notebook', { text: text }).then(function (r) { A.merge(r.cells); A.render(); });
  };
  var analyseTimer = null;
  // The text changed: mark it unsaved and parse it again once typing pauses for `ms`.
  function unsaved(ms) {
    A.dirty = true;
    clearTimeout(analyseTimer);
    analyseTimer = setTimeout(function () { A.analyse().catch(function (e) { A.status(e.message); }); }, ms);
    A.status('Unsaved changes');
  }
  A.edited = function (cell) {
    A.markStale(cell);
    unsaved(500);
  };

  // ---- Graph helpers ------------------------------------------------------------------------
  A.downstream = function (indexes) {
    var out = {}, frontier = indexes.slice();
    while (frontier.length) {
      var i = frontier.pop();
      A.cells.forEach(function (c, j) { if (!out[j] && (c.depends_on || []).indexOf(i) >= 0) { out[j] = true; frontier.push(j); } });
    }
    return Object.keys(out).map(Number).sort(function (a, b) { return a - b; });
  };
  A.readers = function (variable) {
    return A.cells.map(function (c, i) { return (c.reads || []).indexOf('$' + variable) >= 0 ? i : -1; }).filter(function (i) { return i >= 0; });
  };
  A.isAuto = function (c) {
    var run = c.options && c.options.run;
    if (run) return run === 'auto';
    return c.kind === 'markdown' || c.kind === 'chart' || (c.kind === 'sql' && c.is_query);
  };
  A.markStale = function (cell) {
    var i = A.cells.indexOf(cell);
    [i].concat(A.downstream([i])).forEach(function (j) { if (A.cells[j].state !== 'running') A.cells[j].state = 'stale'; });
  };

  // ---- Running ------------------------------------------------------------------------------
  // Runs cells (all of them without indexes). Cells in redraw only draw their charts again.
  A.run = function (indexes, redraw) {
    if (A.running) return Promise.resolve();
    A.running = true;
    A.live = true;
    A.trusted = true;
    $('interrupt').hidden = false;
    (indexes || A.cells.map(function (_, i) { return i; })).concat(redraw || []).forEach(function (i) { if (A.cells[i]) A.cells[i].state = 'running'; });
    A.render();
    A.status('Running…');
    var started = performance.now();
    return A.api('POST', '/api/run', { text: A.text(), cells: indexes || null, redraw: redraw || [], page_rows: A.pageSize }).then(function (r) {
      A.merge(r.cells);
      r.results.forEach(function (res) {
        var c = A.cells[res.idx];
        if (!c) return;
        if (res.redraw) {
          c.result = Object.assign({}, c.result, { charts: res.charts, status: res.status, error: res.error });
          c.state = res.status === 'error' ? 'failed' : 'fresh';
          return;
        }
        show(c, res, null);
      });
      A.cells.forEach(function (c) { if (c.state === 'running') c.state = 'stale'; });
      if ($('banner').querySelector('.nb-problem')) $('banner').innerHTML = '';
      A.status('Ran ' + r.results.length + ' cell' + (r.results.length === 1 ? '' : 's') + ' in ' + Math.round(performance.now() - started) + ' ms');
      AppExtra.catalog();
    }).catch(function (e) {
      A.cells.forEach(function (c) { if (c.state === 'running') c.state = 'failed'; });
      A.status('Run failed: see the message above the cells');
      A.problem(e.message);
    }).then(function () {
      A.running = false;
      $('interrupt').hidden = true;
      A.render();
    });
  };

  // Draws a query cell's charts again without running its SQL, once it has run.
  A.redraw = function (cell) {
    var r = cell.result;
    if (!r || !r.data || cell.state === 'stale') return A.runCell(cell);
    return A.run([], [A.cells.indexOf(cell)]);
  };

  A.runCell = function (cell) {
    if (window.Ink) Ink.fresh();
    var i = A.cells.indexOf(cell), list = [i];
    if (A.autorun) A.downstream([i]).forEach(function (j) { if (A.isAuto(A.cells[j])) list.push(j); });
    return A.run(list);
  };

  A.setVariable = function (name, value, cell) {
    return A.api('POST', '/api/variable', { name: name, value: value, cell: cell.idx }).then(function () {
      // A query cell whose SQL does not read the variable only filters its charts on it: redraw them.
      var readers = A.readers(name), redraw = readers.filter(function (j) {
        var c = A.cells[j];
        return c.kind === 'sql' && (c.variables || []).indexOf(name) < 0;
      });
      var full = readers.filter(function (j) { return redraw.indexOf(j) < 0; });
      var list = full.concat(A.downstream(full));
      list.forEach(function (j) { A.cells[j].state = 'stale'; });
      if (!A.live) {
        // Nothing has run in this session: the cells that read the value wait for a run.
        redraw.forEach(function (j) { A.cells[j].state = 'stale'; });
        A.status('Set ' + name + '. Nothing has run yet: press Run all.');
        A.render();
        return;
      }
      redraw = redraw.filter(function (j) { return list.indexOf(j) < 0 && A.cells[j].result && A.cells[j].result.data; });
      var auto = list.filter(function (j) { return A.isAuto(A.cells[j]); });
      auto = auto.filter(function (j, k) { return auto.indexOf(j) === k; }).sort(function (a, b) { return a - b; });
      if (A.autorun && (auto.length || redraw.length)) return A.run(auto, redraw);
      A.render();
    }).catch(function (e) { A.status(e.message); });
  };

  // A brush or a click on a chart. On a page that only shows the last run's results there is nothing
  // to filter yet.
  A.select = function (name, value, cell) {
    if (!A.live) {
      A.status('These are the last run\'s results: press Run all, then select.');
      A.render();
      return;
    }
    A.selected[name] = value;
    A.setVariable(name, value, cell);
  };

  A.save = function () {
    return A.api('POST', '/api/notebook', { text: A.text(), save: true }).then(function (r) {
      A.merge(r.cells);
      A.lastText = A.text();
      A.dirty = false;
      A.status(r.note || 'Saved');
      A.render();
    }).catch(function (e) { A.status('Save failed: ' + e.message); });
  };

  // Sets (or with null, removes) a key=value option on a cell's marker line, keeping the rest of the
  // line as written, so a click in the UI is an ordinary edit of the file.
  A.setOption = function (cell, key, value) {
    var tokens = (cell.head || '').match(/(?:[^\s"]+|"[^"]*")+/g) || [];
    tokens = tokens.filter(function (t) { return t.split('=')[0] !== key; });
    if (value != null && value !== '') tokens.push(key + '=' + (/\s/.test(value) ? '"' + value + '"' : value));
    cell.head = tokens.join(' ');
    cell.options = Object.assign({}, cell.options);
    if (value == null || value === '') delete cell.options[key]; else cell.options[key] = String(value);
    unsaved(300);
  };

  // hide= as two switches.
  A.hidden = function (cell) {
    var h = (cell.options && cell.options.hide) || '';
    return { code: NB.hides(h, 'code'), output: NB.hides(h, 'output') };
  };
  A.setHidden = function (cell, code, output) {
    A.setOption(cell, 'hide', code && output ? 'all' : code ? 'code' : output ? 'output' : null);
    A.render();
  };

  // ---- Rendering ----------------------------------------------------------------------------
  function badge(state) {
    state = state || 'stale';
    return NB.el('span', { 'class': 'nb-badge ' + state, title: state }, [NB.el('i'), state]);
  }

  function addBar(at) {
    var add = function (head, body) {
      return function () {
        A.cells.splice(at, 0, { head: head, body: body, kind: 'sql', state: 'stale', options: {} });
        A.edited(A.cells[at]);
        A.render();
        var ta = document.querySelectorAll('.nb-cell textarea')[at];
        if (ta) ta.focus();
      };
    };
    return NB.el('div', { 'class': 'nb-add' }, [
      NB.el('button', { text: '+ SQL', onclick: add('', '') }),
      NB.el('button', { text: '+ Markdown', onclick: add('[markdown]', '/*\n\n*/') }),
      NB.el('button', { text: '+ Input', onclick: add('input value number', 'SET VARIABLE value = 0;') })
    ]);
  }

  // A text area over part of a cell. The default edits the body; a query cell's SQL and its chart
  // specs pass their own get/set, and run with their own action.
  // Names of query cells, coloured where SQL refers to them.
  A.cellNames = function () {
    var names = {};
    A.cells.forEach(function (c) { if (c.kind === 'sql' && c.name) names[c.name] = true; });
    return names;
  };

  function codeArea(cell, part) {
    part = part || {
      get: function () { return cell.kind === 'sql' ? cell.sql : cell.body; },
      set: function (v) { if (cell.kind === 'sql') { cell.sql = v; A.compose(cell); } else cell.body = v; A.edited(cell); },
      run: function () { A.runCell(cell); },
      sql: cell.kind === 'sql' || cell.kind === 'input'
    };
    var text = part.get() || '';
    var ta = NB.el('textarea', { 'class': 'nb-code' + (part.className ? ' ' + part.className : ''), spellcheck: 'false', rows: Math.min(24, Math.max(2, text.split('\n').length + 1)) });
    ta.value = text;
    ta.addEventListener('input', function () { part.set(ta.value); });
    ta.addEventListener('keydown', function (e) {
      if ((e.shiftKey || e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); part.run(); }
      else if (e.key === 'Tab' && !e.shiftKey) {
        e.preventDefault();
        var s0 = ta.selectionStart;
        ta.setRangeText('    ', s0, ta.selectionEnd, 'end');
        ta.dispatchEvent(new Event('input'));
      } else if (e.ctrlKey && e.key === ' ') { e.preventDefault(); AppExtra.complete(ta, cell); }
    });
    if (!part.sql) return ta;
    // Highlighting: the coloured text is a <pre> behind a transparent textarea with the same metrics.
    // The textarea grows to fit its text, so neither ever scrolls and the two stay aligned.
    var hl = NB.el('pre', { 'class': 'nb-code-hl', 'aria-hidden': 'true' });
    ta.classList.add('nb-code-over');
    var fit = function () {
      if (!ta.offsetParent) return;  // hidden (hide=code): sized when shown
      ta.style.height = 'auto';
      ta.style.height = ta.scrollHeight + 'px';
    };
    var paint = function () { hl.innerHTML = NB.highlightSql(ta.value, { names: A.cellNames() }) + '\n'; fit(); };
    ta.addEventListener('input', paint);
    paint();
    requestAnimationFrame(fit);
    return NB.el('div', { 'class': 'nb-code-wrap' }, [hl, ta]);
  }

  // A chart spec of a query cell was edited: only that cell's charts need drawing again.
  A.specEdited = function (cell) {
    A.compose(cell);
    unsaved(500);
  };

  A.addChart = function (cell, spec) {
    cell.specs = (cell.specs || []).concat([spec]);
    cell.tab = cell.specs.length - 1;
    cell.editingChart = false;
    cell.picking = false;
    A.specEdited(cell);
    clearTimeout(analyseTimer);
    return A.analyse().then(function () { return A.redraw(cell); });
  };

  A.removeChart = function (cell, k) {
    cell.specs.splice(k, 1);
    if (cell.result && cell.result.charts) cell.result.charts.splice(k, 1);
    cell.tab = Math.min(cell.tab || 0, cell.specs.length);
    A.specEdited(cell);
    A.render();
  };

  // timing: false in the [notebook] cell hides how long cells took. It is a setting of the notebook,
  // so it is saved with it; the gear menu's switch edits that line.
  A.notebookCell = function () {
    return A.cells.find(function (c) { return c.kind === 'notebook'; });
  };
  // cache: false in the [notebook] cell keeps no results for this notebook (FORMAT §8).
  A.flag = function (key) {
    var nb = A.notebookCell(), m = nb && new RegExp('^' + key + ':\\s*(\\S+)\\s*$', 'm').exec(nb.content || '');
    return !(m && m[1] === 'false');
  };
  A.timing = function () { return A.flag('timing'); };
  A.caching = function () { return A.flag('cache'); };
  A.setTiming = function (on) { A.setFlag('timing', on); };
  A.setCaching = function (on) {
    A.setFlag('cache', on);
    // Switching it off forgets what was kept: the server does so when it reads the new text.
    if (!on) A.cells.forEach(function (c) { if (c.kept) dropKept(c); });
    A.status(on ? 'Results will be kept from the next run on' : 'Results are no longer kept for this notebook');
    A.render();
  };
  A.setFlag = function (key, on) {
    var nb = A.notebookCell();
    if (!nb) {
      if (on) return;
      nb = { head: '[notebook]', body: '/*\n*/', kind: 'notebook', state: 'stale', options: {}, content: '' };
      A.cells.unshift(nb);
    }
    var lines = (nb.content || '').split('\n').filter(function (l) { return l.indexOf(key + ':') !== 0; });
    while (lines.length && !lines[lines.length - 1].trim()) lines.pop();
    if (!on) lines.push(key + ': false');
    nb.content = lines.join('\n');
    nb.body = '/*\n' + nb.content + (nb.content ? '\n' : '') + '*/';
    A.edited(nb);
    A.render();
  };
  function elapsed(ms) {
    return ms < 1000 ? Math.max(0, Math.round(ms)) + ' ms' : ms < 60000 ? (ms / 1000).toFixed(1) + ' s' : Math.floor(ms / 60000) + ' min ' + Math.round(ms % 60000 / 1000) + ' s';
  }

  // A symbol for each kind of cell; the name is in its tooltip.
  var KINDS = {
    notebook: ['☰', 'Notebook settings'], markdown: ['¶', 'Text'], input: ['◉', 'Input'], chart: ['∿', 'Chart'],
    query: ['▦', 'Query: its result is a table, with charts as tabs'], sql: ['✎', 'SQL that changes the database']
  };
  function kindMark(cell) {
    var k = KINDS[NB.kindName(cell)] || ['•', cell.kind];
    return NB.el('span', { 'class': 'nb-kind', text: k[0], title: k[1], 'aria-label': k[1] });
  }

  function messages(cell) {
    var box = NB.el('div', { 'class': 'nb-meta' });
    (cell.errors || []).forEach(function (m) { box.appendChild(NB.el('div', { 'class': 'nb-error', text: m })); });
    (cell.warnings || []).forEach(function (m) { box.appendChild(NB.el('div', { 'class': 'nb-warning', text: '⚠ ' + m })); });
    return box;
  }

  // The bottom right corner of a cell: rows, what it defines, and how long it took.
  function footer(cell) {
    var r = cell.result, parts = [];
    if (!r || r.status === 'skipped') return null;
    if (A.outdated(cell)) {
      parts.push(NB.el('span', { 'class': 'nb-outdated', text: 'from an earlier run',
        title: 'This result is from an earlier run' + (cell.kept && cell.kept !== true ? ' (' + NB.formatValue(cell.kept) + ' UTC)' : '') + '. The cell, or something it reads, has changed since: run it to bring it up to date.' }));
    }
    if (A.reading) return parts.length ? NB.el('div', { 'class': 'nb-foot' }, parts) : null;
    if (r.row_count != null) parts.push(NB.el('span', { text: NB.formatNumber(r.row_count, ',') + (r.row_count === 1 ? ' row' : ' rows') }));
    if (cell.defines && cell.defines.length) parts.push(NB.el('span', { text: 'defines ' + cell.defines.join(', ') }));
    if (r.elapsed_ms != null && A.timing()) parts.push(NB.el('span', { 'class': 'nb-time', text: elapsed(r.elapsed_ms), title: 'Time this cell took to run' }));
    if (!parts.length) return null;
    var foot = NB.el('div', { 'class': 'nb-foot' });
    parts.forEach(function (p, k) { if (k) foot.appendChild(document.createTextNode(' · ')); foot.appendChild(p); });
    return foot;
  }

  // A chart without width= on its cell takes the width of a wide page (NB.width).
  A.fluid = function (cell) { return !(cell.options && cell.options.width); };

  function output(cell) {
    var out = NB.el('div', { 'class': 'nb-output' }), r = cell.result || {};
    // A query whose chart failed still shows its result; the chart's tab shows the error.
    if (r.status === 'error' && !(cell.kind === 'sql' && r.data)) { out.appendChild(NB.el('div', { 'class': 'nb-error', text: r.error })); return out; }
    if (cell.kind === 'markdown') {
      // Before a run, {{ expressions }} show as code rather than as half-rendered markdown.
      var text = r.output != null ? r.output : (cell.content || '').replace(/\{\{([\s\S]*?)\}\}/g, function (_, e) { return '`' + e.trim() + '`'; });
      out.appendChild(NB.el('div', { 'class': 'nb-md', html: NB.markdown(text) }));
    } else if (cell.kind === 'chart' && r.data) {
      NB.chart(out, r.data, { selected: A.selected, fluid: A.fluid(cell), onSelect: function (name, value) { A.select(name, value, cell); } });
    } else if (cell.kind === 'sql') {
      queryOutput(cell, out);
    }
    return out;
  }

  // A query's output: its table, or tabs for its charts with the table last. Without charts, the
  // table and the chart picker that starts one.
  function queryOutput(cell, out) {
    var r = cell.result || {}, specs = cell.specs || [];
    var tab = specs.length ? Math.min(cell.tab || 0, specs.length) : specs.length;
    if (specs.length) {
      var items = NB.queryTabItems(r.charts, specs.length);
      out.appendChild(NB.tabs(items, tab, function (k) { cell.tab = k; A.render(); },
        [NB.el('button', { 'class': 'nb-tab nb-tab-add nb-hide-reading', text: '+ chart', title: 'Add a chart of this result', onclick: function () { cell.tab = specs.length; cell.picking = true; A.render(); } })]));
    }
    if (tab < specs.length) {
      var holder = NB.el('div', { 'class': 'nb-tab-body' });
      out.appendChild(holder);
      var chart = (r.charts || [])[tab];
      if (chart) NB.chart(holder, chart, { selected: A.selected, fluid: A.fluid(cell), seed: 'cell' + cell.idx + '/' + tab, inTab: true, onSelect: function (name, value) { A.select(name, value, cell); } });
      else holder.appendChild(NB.el('div', { 'class': 'nb-stub', text: 'Not drawn yet: run the cell (Shift+Enter).' }));
      if (!A.reading) {
        out.appendChild(stub((cell.editingChart ? '▾ ' : '▸ edit ') + 'chart spec', function () { cell.editingChart = !cell.editingChart; A.render(); }));
        if (cell.editingChart) {
          var k = tab;
          out.appendChild(NB.el('div', { 'class': 'nb-code-area nb-spec-area' }, [
            codeArea(cell, { className: 'nb-spec', get: function () { return specs[k]; }, set: function (v) { specs[k] = v; A.specEdited(cell); }, run: function () { A.redraw(cell); } }),
            NB.el('div', { 'class': 'nb-spec-actions' }, [
              NB.el('button', { text: '▶ Draw', title: 'Draw this chart again (Shift+Enter)', onclick: function () { A.redraw(cell); } }),
              NB.el('button', { text: 'Remove chart', onclick: function () { A.removeChart(cell, k); } })
            ])
          ]));
        }
      }
      return;
    }
    if (r.data) {
      var size = +((cell.options && cell.options.rows) || A.pageSize);
      var fetchPage = function (offset, limit) {
        // A remembered result holds one page; the rest is in a session that has not run yet.
        if (!A.live) { A.status('This is the first page of the last run: press Run all to see the rest.'); return Promise.resolve(); }
        return A.api('GET', '/api/page?cell=' + cell.idx + '&offset=' + offset + '&limit=' + limit).then(function (data) {
          cell.offset = offset; data.total = r.data.total; r.data = data; A.render();
        }).catch(function (e) { A.status(e.message); });
      };
      var grid = NB.el('div', { 'class': 'nb-tab-body' });
      out.appendChild(grid);
      NB.table(grid, r.data, {
        offset: cell.offset || 0, pageSize: size,
        onPage: fetchPage,
        // The page size is part of the notebook: it is written to the cell as rows=N.
        onPageSize: function (next) { A.setOption(cell, 'rows', next); fetchPage(0, next); }
      });
      // The chart picker stays out of the way until asked for.
      if (cell.picking) out.appendChild(AppExtra.switcher(cell, r.data.columns));
      else if (!specs.length) {
        out.appendChild(NB.el('button', { 'class': 'nb-tab nb-tab-add nb-pick-chart nb-hide-reading', text: '+ chart', title: 'Chart this result',
          onclick: function () { cell.picking = true; A.render(); } }));
      }
    }
  }

  function inputCell(cell) {
    var value = cell.result && cell.result.value !== undefined ? cell.result.value : cell.value;
    var view = Object.assign({}, cell, { value: value, choices: cell.choices });
    var box = NB.el('div');
    var widget = NB.inputWidget(view, function (c, v) { cell.value = v; if (cell.result) cell.result.value = v; A.setVariable(cell.name, v, cell); });
    box.appendChild(widget);
    if ((cell.widget === 'select' || cell.widget === 'multiselect') && !cell.choices && cell.name && !cell.fetching) {
      // Choices from a cell's column exist once that cell has run; /api/run refreshes them.
      cell.fetching = true;
      A.api('GET', '/api/options?cell=' + cell.idx).then(function (r) { if (r.values.length) { cell.choices = r.values; A.render(); } }).catch(function () {});
    }
    return box;
  }

  function headButtons(cell, i) {
    var hidden = A.hidden(cell), kids = [];
    kids.push(NB.el('button', { 'class': 'nb-run', text: '▶', title: 'Run (Shift+Enter)', onclick: function () { A.runCell(cell); } }));
    if (cell.kind !== 'notebook') {
      kids.push(NB.el('button', { 'class': 'nb-toggle nb-hide-code' + (hidden.code ? ' on' : ''), text: '</>', title: hidden.code ? 'Code is hidden (hide=code): show it' : 'Hide code in reading view and exports',
        onclick: function () { A.setHidden(cell, !hidden.code, hidden.output); } }));
      kids.push(NB.el('button', { 'class': 'nb-toggle nb-hide-output' + (hidden.output ? ' on' : ''), text: '◫', title: hidden.output ? 'Output is hidden (hide=output): show it' : 'Hide output in reading view and exports',
        onclick: function () { A.setHidden(cell, hidden.code, !hidden.output); } }));
    }
    kids.push(NB.el('button', { text: '↑', title: 'Move up', disabled: i === 0 ? 'disabled' : null, onclick: function () { A.cells.splice(i - 1, 0, A.cells.splice(i, 1)[0]); A.edited(cell); A.render(); } }));
    kids.push(NB.el('button', { text: '↓', title: 'Move down', disabled: i === A.cells.length - 1 ? 'disabled' : null, onclick: function () { A.cells.splice(i + 1, 0, A.cells.splice(i, 1)[0]); A.edited(cell); A.render(); } }));
    kids.push(NB.el('button', { text: '✕', title: 'Delete cell', onclick: function () { if (confirm('Delete this cell?')) { A.cells.splice(i, 1); A.dirty = true; A.analyse(); A.render(); } } }));
    return kids;
  }

  // A hidden part of a cell collapses to a line that shows it while editing, without changing hide=.
  function stub(text, onclick) {
    return NB.el('div', { 'class': 'nb-stub nb-hide-reading', text: text, onclick: onclick });
  }

  // The browser recomputes :hover only when the mouse moves, so after a redraw (a button click, a run)
  // the cell under a still pointer would lose its buttons. Remember where the pointer is and mark that
  // cell until the pointer leaves it.
  // The class goes on while the cell is built, so nothing fades out and back in.
  var pointer = null;
  document.addEventListener('mousemove', function (e) { pointer = [e.clientX, e.clientY]; }, { passive: true });
  function pointedIndex() {
    var under = pointer && document.elementFromPoint(pointer[0], pointer[1]), cell = under && under.closest && under.closest('.nb-cell');
    return cell ? +cell.getAttribute('data-idx') : -1;
  }

  // ---- Presenting (render-present.js) ---------------------------------------------------------
  // What the reading view shows of a cell, as presenting needs to know it before the cell is built:
  // prose, inputs, charts and query results, unless hidden. hide=all leaves a cell without a screen.
  A.presentable = function (cell) {
    var hidden = A.hidden(cell), r = cell.result;
    if (cell.kind === 'notebook' || (hidden.code && hidden.output)) return false;
    if (cell.kind === 'input') return true;
    if (hidden.output) return false;
    if (cell.kind === 'markdown') return true;
    return !!(r && (r.data || r.status === 'error'));
  };
  var wasReading = false;
  A.present = {
    render: function () { A.render(); },
    // Presenting is the reading view, a screen at a time. Asked for by the reader, a session that has not
    // run yet runs first: cached results (FORMAT §8) show at once, but inputs and brushing need a live
    // session. A page opened at #present runs nothing until the reader asks (FORMAT §8).
    enter: function (asked) {
      wasReading = A.reading;
      A.reading = true;
      A.menu(false);
      if (asked && !A.running && !A.live) A.run(null);
    },
    leave: function () { A.reading = wasReading; }
  };

  A.render = function () {
    var root = $('cells'), pointed = pointedIndex();
    // Rebuilding the cells empties the page for a moment; keep the reader where they were.
    var main = $('main'), scrollTop = main.scrollTop;
    var focused = document.activeElement, focusIndex = -1, caret = 0;
    if (focused && focused.tagName === 'TEXTAREA' && focused.closest('.nb-cell')) {
      focusIndex = Array.prototype.indexOf.call(root.querySelectorAll('textarea'), focused);
      caret = focused.selectionStart;
    }
    root.innerHTML = '';
    document.getElementById('app').classList.toggle('reading', A.reading);
    NB.width.declare(NB.width.declared(A.cells));
    var screen = null;
    if (Present.on) {
      screen = Present.begin(A.cells.map(function (c, i) {
        return { id: i, span: c.options && c.options.span, kind: c.kind, screen: c.options && c.options.screen, visible: A.presentable(c) };
      }), root, main);
    } else Present.reset(root);
    A.cells.forEach(function (cell, i) {
      if (screen && !screen[i]) return;
      if (!A.reading) root.appendChild(addBar(i));
      var hidden = A.hidden(cell);
      var hideCode = hidden.code && !(cell.peekCode && !A.reading);
      var hideOutput = hidden.output && !(cell.peekOutput && !A.reading);
      // Reading view shows results, not code; markdown and inputs have no code to show there.
      if (A.reading) hideCode = true;
      var box = NB.el('div', { 'class': 'nb-cell' + (hideCode ? ' hide-code' : '') + (hideOutput ? ' hide-output' : '') + (A.outdated(cell) ? ' outdated' : ''), 'data-kind': cell.kind, 'data-idx': i, 'data-seed': 'cell' + i + (cell.name || '') });
      if (i === pointed) {
        box.classList.add('pointed');
        box.addEventListener('mouseleave', function () { box.classList.remove('pointed'); }, { once: true });
      }
      var span = cell.options && cell.options.span;
      if (span && A.reading) box.style.gridColumn = 'span ' + NB.span(span);
      // Left margin: the cell's kind and name. Clicking either opens its -- %% line (name and options)
      // for editing. Right margin: its state. Its buttons show while the pointer or caret is in it.
      var head = NB.el('input', { 'class': 'nb-head', value: cell.head || '', placeholder: 'name and options', title: 'The -- %% marker line' });
      head.addEventListener('change', function () { cell.head = head.value.trim(); A.edited(cell); });
      head.addEventListener('keydown', function (e) {
        if (e.key === 'Enter') head.blur();
        else if (e.key === 'Escape') { head.value = cell.head || ''; head.blur(); }
      });
      // After the blur, so a click elsewhere has already moved the focus that render() restores.
      head.addEventListener('blur', function () { setTimeout(function () { if (cell.editingHead) { cell.editingHead = false; A.render(); } }, 0); });
      var editHead = function () {
        cell.editingHead = true;
        A.render();
        var input = document.querySelector('.nb-cell[data-idx="' + i + '"] input.nb-head');
        if (input) { input.focus({ preventScroll: true }); input.select(); }
      };
      var kind = kindMark(cell);
      kind.addEventListener('click', editHead);
      box.appendChild(NB.el('div', { 'class': 'nb-margin' + (cell.editingHead ? ' editing' : '') }, [
        kind,
        NB.el('span', { 'class': 'nb-name', text: cell.name || '', title: (cell.head || '') + ' (click to edit the name and options)', onclick: editHead }),
        head
      ]));
      box.appendChild(NB.el('div', { 'class': 'nb-cell-head' }, [badge(cell.state)]));
      // The buttons sit on the cell's top edge, so a one-line cell is as easy to use as a tall one.
      box.appendChild(NB.el('div', { 'class': 'nb-tools' }, headButtons(cell, i)));
      var body = NB.el('div', { 'class': 'nb-cell-body' });
      if (cell.kind === 'input') body.appendChild(inputCell(cell));
      // Prose, charts, inputs and metadata show their result; their source opens on demand.
      var folded = cell.kind !== 'sql' && !cell.editing && !(cell.errors && cell.errors.length);
      var codeWrap = NB.el('div', { 'class': 'nb-code-area' }, [codeArea(cell)]);
      if (hidden.code && !A.reading) {
        body.appendChild(stub(cell.peekCode ? '▾ code (hidden when reading)' : '▸ code hidden', function () { cell.peekCode = !cell.peekCode; A.render(); }));
      } else if (cell.kind !== 'sql' && !A.reading) {
        var what = cell.kind === 'markdown' ? 'text' : cell.kind === 'chart' ? 'chart spec' : cell.kind === 'input' ? 'default' : 'settings';
        body.appendChild(stub((folded ? '▸ edit ' : '▾ ') + what, function () { cell.editing = !cell.editing; A.render(); }));
      }
      if (folded && !hidden.code) codeWrap.style.display = 'none';
      body.appendChild(codeWrap);
      body.appendChild(messages(cell));
      if (hidden.output && !A.reading) {
        body.appendChild(stub(cell.peekOutput ? '▾ output (hidden when reading)' : '▸ output hidden', function () { cell.peekOutput = !cell.peekOutput; A.render(); }));
      }
      var out;
      try { out = output(cell); } catch (e) {
        // A remembered result that cannot be drawn is dropped, never an error.
        if (!cell.kept) throw e;
        dropKept(cell);
        box.classList.remove('outdated');
        out = output(cell);
      }
      body.appendChild(out);
      var foot = footer(cell);
      if (foot) body.appendChild(foot);
      box.appendChild(body);
      root.appendChild(box);
      if (cell.kind === 'markdown') box.addEventListener('dblclick', function (e) { if (!cell.editing && !A.reading && e.target.closest('.nb-output')) { cell.editing = true; A.render(); } });
      box.addEventListener('focusin', function () { box.classList.add('focused'); });
      box.addEventListener('focusout', function () { box.classList.remove('focused'); });
      NB.width.fit(box);
      Sketch.outline(box, { width: 1.5 });
      if (window.Ink) write(box, cell);
    });
    if (!A.reading) root.appendChild(addBar(A.cells.length));
    if (focusIndex >= 0) {
      var t = root.querySelectorAll('textarea')[focusIndex];
      if (t) { t.focus({ preventScroll: true }); t.selectionStart = t.selectionEnd = caret; }
    }
    AppExtra.graph();
    main.scrollTop = scrollTop;
    if (Present.on) Present.finish(main, root, A.running ? 'Running…' : 'Nothing to show yet: leave (Esc) and run the notebook.');
    var timing = $('show-timing');
    if (timing) timing.checked = A.timing();
    if ($('keep-results')) $('keep-results').checked = A.caching();
    if ($('wide')) $('wide').checked = NB.width.mode() === 'wide';
  };

  // Hands each piece of a cell to the pen (render-ink.js): its bracket, margin, state, and each part
  // of its body. A piece already written with the same content is left alone, so a render does not
  // replay anything; a new result is written. Code is keyed to the notebook's generation, not its text.
  var cellIds = new WeakMap(), nextCellId = 0;
  function write(box, cell) {
    // Keyed by the cell itself, not its position: moving or inserting a cell does not rewrite the others.
    if (!cellIds.has(cell)) cellIds.set(cell, ++nextCellId);
    // A presentation writes its screens afresh, whatever the editor has already written.
    var id = Present.prefix() + 'cell' + cellIds.get(cell) + '/';
    Array.prototype.forEach.call(box.children, function (part) {
      var c = part.getAttribute('class') || '';
      if (c.indexOf('sk-outline') >= 0) Ink.present(part, id + 'outline', cell.kind + (cell.name || ''), Ink.bracket(box));
      else if (c === 'nb-margin' || c === 'nb-cell-head') Ink.present(part, id + c);
      else if (c === 'nb-cell-body') {
        Array.prototype.forEach.call(part.children, function (bit, k) {
          var b = bit.getAttribute('class') || '';
          if (b === 'nb-code-area') Ink.present(bit, id + 'code', A.generation + '/' + cell.kind + (A.hidden(cell).code ? '/hidden' : ''));
          else Ink.present(bit, id + k + b);
        });
      }
    });
  }

  // Input widgets, shared with the exported viewer's look.
  // ---- File sync ----------------------------------------------------------------------------
  A.watch = function () {
    if (!window.EventSource) return;
    var es = new EventSource('/api/events?' + A.query() + '&token=' + encodeURIComponent(A.token));
    es.addEventListener('changed', function (e) {
      var data = JSON.parse(e.data);
      if (data.text === A.text() || data.text === A.lastText) return;
      var apply = function () { $('banner').innerHTML = ''; A.api('POST', '/api/notebook', { text: data.text, reload: true }).then(function (r) { A.load(r); }); };
      if (!A.dirty) { apply(); A.status('Reloaded: the file changed on disk'); return; }
      $('banner').innerHTML = '';
      $('banner').appendChild(NB.el('div', { 'class': 'nb-banner' }, [
        NB.el('span', { text: 'The file changed on disk, and you have unsaved edits.' }),
        NB.el('button', { text: 'Load the file', onclick: apply }),
        NB.el('button', { text: 'Keep mine', onclick: function () { A.lastText = data.text; $('banner').innerHTML = ''; } })
      ]));
    });
  };

  // ---- Export and menu ----------------------------------------------------------------------
  A.exportUrl = function () {
    var f = $('export-format').value, code = $('export-code').checked, out = $('export-output').checked;
    var hide = !code && !out ? 'all' : !code ? 'code' : !out ? 'output' : 'none';
    var format = f === 'html-precomputed' ? 'html&interactive=precomputed' : f;
    return '/api/export?' + A.query() + '&token=' + encodeURIComponent(A.token) + '&format=' + format + '&hide=' + hide;
  };

  A.menu = function (open) {
    var menu = $('menu'), gear = $('gear');
    open = open == null ? !menu.classList.contains('open') : open;
    menu.classList.toggle('open', open);
    gear.classList.toggle('open', open);
    gear.setAttribute('aria-expanded', open ? 'true' : 'false');
    if (open) Sketch.outline(menu, { fill: '#fdfcf8', seed: 'menu' });
  };

  // ---- Start --------------------------------------------------------------------------------
  A.start = function () {
    $('run-all').onclick = function () { if (window.Ink) Ink.fresh(); A.run(null); };
    $('interrupt').onclick = function () { A.api('POST', '/api/interrupt', {}); };
    $('gear').onclick = function (e) { e.stopPropagation(); A.menu(); };
    $('menu').onclick = function (e) { e.stopPropagation(); };
    document.addEventListener('click', function () { A.menu(false); });
    document.addEventListener('keydown', function (e) { if (e.key === 'Escape') A.menu(false); });
    $('autorun').onchange = function (e) { A.autorun = e.target.checked; };
    $('save').onclick = A.save;
    $('view-reading').onclick = function () { A.reading = !A.reading; $('view-reading').classList.toggle('primary', A.reading); A.render(); };
    $('view-present').onclick = function () { Present.enter(0); };
    $('view-graph').onclick = function () { A.graph = !A.graph; $('view-graph').classList.toggle('primary', A.graph); A.render(); };
    $('view-sidebar').onclick = function () { $('app').classList.toggle('with-sidebar'); $('view-sidebar').classList.toggle('primary'); };
    var theme = localStorage.getItem('nb.theme');
    if (theme) document.documentElement.setAttribute('data-theme', theme);
    $('theme').onclick = function () {
      var next = document.documentElement.getAttribute('data-theme') === 'dark' ? 'light' : 'dark';
      document.documentElement.setAttribute('data-theme', next);
      localStorage.setItem('nb.theme', next);
    };
    $('show-timing').onchange = function (e) { A.setTiming(e.target.checked); };
    $('wide').onchange = function (e) { NB.width.set(e.target.checked ? 'wide' : 'normal'); };
    $('keep-results').onchange = function (e) { A.setCaching(e.target.checked); };
    $('forget-results').onclick = function () { A.forget(); };
    NB.pageSizes.forEach(function (n) { $('page-size').appendChild(NB.el('option', { value: n, text: n, selected: n === A.pageSize ? 'selected' : null })); });
    $('page-size').onchange = function (e) {
      A.pageSize = +e.target.value;
      localStorage.setItem('nb.pageSize', A.pageSize);
      A.status('Tables without rows= now show ' + A.pageSize + ' rows; run again to refresh them');
    };
    $('export').onclick = function () {
      var f = $('export-format').value;
      A.menu(false);
      if (f === 'pdf') {
        // Print the reading view, with the same code/output choices as the other formats.
        var code = $('export-code').checked, out = $('export-output').checked, wasReading = A.reading;
        A.reading = !code; A.render();
        document.body.classList.toggle('print-no-output', !out);
        setTimeout(function () { window.print(); A.reading = wasReading; A.render(); }, 150);
        return;
      }
      window.open(A.exportUrl(), '_blank');
    };
    document.addEventListener('keydown', function (e) {
      if ((e.ctrlKey || e.metaKey) && e.key === 's') { e.preventDefault(); A.save(); }
      if ((e.ctrlKey || e.metaKey) && e.shiftKey && e.key === 'Enter') { e.preventDefault(); if (window.Ink) Ink.fresh(); A.run(null); }
    });
    window.addEventListener('beforeunload', function (e) { if (A.dirty) { e.preventDefault(); e.returnValue = ''; } });
    document.fonts.ready.then(function () {
      Sketch.outline($('toolbar-rule'), { kind: 'underline', stroke: '#ced4da', width: 1.3, seed: 'rule' });
      if (window.Ink) Ink.present($('toolbar-rule').querySelector('svg.sk-outline'), 'toolbar/rule', 'rule');
    });
    if (window.Ink) {
      ['run-all', 'gear'].forEach(function (id) { Ink.present($(id), 'toolbar/' + id); });
      $('ink').value = Ink.mode();
      $('ink').onchange = function (e) { Ink.setSetting(e.target.value); };
      // Watching starts by writing the whole page again; leaving it (Esc) puts the menu back.
      Ink.onrewrite = function () { A.render(); };
      window.addEventListener('inkmode', function () { $('ink').value = Ink.mode(); });
    } else $('ink').disabled = true;
    A.api('GET', '/api/notebook').then(function (data) {
      A.load(data);
      NB.width.start({ render: function () { A.render(); } });
      Present.start(A.present);
      A.watch();
      AppExtra.catalog(); AppExtra.picker();
      var kept = (data.cached || []).length;
      A.status(!kept ? 'Opened. Nothing has run yet: press Run all.' : A.live ? 'Showing the last results.' : 'Showing the last results. Nothing has run yet: press Run all.');
    }).catch(function (e) { A.status(e.message); $('cells').appendChild(NB.el('div', { 'class': 'nb-error', text: e.message })); });
  };

  return A;
})();
