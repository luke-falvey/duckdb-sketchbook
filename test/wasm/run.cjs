// Loads the sketchbook extension's WebAssembly build into DuckDB-WASM in headless Chromium and runs
// notebook_cells, notebook_sql, notebook_chart_query and notebook_run on examples/revenue.sql.
// See test/wasm/README.md. The files are served by an HTTP server inside this process, on
// 127.0.0.1 and a free port, closed when the run ends. (Playwright's request routing cannot answer
// the threads bundle: it starts its pthread workers from inside a worker.)
//
//   node test/wasm/run.cjs [wasm_eh] [wasm_threads]
//
// Each architecture needs `make wasm_eh` / `make wasm_threads` first. With no argument, every
// architecture that has been built is tested.
'use strict';
const fs = require('fs');
const http = require('http');
const path = require('path');
const { chromium } = require('playwright');
const esbuild = require('esbuild');

const ROOT = path.resolve(__dirname, '..', '..');
const DIST = path.join(__dirname, 'node_modules', '@duckdb', 'duckdb-wasm', 'dist');

// DuckDB-WASM 2.0 has two bundles: base (platform wasm_eh) and threads (wasm_threads). There is no
// bundle for wasm_mvp any more, so a wasm_mvp extension has nothing to load into.
const ARCHS = {
  wasm_eh: { bundle: 'base' },
  wasm_threads: { bundle: 'threads' },
};

function extensionPath(arch) {
  return path.join(ROOT, 'build', arch, 'extension', 'sketchbook', 'sketchbook.duckdb_extension.wasm');
}

const LOAD_ATTEMPTS = 3;

const CONTENT_TYPES = { '.js': 'text/javascript', '.wasm': 'application/wasm', '.html': 'text/html', '.sql': 'text/plain' };

// A step that hangs fails the run instead of stalling it.
function withTimeout(promise, what, ms = 60000) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`timed out after ${ms} ms: ${what.slice(0, 100)}`)), ms);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

// Serves the page, DuckDB-WASM and each architecture's extension, with the headers that make the
// page cross-origin isolated (the threads bundle needs SharedArrayBuffer). 127.0.0.1 is a secure
// context, which isolation also needs.
function serve(pageScript) {
  const files = {
    '/index.html': Buffer.from('<!doctype html><meta charset="utf-8"><title>notebook wasm test</title><script src="page.js"></script>'),
    '/page.js': Buffer.from(pageScript),
  };
  for (const name of fs.readdirSync(DIST)) {
    if (/\.(wasm|worker\.js)$/.test(name)) files['/' + name] = path.join(DIST, name);
  }
  for (const arch of Object.keys(ARCHS)) files[`/${arch}/sketchbook.duckdb_extension.wasm`] = extensionPath(arch);
  const server = http.createServer((req, res) => {
    const pathname = new URL(req.url, 'http://localhost').pathname;
    const file = files[pathname];
    if (file === undefined || (typeof file === 'string' && !fs.existsSync(file))) {
      res.writeHead(404).end('not found');
      return;
    }
    res.writeHead(200, {
      'content-type': CONTENT_TYPES[path.extname(pathname)] || 'application/octet-stream',
      'cross-origin-opener-policy': 'same-origin',
      'cross-origin-embedder-policy': 'require-corp',
    });
    res.end(typeof file === 'string' ? fs.readFileSync(file) : file);
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

async function testArch(browser, arch, origin) {
  const failures = [];
  const check = (name, ok, detail) => {
    console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${name}${ok || detail === undefined ? '' : ': ' + JSON.stringify(detail)}`);
    if (!ok) failures.push(name);
  };
  // DuckDB-WASM's threads bundle sometimes hangs in LOAD (about one load in eight here, DuckDB's own
  // icu extension as well as this one; docs/DESIGN.md section 21). A hung LOAD is retried in a fresh
  // page for wasm_threads only, at most LOAD_ATTEMPTS times, and every retry is reported.
  const attempts = arch === 'wasm_threads' ? LOAD_ATTEMPTS : 1;
  let context;
  let page;
  let sql;
  let load;
  for (let attempt = 1; attempt <= attempts; attempt++) {
    context = await browser.newContext();
    page = await context.newPage();
    page.on('pageerror', (e) => console.log('  page error:', e.message));
    await page.goto(origin + '/index.html');
    const t0 = Date.now();
    await withTimeout(page.evaluate((bundle) => window.notebookTest.boot(bundle), ARCHS[arch].bundle), 'starting DuckDB-WASM');
    console.log(`  DuckDB-WASM started in ${Date.now() - t0} ms`);
    const p = page;
    sql = (text, ms) => withTimeout(p.evaluate((t) => window.notebookTest.sql(t), text), text, ms);
    const t1 = Date.now();
    try {
      load = await sql(`LOAD '${origin}/${arch}/sketchbook.duckdb_extension.wasm'`, 20000);
      console.log(`  extension loaded in ${Date.now() - t1} ms`);
      break;
    } catch (e) {
      load = { error: String(e.message || e) };
      console.log(`  LOAD hung (attempt ${attempt} of ${attempts}): ${load.error}`);
      await context.close();
    }
  }
  check('LOAD sketchbook', !load.error, load.error);
  if (load.error) {
    return failures;
  }
  check('page is cross-origin isolated', await page.evaluate(() => window.notebookTest.isolated()));

  const info = await sql("SELECT version() AS version, (SELECT platform FROM pragma_platform()) AS platform");
  const engine = info.rows && info.rows[0];
  check('engine is DuckDB v2.0.1', engine && engine.version === 'v2.0.1', info);
  check(`engine platform is ${arch}`, engine && engine.platform === arch, info);

  // What the html-wasm export works around on DuckDB-WASM 1.5 (docs/DESIGN.md section 12).
  const shorthand = await sql("SET VARIABLE nb_probe = 'x'; SELECT $nb_probe AS v");
  check('engine: $name shorthand works', shorthand.rows && shorthand.rows[0].v === 'x', shorthand);
  const untyped = await sql('SET VARIABLE nb_probe = NULL; SELECT $nb_probe.lo AS v');
  check('engine: .lo of an untyped NULL variable', untyped.rows && untyped.rows[0].v === null, untyped);


  await page.evaluate(
    ([name, text]) => window.notebookTest.registerText(name, text),
    ['revenue.sql', fs.readFileSync(path.join(ROOT, 'examples', 'revenue.sql'), 'utf8')],
  );

  const cells = await sql(
    'SELECT idx, kind, name, len(errors) AS errors, len(charts) AS charts, depends_on FROM notebook_cells(\'revenue.sql\') ORDER BY idx',
  );
  check('notebook_cells', cells.rows && cells.rows.length > 5 && cells.rows.every((r) => r.errors === 0), cells);
  const monthly = (cells.rows || []).find((r) => r.name === 'monthly');
  const filtered = (cells.rows || []).find((r) => r.name === 'filtered');
  check('notebook_cells: monthly has charts and depends on filtered', monthly && filtered && monthly.charts > 0 &&
    (monthly.depends_on || []).includes(filtered.idx), monthly);

  const script = await sql("SELECT sql FROM notebook_sql('revenue.sql')");
  check('notebook_sql', script.rows && /CREATE OR REPLACE TABLE orders/.test(script.rows[0].sql) &&
    /CREATE OR REPLACE TEMP VIEW monthly|monthly/.test(script.rows[0].sql), script.error);

  await sql("CREATE TABLE nb_chart_t AS SELECT range AS i, range % 3 AS g FROM range(100)");
  const chart = await sql("SELECT len(queries) AS n, queries[1] AS q, render FROM notebook_chart_query('mark: bar\nx: g\ny: {field: i, agg: sum}', 'nb_chart_t')");
  const compiled = chart.rows && chart.rows[0];
  check('notebook_chart_query compiles', compiled && compiled.n === 1 && /"mark":"bar"/.test(compiled.render), chart);
  if (compiled) {
    const data = await sql(compiled.q);
    check('compiled chart query runs', data.rows && data.rows.length === 3, data);
  }

  const peekSql = 'WITH a AS (SELECT 1 AS x) SELECT * FROM a';
  const peek = await sql(`SELECT kind, name, query FROM notebook_peek('${peekSql}', ${peekSql.indexOf('SELECT 1')})`);
  check('notebook_peek', peek.rows && peek.rows[0].kind === 'cte' && peek.rows[0].name === 'a' &&
    peek.rows[0].query === 'WITH a AS (SELECT 1 AS x) SELECT * FROM a', peek);

  const run = await sql(
    "SELECT idx, kind, name, status, error, row_count FROM notebook_run('revenue.sql', params := {region: 'US'}) ORDER BY idx",
  );
  check('notebook_run: every cell ok or skipped', run.rows && run.rows.length > 5 &&
    run.rows.every((r) => r.status === 'ok' || r.status === 'skipped'), run);
  check('notebook_run: tables it creates remain', !(await sql('SELECT count(*) AS n FROM orders')).error);

  // The result cache keeps entries in a directory beside the notebook. DuckDB-WASM's in-memory file
  // system has no directories, so there is no cache there: the run works and nothing is written.
  const cached = await sql("SELECT count(*) AS n FROM notebook_run('revenue.sql', cache := true) WHERE status = 'ok'");
  check('notebook_run(cache := true) runs', cached.rows && cached.rows[0].n > 0, cached);
  const entries = await sql("SELECT count(*) AS n FROM notebook_cache('revenue.sql')");
  check('notebook_cache: no entries in the in-memory file system', entries.rows && entries.rows[0].n === 0, entries);
  const leftovers = await sql("SELECT count(*) AS n FROM glob('.duckdb-sketchbook/**')");
  check('notebook_cache: nothing written', leftovers.rows && leftovers.rows[0].n === 0, leftovers);
  const cleared = await sql("SELECT * FROM notebook_cache_clear('revenue.sql')");
  check('notebook_cache_clear', !cleared.error, cleared);

  const md = await sql("SELECT bytes FROM notebook_export('revenue.sql', file := 'revenue.md', format := 'md')");
  check('notebook_export md', md.rows && md.rows[0].bytes > 0, md);
  if (md.rows) {
    const text = await page.evaluate(() => window.notebookTest.readFile('revenue.md'));
    check('notebook_export md: written to the virtual file system', /Revenue review/.test(text), text.slice(0, 200));
  }

  for (const [name, query] of [
    ['notebook_start', 'SELECT * FROM notebook_start()'],
    ['notebook_stop', 'SELECT * FROM notebook_stop()'],
    ['notebook_export pdf', "SELECT * FROM notebook_export('revenue.sql', file := 'r.pdf', format := 'pdf')"],
  ]) {
    const result = await sql(query);
    check(`${name} fails with a clear message`, result.error && /not available in this build|cannot do/.test(result.error), result);
  }

  await context.close();
  return failures;
}

(async () => {
  let archs = process.argv.slice(2);
  if (archs.length === 0) archs = Object.keys(ARCHS).filter((a) => fs.existsSync(extensionPath(a)));
  if (archs.length === 0) {
    console.error('No WebAssembly build found. Run `make wasm_eh` first (see test/wasm/README.md).');
    process.exit(1);
  }
  for (const arch of archs) {
    if (!ARCHS[arch]) {
      console.error(`Unknown architecture ${arch}; expected one of ${Object.keys(ARCHS).join(', ')}`);
      process.exit(1);
    }
    if (!fs.existsSync(extensionPath(arch))) {
      console.error(`${extensionPath(arch)} is missing; run \`make ${arch}\` first`);
      process.exit(1);
    }
  }
  const bundled = await esbuild.build({
    entryPoints: [path.join(__dirname, 'page.js')],
    bundle: true,
    format: 'iife',
    write: false,
    logLevel: 'warning',
  });
  const server = await serve(bundled.outputFiles[0].text);
  const origin = `http://127.0.0.1:${server.address().port}`;
  let browser = null;
  let failed = 0;
  try {
    browser = await chromium.launch();
    for (const arch of archs) {
      console.log(`${arch}:`);
      const failures = await testArch(browser, arch, origin);
      failed += failures.length;
    }
  } finally {
    if (browser) await browser.close();
    server.close();
  }
  console.log(failed ? `${failed} check(s) failed` : 'all checks passed');
  process.exit(failed ? 1 : 0);
})();
