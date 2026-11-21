# DuckDB Notebook: design

Status: every roadmap step (§9) has a first implementation: format parser, dependency graph,
headless runner, server and browser UI, chart compiler and renderers, in-database storage, and the
HTML, PDF, Markdown, SQL, data and DuckDB-WASM exports. §11 records what building it taught us and
where the implementation departs from the design below.

Companion documents: [FORMAT.md](FORMAT.md) specifies the file format; [RESEARCH.md](RESEARCH.md)
records the survey this design draws on and what could not be verified.

## Summary

A notebook is a plain `.sql` file. A DuckDB extension turns your own DuckDB process into the notebook
server and carries the whole user interface inside it, so it works offline. Cells are SQL, prose,
inputs and declarative charts; they form a dependency graph and re-run when something upstream
changes. The same text can be saved, with history, in a table inside any DuckDB database. A notebook
exports to one HTML file that opens anywhere without DuckDB, and to PDF.

```sql
INSTALL sketchbook FROM community;
LOAD sketchbook;
CALL notebook_start('reports/revenue.sql');   -- opens the browser
```

| Decision | Choice | Main reason |
|---|---|---|
| File format | `.sql` with `-- %%` cell markers | Valid SQL; diffs, tools and agents handle it natively |
| Runtime | DuckDB extension with embedded HTTP server | Runs in the user's process with their data, secrets and version |
| Frontend | Open source, compiled into the extension | Offline; nothing fetched at run time |
| Cell kinds | SQL, markdown, input, chart | One language to analyse, so reactivity and export stay simple |
| Execution | Reactive graph from parsed SQL | No hidden state; inputs and selections are just variables |
| Charts | Own small YAML spec, compiled to SQL by the extension | Diffable; big data stays in DuckDB; exports need no browser |
| In-database storage | Same text in a versions table | One format, lossless both ways |
| Export | Static single-file HTML by default; PDF via print | Shareable with no DuckDB and no server |

## 1. Goals and non-goals

**Goals**

1. Work entirely offline with every component open source.
2. SQL first: a person who knows DuckDB SQL can do everything, including charts and interactivity.
3. A notebook is a text file that reviews well in a pull request and that an AI agent can edit safely.
4. A notebook can also live inside the database it describes.
5. A wide range of chart types, usable on tables too large to send to a browser.
6. Share a notebook with someone who has neither DuckDB nor this extension.

**Non-goals for the first version**

- Python, JavaScript or R cells, and third-party widget plugins.
- Simultaneous multi-user editing.
- A hosted service, scheduling service or permissions model.

**What prompted these goals.** The official DuckDB UI is the closest existing tool, and its open issues
are the gap this project fills: most of its frontend is closed and fetched from `ui.duckdb.org` on each
load, it does not work offline, it has no charts, and notebooks cannot be exported or put in git.
Elsewhere, the notebooks people praise most (marimo, Livebook, Pluto, Evidence, Observable Framework)
share three traits: a plain-text format, reactive execution, and a static export.

## 2. The notebook file

[FORMAT.md](FORMAT.md) is the full specification. The essentials:

```sql
-- %% [markdown]
/*
# Revenue review
*/

-- %% input region select from=regions.name
SET VARIABLE region = 'EU';

-- %% monthly
SELECT date_trunc('month', ts) AS month, sum(amount) AS revenue
FROM orders
WHERE region = $region
GROUP BY ALL;
/* chart
mark: line
x: month
y: revenue
*/
```

- **Markers are comments.** `-- %%` starts a cell; prose and chart specs sit in block comments. DuckDB,
  sqlfluff and any editor see a normal SQL file.
- **A named query cell is a view.** `monthly` above can be queried by any later cell.
- **A query's output is a table until it is given charts.** Chart specs written after the query are
  views of its result, shown as tabs (§14).
- **Parameters are DuckDB variables.** An input cell is a real `SET VARIABLE` statement, and `$region`
  reads it. This needs no templating layer: `$name` is DuckDB 2.0's documented shorthand for
  `getvariable('name')`. Notebooks therefore need DuckDB 2.0 or later.
- **The cell name is its ID.** No generated identifiers appear in the file.
- **No outputs in the file.** They go to a disposable, git-ignored cache directory.

### Why this format

The survey compared five families of notebook format ([RESEARCH.md §2](RESEARCH.md#2-notebook-formats)).
A source file in the host language with comment delimiters (marimo `.py`, Pluto `.jl`, Databricks
`-- COMMAND`, Jupytext `# %%`) gives the best diffs and the best tool support, because the file is
still valid code. For a SQL notebook the host language is SQL.

The cost is that prose and chart specs are comments, so GitHub shows them as comment text rather than
rendered Markdown. A Markdown export covers the case where a rendered view in the repository matters.

### What "runnable" means

The file always parses in DuckDB. It runs start to finish in a plain client only when cells do not
refer to each other by name, because the view wrapper is added by the runner. That limit is deliberate:
keeping the bare `SELECT` in the file is what makes cells pleasant to read and edit. For portability
the extension writes an expanded script with the wrappers spelled out
([example](../examples/revenue.expanded.sql)), which runs in any DuckDB client.

## 3. Execution model

**Graph.** Each cell has a set of names it defines and a set it reads.

| Cell | Defines | Reads |
|---|---|---|
| Query cell | Its own name; its charts' selection variables | Tables, views and variables in its SQL; its charts' `filter` selections (no dependency) |
| Effect cell | Objects it creates or attaches; variables it sets | Same |
| Input | Its variable | The `from=` cell, if any |
| Chart | Its selection variable, if any | `from`, `filter` |
| Markdown | Nothing | Names inside `{{ }}` |

Reads come from DuckDB's parser, not from regular expressions. The extension walks the parsed
statement for table references that are not CTEs, for `$name` parameters and for `getvariable('…')`
calls. Names that match no cell are assumed to be catalog objects and are left alone.

**Rules**

- One definition per name. A second cell defining the same name is an error on both.
- SQL and input cells read only cells above them, so the file reads top to bottom. The one exception is
  a chart's selection: a query cell's chart filters are read but add no edge, so charts of one query can
  select and filter each other (§14).
- A cell is `fresh`, `stale`, `running` or `failed`. When a cell's source or anything it reads changes,
  it and everything downstream become stale.

**Query cells re-run automatically.** They are views, so "re-running" means re-reading the view for
the result grid and re-running the data queries of charts that depend on it. With auto-run off, cells
are only marked stale and wait for a click, which is how marimo's lazy mode and Livebook behave.

**Effect cells run only when asked.** Automatically replaying `INSERT`, `COPY … TO` or `ATTACH` would
be surprising and sometimes destructive. Editing or running an effect cell marks its dependents stale.
"Run all" executes every cell in run order, which is also what the headless runner does: file order,
except that a markdown or chart cell waits for the cells it reads (§11).

**Inputs and selections** set a variable in the session and trigger the same staleness propagation. To
the graph, a slider and a chart brush are the same thing.

**Sessions.** Each open notebook gets a session of its own. Temporary views and variables are
private to a connection (checked in 2.0.0), so two notebooks can both define `monthly` without
colliding, while both see the same attached databases. The server does not hold a connection
between requests (see §11): a session is its variables plus the statements that rebuild its
temporary views, replayed onto a fresh connection for each request.

**Long queries.** Results stream to the grid in pages. Any running cell can be interrupted. A
`materialize=table` option stores a slow cell's result as a temp table so dependents do not recompute
it.

**Trust.** Opening a notebook does not run it. SQL can read local files and reach the network through
`httpfs`, so a notebook from someone else stays inert, showing cached output if any, until the user
chooses to run it. That includes the `[notebook]` cell's `extensions:` and `attach:` and an input's
`from=` query: until the reader has run something in the session (`Session::trusted`, cleared again
when the text is reloaded from disk), the server's catalog lists the database as it is, a select
input offers only its `options=` list (`/api/options` answers `reason: "nothing has been run yet"`),
paging is refused, and setting an input gives it a type only where that needs no query. Peeks wait
too (§19).

A one-off run (`notebook_run`, an export) is bounded by the call it belongs to. The caller's interrupt is checked
before the replay, `Prepare` and every cell, and after the last. Its deadline (`max_execution_time`) bounds every
query of the runner's connection: a `ClientContextState` lowers each query's deadline to the caller's in
`QueryBegin`, which DuckDB calls after it has set the deadline from the setting. Writing the time left as a setting
does not work: every query would get it afresh, and a cell's own `SET max_execution_time = 0` would lift it. The
server's runs have no caller and no bound, and a cell already running is not interrupted (only its deadline ends it).

Functions that reach outside DuckDB respect its own switches: `notebook_start` (a listener on a
port, and a browser opened with the shell) refuses when `enable_external_access` is false or
`lock_configuration` is set, and so does `notebook_export(format := 'pdf')`, which starts a browser.

## 4. Runtime architecture

```
 DuckDB process (CLI, Python, any client)
 ┌─────────────────────────────────────────────────────────────┐
 │  sketchbook extension                                       │
 │   ├─ format parser, dependency graph, chart-spec compiler   │
 │   ├─ runner (a session replayed onto a connection per run)  │
 │   ├─ exporters (HTML, PDF, SQL, Markdown, data, WASM)       │
 │   └─ HTTP server on 127.0.0.1 ── serves embedded frontend   │
 └───────────────────────────┬─────────────────────────────────┘
                             │ HTTP + server-sent events, JSON results
                      Browser: editor, grids, charts
```

**Why an extension.** It runs inside the DuckDB the user already has, so the notebook sees their
attached databases, secrets, settings and extensions, with no second engine version and no file-lock
conflict. Installation is one `INSTALL` statement. Storing notebooks in the database is natural because
the code is already in the database process.

**What it costs.** The extension is C++ and must be rebuilt for each DuckDB release. To limit that,
all logic that does not need engine internals (the UI, chart rendering, export viewer) lives in the
JavaScript frontend, and the C++ side stays small: parser, graph, compiler, runner, server.

### SQL surface

Everything the UI does is also callable from SQL, so notebooks can be run and exported from a script
or CI job with no browser.

| Function | Purpose | State |
|---|---|---|
| `notebook_cells(path)` | One row per cell: kind, name, options, body, chart specs, variables read, `defines`, `reads`, `depends_on`, errors and warnings | Built |
| `notebook_sql(path)` | The notebook as a plain SQL script (the `sql` export) | Built |
| `notebook_run(path, params := {…})` | Run every cell in order in a session of its own; per-cell status, row count, timing and rendered markdown | Built |
| `notebook_start([path or name], port := …, open := true)` | Start the server, optionally opening a notebook | Built |
| `notebook_stop()` | Stop it | Built |
| `notebook_export(path, file := 'out.html', format := …, params := {…}, interactive := …, hide := …, page_rows := …, budget := …, data_format := …, chrome := …, wasm_runtime := …)` | Export (see §7) | Built |
| `notebook_save(path, name := …, message := …, database := …, force := …)` | Store a file in the current database as a new version | Built |
| `notebook_checkout(name, file := 'x.sql', version := …, database := …, force := …)` | Write a stored version to a file | Built |
| `notebook_chart_query(spec, from)` | The SQL a chart spec compiles to, for inspection | Built |

The output file parameter is `file :=` rather than `to :=`: `to` is a reserved word in DuckDB's
grammar and cannot name a parameter. Every function that takes a path also takes `db:<name>` or
`db:<name>@<version>` for a notebook stored in the database.

All of these are table functions. `notebook_run` executes in its own connection, so tables it creates
persist but its views and variables do not reach the caller. It runs chart cells too (compiling and
querying their data), so a broken chart spec fails a headless run. To run a notebook inside your own
connection, execute the script `notebook_sql` returns.

### HTTP server

- Binds to `127.0.0.1` only. A random token is created at start and required on every request. `Host`
  and `Origin` headers are checked as well, which blocks DNS-rebinding and cross-site requests. (The
  official UI relies on an `Origin` check alone.)
- Endpoints: load and save a notebook, parse to cells and graph, run and interrupt cells, set a
  variable, page through a result, input choices, catalog listing, export. A server-sent-events
  stream reports edits made to the file by another program.
- **Results are JSON** (S2, see §11): a result page or a chart's aggregated rows are small, and JSON
  needs no decoder in the browser.
- The server is cpp-httplib, which DuckDB already vendors. The frontend is compiled into the extension
  binary. Nothing is fetched from the network at run time.

### Frontend

A single-page app in plain JavaScript with no dependencies (`frontend/`). It looks like a sketchbook
drawn in Excalidraw (see Themes, §6): handwriting, paper, and hand-drawn boxes, lines and charts.

- A quiet toolbar: the notebook's title, its status, **Run all**, and a gear. Everything else (open,
  save, auto-run, reading and graph views, catalog, theme, default page size, export) is in the gear's
  menu.

- An editor per cell with Ctrl+Space completion from the live catalog, cell names and variables, and
  Shift+Enter to run. The design called for CodeMirror 6; bundling it needs a JavaScript build step in
  the extension build, which was left for later (§11).
- SQL highlighting without an editor library (`render-sql.js`): a tokenizer that follows
  `sql_lexer.cpp` (nested comments, `''` escapes, `$tag$` strings versus `$name` variables) colours a
  `<pre>` laid exactly behind a transparent textarea, which grows to fit its text so neither scrolls.
  Keywords come from the running DuckDB's `duckdb_keywords()` (reserved, column-name and type/function
  ones; unreserved keywords include common column names such as `day`, so only statement words are
  taken from them) and types from `duckdb_types()`. Query cell names are coloured where SQL reads
  them. Exports and ```` ```sql ```` fences in prose use the same highlighter (about 5 KB).
- Result grid, paged from the server, headed by plain column names. An earlier per-column profile
  (type, null share, histogram or top values) was removed to keep tables quiet; the catalog sidebar
  still lists column types. A page-size picker under each table writes `rows=N` to the cell, so the
  choice is saved with the notebook and applies to exports.
- Margins: a cell keeps only the left edge of its hand-drawn box, a bracket that darkens under the
  pointer and turns violet with the caret. Its kind symbol (☰ settings, ¶ text, ◉ input, ▦ query, ✎ SQL
  that changes the database, ∿ chart) and name sit in the left margin; clicking either opens its
  `-- %%` line to edit the name and options. Its state dot sits in the right margin. Its buttons (run,
  hide code, hide output, move, delete) form one row at its top right, in line with the state dot,
  while the pointer or caret is in the cell: a column of them in the margin was out of reach on
  one-line cells. The buttons are inside the cell, and a redraw keeps them for the cell under a still pointer
  (browsers recompute `:hover` only when the mouse moves), so clicking one does not hide the rest. Narrow
  windows put both margins in a row at the top of the cell. Chosen from mockups of four quieter
  treatments (faint frames, no frames, margins, frames only on hover).
- Quiet by default: the buttons that add a cell appear
  only when the pointer is between cells; a cell's rows, what it defines and its run time sit in the
  bottom right corner of its box (`timing: false` in the `[notebook]` cell hides the time); and a
  table's chart picker stays behind a "+ chart" button until asked for.
- Hover values: pointing at a mark shows its values in a paper note by the pointer, in the editor and
  in exports. On line and area charts the nearest point anywhere along the line answers, marked with a
  dot, even under a brush; a bar answers anywhere in its column (its row, when horizontal), which is
  highlighted. Hatched shapes carry an invisible hit area, so the gaps between hatch lines count.
  Bars on a continuous x (months on a date axis) are as wide as the spacing allows, and the axis is
  padded so the first and last are too. Re-rendering
  keeps the scroll position, so changing an input or brushing a chart does not move the page.
- Code and output switches in every cell header write `hide=` to the cell. While editing, a hidden
  part collapses to a line that reveals it; reading view and exports leave it out.
- Prose, chart, input and metadata cells show their result and keep their source behind an "edit"
  line (or a double-click on prose); SQL cells show their code.
- Catalog browser for attached databases, with click and drag to insert.
- A query's charts as tabs over its result, with the table last. A chart picker under the table (or
  "+ chart") writes a new `/* chart */` comment into the cell; "edit chart spec" opens a chart's spec,
  and Shift+Enter redraws it without running the query.
- Graph view of cells, showing stale and failed cells.
- Editing view (cells with code) and reading view (code hidden, grid layout via `span`).
- **Two-way file sync.** Saving writes the `.sql` file; an edit made in another editor or by an agent
  appears in the UI through the file watcher. The file is always the source of truth for a file-backed
  notebook.
- Live preview while typing is a later addition. MotherDuck's Instant SQL does this by parsing the
  query locally, caching the data it depends on and rewriting it to run fast, and lets the user click
  into any CTE to see its rows. Here a named cell is already a view, so a preview is a `LIMIT` over
  it and stepping through intermediate results is stepping through cells. Inside a cell, "peek under
  the pen" (§19) shows the rows of the CTE or sub-query under the caret.

## 5. Notebooks inside the database

A notebook can be saved into any attached, writable database. The schema is small:

```sql
CREATE SCHEMA IF NOT EXISTS _sketchbook;

CREATE TABLE _sketchbook.notebooks (
    name        VARCHAR PRIMARY KEY,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE _sketchbook.versions (
    name         VARCHAR NOT NULL REFERENCES _sketchbook.notebooks (name),
    version      INTEGER NOT NULL,
    content      VARCHAR NOT NULL,      -- the .sql file, byte for byte
    content_hash VARCHAR NOT NULL,      -- sha256(content)
    saved_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    message      VARCHAR,
    PRIMARY KEY (name, version)
);

CREATE VIEW _sketchbook.current AS
SELECT * FROM _sketchbook.versions
QUALIFY version = max(version) OVER (PARTITION BY name);
```

- **One format.** `content` is exactly the text of the file. Saving to the database and checking out
  to a file are lossless in both directions, so a notebook can move between git and a database freely.
  (Checked: a file read with `read_text`, stored, and read back is identical.)
- **History is rows.** Each save appends a version; a save whose hash equals the current version is
  skipped. Old versions can be listed, diffed and restored with plain SQL.
- **A database carries its own analysis.** Send someone `sales.duckdb` and they get the data and the
  notebooks that explain it. `CALL notebook_start('db:revenue')` opens a stored notebook.
- **Result cache.** For a database-backed notebook the cache goes in `_sketchbook.cache(name, cell,
  format, key, data, saved_at, result)` in place of the sidecar directory (FORMAT.md §8, and §17 below).
- **Databases from before the rename.** The extension was called `notebook` and its schema `_notebook`.
  A database with `_notebook.versions` and no `_sketchbook.versions` is read from the old schema, and the
  first save there copies the old history into `_sketchbook` so versions carry on; `_notebook` is left
  as it was.

**File and database together.** A notebook session has one source of truth: the place it was opened
from. `notebook_save` and `notebook_checkout` copy between the two explicitly. There is no automatic
two-way sync in the first version, because silent merges between a git checkout and a database row
are exactly where work gets lost. Both commands compare hashes and refuse to overwrite a newer copy
without `force := true`: `notebook_save` refuses text that equals an *older* stored version, and
`notebook_checkout` refuses to overwrite a file whose content matches no stored version. Saving from
the UI of a `db:` notebook appends a version.

**Alternatives considered.** DuckLake snapshots give time travel for free, but require a DuckLake
catalog and are heavy for a few kilobytes of text. The official UI keeps notebooks as JSON rows in a
private `ui.db`; using the user's own database and the plain-text format is what makes ours portable.

## 6. Visualisation

### Principle: the extension writes the query, the browser draws the result

A chart is a short YAML spec ([FORMAT.md §5](FORMAT.md#5-charts)), written at the end of a query cell
or as a chart cell. The extension compiles it into two things:

1. **A data query**: one aggregate SQL statement over the relation the chart draws. Grouping, binning,
   time truncation and downsampling all happen in DuckDB.
2. **A render description**: marks, scales and axes for the browser, referring to the query's columns.

The browser receives a few hundred or thousand rows whatever the size of the source table.

This split was chosen over running a query-generating library in the browser for three reasons:

- **Export without a browser.** The extension can compute every chart's data on its own, which is what
  lets `notebook_export` produce a complete HTML file from a script.
- **Small, private exports.** An exported page contains the aggregates behind each chart, not the
  underlying table.
- **One reactive model.** A selection sets a variable and dependents re-query, the same path as an
  input widget.

The techniques come from Mosaic (UW Interactive Data Lab), which demonstrated that pushing aggregation
into DuckDB keeps charts interactive on very large tables: M4 downsampling for lines (at most four
points per pixel column) and binning to the pixel grid for scatter and heat marks. Mosaic itself is
not the chosen dependency for the core path because it compiles queries in the browser and its API is
still changing between 0.x releases. It remains the likely basis for the fully interactive WASM export
(§7).

### Renderers

The spec is ours; the renderers behind it are replaceable. The design planned on libraries:

| Chart family | Planned library | Built |
|---|---|---|
| Marks on axes, facets, layers | Observable Plot | Own SVG (`frontend/render-chart.js`) |
| Sankey, chord, treemap, sunburst, icicle | D3 layouts | Own SVG layouts (`frontend/render-typed.js`) |
| Funnel, candlestick, gauge, calendar | Apache ECharts | Own SVG |
| Maps | MapLibre GL + deck.gl | Own SVG, equirectangular, no basemap |
| Networks | cosmos.gl | Own SVG with a small force layout (up to a few thousand nodes) |
| Tables, pivots | Virtualised grid | Paged grid; pivot is a DuckDB `PIVOT` query |
| KPI tiles, sparklines, gauge | Own SVG | Own SVG |
| Raw escape hatch | Vega-Lite, ECharts | Specs whose marks map onto the built-in renderer are drawn; others show as a table |

The first implementation draws everything itself, in about 1,500 lines of dependency-free
JavaScript, for the reason in §11 ("The frontend has no build step"), and draws it by hand:
`frontend/render-sketch.js` is a small rough.js-style library (wobbly double strokes, overshooting
corners, hachure fills) that every renderer uses. Above 600 marks in a layer it falls back to plain
shapes in the same colours, because the wobble costs time and adds nothing at that density. Drawing
is seeded per chart, so a chart looks the same in the editor, an export and a PDF. All charts are SVG,
so PDFs stay vector. A basemap needs tiles, which are not available offline, so maps draw the data alone. The
libraries remain the upgrade path for richer interaction (zoom, GPU-scale networks, tiled maps).

### Building charts

- **Chart switcher under every result**, as in Zeppelin: pick a chart type and columns for its
  channels. The difference is that the result is written into the file as a `/* chart */` comment in
  the query's cell, so a chart made by clicking is reviewable and editable as text.
- **Suggested chart** from the result's column types: one time column and one number gives a line; one
  category and one number, a bar; two numbers, a scatter or a binned heatmap when large; a single
  value, a KPI tile; a geometry or latitude/longitude pair, a map.
- **Cross-filtering.** A selection on one chart filters others through `filter: <selection>`, or any
  SQL cell through its variable.

### Themes

The notebook looks like a sketchbook drawn in Excalidraw ("Option A" of the design mockups): warm
paper with a faint grain, cells in hand-drawn rounded boxes, Excalifont for text and Cascadia Code for
SQL (both SIL OFL, compiled into the extension; `third_party/fonts/LICENSE.md`), values computed in
prose highlighted with a marker, and charts in Excalidraw's palette: a stroke colour with its pastel
as a hachure fill. Dark mode is Excalidraw's own: the page is inverted and its hues turned back.

## 7. Export

| Format | Needs DuckDB to view | Produced by | Contents |
|---|---|---|---|
| `html` (default) | No | Extension alone | One file: viewer, prose, chart specs, chart data |
| `html` with `interactive := 'precomputed'` | No | Extension alone | As above plus precomputed results for input values |
| `html-wasm` | No (ships DuckDB-WASM) | Extension alone | One file, about 12 MB plus data: viewer, DuckDB-WASM, data as a `.duckdb` file |
| `pdf` | No | Browser print, or headless Chrome | Paged document, vector charts |
| `sql` | Yes | Extension alone | Expanded script with no notebook dependency |
| `md` | No | Extension alone | Markdown with tables; each chart as a table of its data |
| `data` | No | Extension alone | One Parquet or CSV file per query cell |

### Single-file HTML

The extension assembles one `.html` file from parts it already has:

- the read-only viewer (a subset of the frontend, stored in the extension),
- the notebook's cells and render descriptions as JSON,
- each chart's data and each visible table's first page as gzip-compressed JSON, base64 encoded.

The page decompresses with the browser's built-in `DecompressionStream` and draws the charts when
opened. It works from a `file://` path, as an email attachment or on any static host; nothing needs a
server. The fonts travel inside the page as data URIs, so it looks the same offline. Measured: the
viewer and renderers are about 35 KB gzip-compressed and the two fonts 90 KB; the revenue example
exports to 252 KB and the 29-chart gallery to 425 KB.

`hide := 'code'`, `'output'` or `'all'` leaves that part of every cell out of any export, on top of
each cell's own `hide=`; prose and inputs always stay. The gear menu offers the same as two
checkboxes. Tables in an export page through the rows it includes (`page_rows`, default 100, raised
to a cell's `rows=` when larger).

### Interactivity without DuckDB

A static page cannot run SQL, so inputs need another answer. With `interactive := 'precomputed'` the
exporter runs the notebook once per value of each `select`, `checkbox` and stepped `slider` input and
stores each result; the page swaps between them. Combinations multiply, so there is a size budget and
the exporter reports which inputs were frozen at their current value to stay inside it. Pluto's static
slider export works the same way. Free-form inputs (`text`, unbounded `number`) are always frozen.

### Fully interactive export

`html-wasm` embeds DuckDB-WASM and the tables the notebook reads, so inputs and selections re-run
real SQL in the reader's browser. It is also a single file that opens from `file://` with no network.
A [spike](../spikes/wasm-single-file/README.md) confirmed this in Chromium: a 13.6 MB page starts
DuckDB in about 1.4 seconds and queries an inlined 200,000-row database. The fixed cost is about
12 MB, against a few hundred kilobytes for the static export, which is why static stays the default.

The spike set four constraints on this export:

- DuckDB-WASM's stock loader hangs under `file://`; a short shim that passes the module to the worker
  by message is needed.
- Data ships as a `.duckdb` file or Arrow, not Parquet. Parquet, JSON and `spatial` are extensions
  that DuckDB-WASM downloads at run time, so they are unavailable offline unless inlined as well.
- The data file must be written in a storage version the bundled engine reads.
- The DuckDB-WASM on npm is engine v1.5.4, which lacks the `$name` shorthand. Until a 2.0-based
  build is published, the exporter rewrites `$name` to `getvariable('name')`.

Built as designed, with one simplification that removes S5: the browser never compiles a chart. A
chart's SQL depends on its spec and its source's column types, not on input values, because inputs and
selections reach the query through `getvariable()`. So the exporter compiles every chart in advance and
the page re-runs the stored SQL after `SET VARIABLE`. The runtime (DuckDB-WASM plus a small adapter,
`wasm/`) is built once with `sh wasm/build.sh` and passed as `wasm_runtime := 'wasm/dist'`; it is not
compiled into the extension because it is 36 MB uncompressed. The revenue example exports to 12.2 MB
and boots in about a second in headless Chromium with the network blocked.

### PDF

Charts are drawn by JavaScript, and the extension has no JavaScript engine, so a browser has to be
involved in making a PDF. Two paths share one print stylesheet (`@page` size and margins, cells never
split across pages, code hidden by default, charts as SVG so they stay vector):

- **From the UI:** "Export PDF" opens the print dialog on the reading view. No dependencies.
- **Headless:** `notebook_export(…, format := 'pdf')` writes the HTML export and runs an installed
  Chrome, Edge or Chromium with `--headless --print-to-pdf`. This was checked with Chromium: content
  drawn by a script after load is captured, and text stays vector.

Maps and GPU network charts are rasterised at print resolution. A Typst backend (text and tables
native, charts as SVG) would give finer typographic control and is noted for later, but it still needs
a browser to produce the SVG.

## 8. Risks and spikes

| ID | Question | Why it matters |
|---|---|---|
| S1 | ~~Can the extension walk a parsed statement for table and parameter references across all statement kinds?~~ | **Done: yes, with the C++ parser and no binding.** See §11 |
| S2 | ~~Arrow IPC from inside the extension, or DuckDB's own serialisation plus the MIT TypeScript decoder?~~ | **Done: neither; JSON.** Pages and chart data are small after aggregation. See §11 |
| S3 | ~~Size of the extension with the frontend embedded; lazy loading of heavy renderers~~ | **Done: about 350 KB of frontend source and 90 KB of fonts (2026-11-17; the pen strokes are 89 KB of that)**, small next to the extension itself |
| S4 | ~~DuckDB-WASM inlined in a single HTML file, opened from `file://`~~ | **Done: it works in Chromium.** See [the spike](../spikes/wasm-single-file/README.md). Firefox, Safari and inlined extensions are untested |
| S5 | ~~Does the extension build for DuckDB-WASM with the server parts left out?~~ | **Done: yes** (§21). `html-wasm` does not need it yet: it ships precompiled chart SQL (§7) |
| S6 | Will the community repository accept an extension that embeds an HTTP server and a multi-megabyte asset bundle? | Distribution. The process itself is documented: a pull request with a descriptor file, after which DuckDB's CI builds for Linux, macOS, Windows and Wasm |
| S7 | ~~Chart compiler coverage: how much of the grammar compiles cleanly to one query~~ | **Done: all of FORMAT.md §5** compiles, with layers and sparklines as one query each. See `examples/charts.sql` |

Other risks:

- **DuckDB version coupling.** Every DuckDB release needs a rebuilt extension. Keeping the C++ side
  small is the mitigation.
- **DuckDB 2.0 is the minimum version.** The `$name` variable shorthand arrived in 2.0. Supporting
  older versions would mean the runner rewriting `$name` to `getvariable('name')`; that is possible
  but is not planned.
- **Reactivity surprises.** Effect cells are the escape from the pure model. The manual-run default
  and visible stale markers are the guard.
- **Scope of charts.** The list in FORMAT.md is long. Every type is drawn, simply; see §6 for what
  the planned libraries would add.

## 9. Roadmap

1. **Format and headless runner.** Parser, `notebook_cells`, `notebook_run`, `sql` export. **Done.**

   1b. **Dependency graph.** `defines` and `reads` per cell from DuckDB's parser, forward-reference
   and cycle errors, `params` for `notebook_run`, `{{ }}` expressions in markdown. Resolves S1.
   **Done.**
2. **Server and minimal UI.** Editor, grid, reactive runs, file sync. Resolves S2, S3. **Done.**
3. **Charts and inputs.** Chart compiler, grammar charts, KPI and table, input widgets, selections.
   **Done.**
4. **In-database storage.** `_sketchbook` schema, save, checkout, history. **Done.**
5. **Export.** Single-file HTML, PDF, Markdown, precomputed interactivity. **Done.**
6. **More chart types.** Maps, hierarchies, flows, networks, pivot. **Done.**
7. **Interactive WASM export.** Resolves S5. **Done.**

8. **Result cache.** FORMAT.md §8; see §17. **Done.**

Next, in rough order: a bundled CodeMirror editor, tracking macro
calls in the graph, Firefox and Safari checks for the exports, building every platform in CI, and the
community-repository submission (S6).

## 10. Open questions

- Should there be a dashboard layout beyond the 12-column `span` grid, such as free placement?
- Should JavaScript cells be added later for bespoke D3, and if so, how do they stay out of the trust
  model's way?
- Should a notebook be able to import cells from another notebook?
- Is the `-- %%` marker the right spelling, or should it follow Databricks' `-- COMMAND ----------`
  for compatibility with that ecosystem?

## 11. What the first pass taught us

The first pass built the format parser, `notebook_cells`, `notebook_sql` and `notebook_run`, with
tests. These findings changed or sharpened the design.

### The extension has to use DuckDB's C++ API

DuckDB offers two ways to write an extension. The first pass tried the stable C API first, because it
is attractive: it built in seconds without compiling DuckDB, and one binary loads into every later
DuckDB release. In DuckDB 2.0 it covers table functions, the file system, statement extraction, Arrow
and interrupts. `notebook_cells` and `notebook_sql` worked on it.

It fails on one requirement: **running SQL from inside the extension**. The C API hands an extension
a database handle only while it is loading. The only way to run queries later is to open a connection
at load time and keep it. That was tested, and it pins the database open: after the user closed their
connection, the write-ahead log was not checkpointed and another process could not open the file.
Loading an extension must not do that. The C API has no way to go from a function's context to a
connection, and no weak reference.

The C++ API does. A function can create a connection from its context when it needs one and release
it afterwards, which is what `notebook_run` does. The same check on the C++ build passed: after a
run, the database closed and checkpointed and a second process could read it. So the extension uses
the C++ API, as the official
UI extension does. The costs are the ones §4 already named, now measured:

- The extension is tied to one DuckDB release and is rebuilt for each.
- The first build compiles all of DuckDB: about 15 minutes on 4 cores. Later builds take seconds.

The second cost shapes how the code is organised. Everything that does not need a database lives in
`src/core`, which has no DuckDB dependency and whose tests compile and run in seconds. If DuckDB
later adds a way to obtain a connection through the C API, the thin `src/functions` layer is the only
part that would need porting back.

### Rule: the extension never owns the database

The same measurement gives a rule for everything that follows. A stored `Connection` keeps the
database alive. The server in step 2 must hold a `weak_ptr` to the database instance, create
connections on demand, and shut down when the database goes away. A notebook session's connection
belongs to the session and is released when the session ends.

### Sessions are separate connections, and that is visible

`notebook_run` gives each run its own connection, as §3 specifies for sessions. The consequence is
user-visible: tables a notebook creates remain, but its temp views and variables are gone when the
run returns. `notebook_sql` exists partly for this reason. It returns the script, which a client can
execute in its own connection to keep the views.

### Variables nobody sets start as NULL

`$name` on an unset variable is an error in DuckDB, which breaks any cell that reads a chart
selection before the user has made one. The runner now finds every variable that cells read but no
input cell or `SET VARIABLE` defines, and sets it to `NULL` before the first cell runs. An untyped
`NULL` is enough: `$picked IS NULL OR ts BETWEEN $picked.lo AND $picked.hi` binds and runs. This
needs no knowledge of the chart spec, so it works before the chart compiler exists. FORMAT.md §2 now
states the rule. The cost is that a misspelt variable name reads as `NULL` instead of failing; the
graph step should report unknown variables as warnings.

### Query or effect is decided without a database

A cell is a query cell if its last statement returns rows. The first pass decides this with a small
lexer in `src/core`, not DuckDB's parser: it finds the statement's main keyword, looking past a
`WITH` clause. That keeps the decision available where there is no database (unit tests, and later
the browser). `json_serialize_sql` was considered as a classifier and rejected: it needs a
connection, and it reports `PIVOT` statements as not serialisable. The lexer is deliberately the
only place that scans SQL text; it tracks strings, nested block comments and dollar-quoted strings,
so a `-- %%` line inside a comment or string is not mistaken for a cell marker.

### The dependency graph comes from the parser, not the binder (S1)

`duckdb_get_table_names` binds, so it fails on statements whose tables do not exist yet, which is the
normal state of a notebook that has not run. The graph instead calls DuckDB's `Parser` and walks the
statement tree (`src/functions/references.cpp`). DuckDB 2.0 made this easier than expected: `INSERT`,
`UPDATE`, `DELETE`, `MERGE` and `COPY` are query nodes, and `ParsedExpressionIterator` walks them all,
so the walker only adds what the iterator skips: expression subqueries, the target of an `INSERT`,
`COPY` and `DROP` tables, `CREATE` and `ATTACH` names, `SET VARIABLE`, and `getvariable('…')`.

The split follows the rule above: the extension supplies one function, "what does this SQL define and
read", and everything else (resolving names to cells, ordering, duplicates, cycles, run order) is in
`src/core/graph.cpp` and unit-tested without a database. Findings:

- `PIVOT` parses into a multi-statement that creates an internal `__pivot_enum_…` type; the walker
  ignores it.
- `ATTACH 'x.duckdb'` without `AS` keeps its path as an expression in 2.0; the walker derives the
  alias `x` from it as DuckDB does.
- CTE names are collected per statement rather than per scope. A CTE that shadows a real table hides
  reads of that table elsewhere in the same statement; this has not mattered in practice.
- Markdown cells may refer to cells below them, so the runner evaluates a markdown cell only after
  everything it reads has run. `notebook_run` therefore returns rows in run order, not file order.
- A syntax error is a warning on the cell, not a format error: the notebook still runs and the cell
  fails on its own terms, as before.
- The unknown-variable warning promised above exists. Chart selections are definitions, so a cell
  reading `$picked_months` warns only if no chart selects into it.

### DuckDB 2.0 specifics worth knowing

- `Connection::Query` returns a `QueryResult` that keeps its connection busy until it is consumed or
  destroyed. Release one result before issuing the next query on the same connection.
- A cell with several statements can be sent as one string; the result is the last statement's.
- Running statements on a second connection from inside a table function works, including DDL.

## 12. What building steps 2 to 7 taught us

### A session is replayed, not held

The server must not hold a `Connection` (§11), yet a notebook session has temporary views and
variables that live in one. The first idea, holding the connection and releasing it when the user's
own connections close, does not work: DuckDB calls `OnConnectionClosed` with its connection-manager
lock held, so releasing a connection there deadlocks. Instead a session is data: its variables, read
back from the connection after each request, and per cell the statements that rebuild what a fresh
connection lacks (the temporary view of a query cell, `CREATE TEMP`, `SET`, `USE`), from
`notebook::ReplaySql` in `src/core/script.cpp` (unit-tested without a database). Each request opens a
connection, replays that, runs, and closes it. Replaying views is cheap because they are lazy.

Replaying a table is not, and replaying statements loses rows: a `materialize=table` cell was
recomputed on every request (paging, an input's choices, the catalog, a peek), and a temp table came
back empty of what `INSERT`, `UPDATE` or `DELETE` had put in it. So the server's sessions
(`SessionState::keep_tables`) have a *store*: `ATTACH ':memory:' AS _sketchbook_session_<n> (HIDDEN true)`,
made by the first cell that needs it. An attached database belongs to the `DatabaseInstance`, not to a
connection, so it survives the request without holding a `Connection`; `HIDDEN` keeps it out of
`duckdb_databases()`, `duckdb_tables()` and the editor's catalog. A `materialize=table` cell creates its
table there and a temp view of it under its own name, and its replay is only that view: the query runs
again only when the cell is run again. After a cell that may change tables (anything but queries,
`SET`, `USE` and `SET VARIABLE`, `notebook::MayWrite`), the temp tables it names or newly made are
copied into the store with their rows, and every new connection copies them back
(`Runner::KeepTempTables`, `Runner::Restore`); `CREATE TEMP TABLE` is then left out of the replay, unless
a copy failed: then the cell's `CREATE TEMP TABLE` statements are replayed and none of its tables comes
from the store (both would clash), and no later cell copies them in; a table an earlier cell made keeps its
older copy, with no mark that it is older. Tests make a copy fail with the `SKETCHBOOK_TEST_FAIL_STORE_COPY`
environment variable (a table name), which changes nothing unless it is set. A table comes back as it was created: the statement DuckDB records for it
(`duckdb_tables().sql`, with its keys, defaults and generated columns), then its rows, a table whose
foreign key names another after that one; only if that
statement cannot run there (a default that uses a temp sequence, which is replayed later) is it rows
only. `ALTER TABLE` counts as a read of its table, so the store copies it again. The remaining cost:
those copies are made on every request, so a large *temp table* (not a `materialize=table` cell) is
still copied per request. The store is detached when the last copy of the session's state goes (the
server stops, or `notebook_stop`), and simply disappears with the database when it closes. Sessions are
not evicted: one lives until the server stops. A session is a stored notebook in its database (the
page's URL names it, `&db=`), or a file by its canonical path, so `a.sql`, `./a.sql` and a link to it
share one; it is built outside the server's lock, so a slow read holds up no other notebook. If it
cannot be attached, the session falls back to replaying the SQL. `notebook_run` and the exports run in
one connection and need no store.

The replay runs on the connection's first use (`Runner::Restore`), not when the `Runner` is made, so
the server registers the connection for `/api/interrupt` (`Runner::Context`) before it, and a peek sets
its `max_execution_time` before it.

### Executions per cell

A named query cell executes its query once to count its rows (creating the view only binds it), once
more with a `LIMIT` for the page the editor is sent, and once per chart query: the count is shared with
its charts, which describe the relation once per cell rather than once per chart. An unnamed query is
one streamed execution (`Connection::Submit` and `QueryResultStream`) that counts every row and keeps
the first page, never holding the whole result, then its chart queries. A `materialize=table` cell
executes once. `test/sql/notebook_exec_counts.test` pins these numbers with a sequence each query
advances (it was 1 + 2k executions for k charts, plus an unlimited page, before).

`notebook_run` runs the whole notebook before returning its first row, so a `LIMIT` or a filter above
it cannot change how many cells run, and `cache := true` is always written. It detaches the databases
its `[notebook]` cell attached when it returns. An `attach:` whose name is already attached to another
path, or in the other mode, is an error rather than a silent reuse; the same database in the same mode
is used and left attached.

The server object lives in the database's object cache as a non-evictable entry. The cache is
destroyed with the database, which stops the server. Checked end to end: a shell running the server
exits, and the database file has no WAL left and opens in another process.

### The frontend has no build step

The design assumed a TypeScript app bundled with npm. That puts Node in the extension build, and
DuckDB's extension CI does not have it. The frontend is plain JavaScript and CSS instead, which CMake
compiles into the binary as byte arrays (`CMakeLists.txt`); editing a file re-runs CMake. The cost is
writing the renderers rather than using libraries (§6). The html-wasm runtime does need Node, so it is
built separately and optionally.

### JSON is enough for results (S2)

Results reach the browser after aggregation or as one page, so they are small. JSON needs nothing in
the browser to decode, keeps the server simple, and is what the exports embed (gzip-compressed). Large
integers beyond 2^53 travel as strings; temporal values as `YYYY-MM-DD HH:MM:SS` text. `DECIMAL` values
in result pages are exact strings (`"268.18"`), and a table shows them as they are unless the cell gives
a format; chart data keeps them numeric. Doubles are written with the fewest digits that read back as
the same double (`notebook::FormatNumber`: 15, 16 or 17 significant digits), so the Markdown export
shows `268.18`, not `268.18000000000001`; a `FLOAT` is first rounded to its own shortest decimal.
JSON has no infinity or NaN, so they travel as the strings `"Infinity"`, `"-Infinity"` and `"NaN"`,
which JavaScript's `Number()` reads back and the table shows as text.

### Variables need types from the browser

A value set from the browser is JSON, but a brush on a timestamp axis must compare as a timestamp.
The server derives the variable's SQL type from what produced it: an input's widget or `from=`
column, or the columns behind a chart selection's channels (`STRUCT(lo T, hi T)` for an interval).
Brushes on a `DATE` axis are typed `TIMESTAMP`, because they end between days.

### DuckDB-WASM is a version behind

The npm DuckDB-WASM is engine 1.5, so the html-wasm export rewrites `$name` to `getvariable('name')`
and writes its data file with `STORAGE_VERSION 'v1.5.0'`. One more difference surfaced: 1.5 cannot
read `.lo` from an untyped `NULL`, so selection variables start as `NULL::STRUCT(lo T, hi T)` there.
Arrow hands decimals over unscaled, and bundlers rename Arrow's classes, so the runtime converts by
`typeId` and scale.

### Smaller findings

- `to` is a reserved word, so the output parameter of `notebook_export` and `notebook_checkout` is
  `file :=`.
- Chart cells now run in `notebook_run`: the compiler is cheap to exercise headless, and a broken
  spec should fail CI like a broken query.
- The PDF export finds Chrome, Chromium, Edge or Playwright's `chrome-headless-shell`; the headless
  shell needs far fewer system libraries than full Chrome.

## 13. What the sketchbook look taught us

- **Hand-drawn is a renderer concern, not a format one.** Nothing in the file changed for it. A
  single primitives library (`render-sketch.js`) sits under the chart renderers, the cell outlines and
  the export viewer, so the look is consistent and could be swapped back to plain SVG in one place.
- **Seeded wobble.** Random wobble that changed on every redraw looked like flicker. Each chart and
  each cell outline seeds its own generator from stable inputs (title, data shape, cell position).
- **Outlines follow their content.** A cell's outline is an SVG drawn behind it at its measured size; a
  `ResizeObserver` redraws it when a result, a chart or a font load changes the size.
- **Nothing scrolls inside the page.** A scrollbar is not part of a sketchbook. A table's page holds
  only the rows that fit its box (`NB.tableRows`, measured from the stylesheet), columns too wide for
  the cell are paged with ‹ › rather than scrolled, a KPI value is sized by its length, and outputs
  clip rather than scroll as a last resort. Only the page itself scrolls.
- **View settings that matter belong in the file.** Page size and hiding code or output change what
  readers of an export see, so the UI writes them as cell options (`rows=`, `hide=`) rather than
  keeping them in the browser. Only the default page size, a personal preference, stays in the
  browser (`localStorage`).
- **Value markers.** To highlight `{{ }}` results in prose, the runner wraps each value in two
  private-use characters (U+E000, U+E001) when asked; the markdown renderer turns them into `<mark>`,
  and the Markdown export strips them. `notebook_run` output is unchanged.
- **SVG attributes do not resolve CSS variables.** Hand-drawn ink is `currentColor`, with the colour
  set in CSS, so themes reach it.

## 14. Charts belong to their query

The first design made every chart a cell of its own, `-- %% chart from=<cell>`. In use, that meant
naming a query only so a chart could find it, and keeping two cells in step for one idea. Now a query
cell can end with `/* chart */` comments: the query says what data is fetched, the charts how it is
shown, and the table remains as the last tab. Chart cells stay for a chart of another cell or of a
table, and every existing notebook still parses.

- **The file is still plain SQL.** A chart is a block comment after the last statement, so DuckDB
  ignores it and `notebook_sql` leaves it out. The parser takes the comments off the body (`Cell::sql`
  is what runs, `Cell::charts` the specs), and the editor rebuilds the body from the two when either
  changes.
- **One relation for every chart.** `ChartFrom` gives the relation a cell's charts read: a chart cell's
  `from`, a named cell's view, or an unnamed cell's query in parentheses. The compiler and the runner
  treat chart cells and query charts alike, so `notebook_chart_query`, exports and DuckDB-WASM needed
  no new chart code.
- **Redraw without re-running.** Editing a spec, or a selection that only a cell's charts filter on,
  redraws that cell's charts (`/api/run` with `redraw`); its query and the cells below it do not run.
- **Selections without cycles.** An overview chart that selects and a detail chart that filters on the
  same query used to be two chart cells reading one view. As charts of one cell, or of cells in either
  order, a filter must not become a dependency, or the pair would form a cycle or a forward reference.
  A query cell's chart filters are therefore listed in `reads` but add no edge to `depends_on`. The
  one thing ruled out is a cell's SQL reading its own chart's selection.
- **A failing chart keeps its result.** The query ran, so its rows and row count stay; the cell is
  reported as an error with the chart's message, which shows in that chart's tab.

## 15. The page is written, not shown

Every piece of a notebook arrives as if a pen wrote it: text letter by letter, shapes stroke by stroke,
tables row by row, in a wave down the page ([HANDWRITING.md](HANDWRITING.md) is the plan and records
what was built). It is a renderer concern like the look itself: nothing in the file format changed.

- **The finished page is the page.** The DOM is built exactly as without the animation, and Ink
  (`frontend/render-ink.js`) lays the animation over it and takes it away again. When a piece is done
  its DOM is identical to a page drawn with the animation off, and so are its pixels; both are tested.
  If the script fails, is switched off or is interrupted, the page is simply complete.
- **Text stays text.** Glyphs are made transparent (`-webkit-text-fill-color`), never removed or
  inserted, so selection, find and screen readers see the whole page from the first frame. A canvas over
  the piece writes each real glyph, revealed through a fat copy of pen strokes recovered from the fonts
  offline (`tools/font_strokes.py`), so the last frame of a glyph is the glyph itself.
- **Time, not state.** A piece's progress is a function of the clock and when it started. `render()`
  rebuilds every cell on every change, so pieces are named by a key and a signature of what they show:
  the same key and signature is never written twice, and a rebuilt element takes over a running piece
  where it was.
- **One pen, one budget.** A single scheduler paces everything: constant pen speed, everything on screen
  within about 2.5 seconds, caps on what is in motion at once, with tables and long code falling back to
  sweeps. What follows the reader's own action is drawn within 200 ms; a click or a key finishes
  everything; content below the fold waits until it is scrolled to.
- **Off when it should be.** Reduced motion, print, PDF export, the gear menu's setting and `ink=off`
  all give the plain page. The existing browser tests run with `ink=off`.
- **Watching is a policy, not a second animator.** Watch mode (`ink=watch`, the gear menu's "watch", an
  export's "✎ watch") uses the same pen with another scheduler: one piece at a time in reading order, one
  stroke or letter at a time, at a human pace on a clock the reader can pause and speed up, nothing
  swept or faded to save time except where a person would hurry too (the 61st bar, the 7th table row).
  The page follows the pen until the reader scrolls. HANDWRITING.md, "Watch mode", has the pace and
  what is approximated.

## 16. Charts that say "compared with what"

Two additions to grammar charts, kept inside the sketchbook: nothing here added a toolbar, a panel or
a library.

### Rules and bands

A reference line used to be a second layer with a `rule` mark. `rules:` and `bands:` make it one line
of the spec (FORMAT §5.1).

- **An aggregate is of what the chart draws.** `y: avg` on a chart of monthly sums means the average
  month, not the average row. The compiler keeps each layer's query before downsampling and takes the
  aggregate over it, as scalar subqueries in one extra query that returns one row. A line drawn with M4
  or points binned to pixels are still averaged over the rows.
- **Still the extension's query, the browser's drawing.** The browser gets a value per rule and never
  aggregates. An export, a PDF and DuckDB-WASM therefore needed no new code: the reference query is one
  more dataset.
- **Behind the data, labels in front.** Lines and hatched bands are drawn after the axes and before the
  marks, in muted ink. Their labels are short handwriting that hatching would hide, so they are written
  after the marks with a halo of paper.
- **They are part of the picture.** The axis grows to include a rule or band outside the data: a target
  nobody reached is the point of drawing it.

### Notes

`notes:` lets an author point at something: a few handwritten words, a curved arrow, optionally a ring
or a bracket (FORMAT §5.1).

- **Data coordinates, pixel placement.** A note names a position in the values of the axes, which is
  what survives a resize, a different page width in a PDF, and a query that returns other rows. Where
  the text goes is decided at draw time, in pixels, by the renderer: the compiler only checks the note
  and types its values by the axis.
- **Finding room is cheap because hover already knew the geometry.** The marks leave their points and
  bars behind for hover values. The same lists, plus points along each line, the rules and their
  labels, and the notes already placed, are what a note must keep clear of. Eight directions at three
  distances are scored by how much they cover and the cheapest wins; outside the plot costs most. It
  is not a layout engine, and `side`, `dx` and `dy` are the escape hatch.
- **Half a position is the common case.** "The bar for Hardware" or "February" names one axis; the note
  then points at the mark there (the top of the bar, the highest line). This is positioning, done in
  the browser from the rows the chart already has, not aggregation.
- **Text width is estimated, not measured.** Measuring needs the font to have loaded, and a detached
  SVG cannot be measured at all; an estimate per character places a note the same way in the editor,
  an export and a PDF.
- **The pen needed nothing.** Notes are `Sketch` shapes and SVG text appended after the marks, so they
  are written after the marks with no animation code. A note in a query cell whose answer depends on
  an input can point at the wrong thing; that is the author's to judge, and the skill says so.

## 17. What the result cache taught us

FORMAT.md §8 specifies it. The code is `src/core/cache.cpp` (keys, entries, fresh or stale, unit
tested) and `src/functions/result_cache.cpp` (where entries live, and the session's copy).

- **The result the editor is sent is the thing to cache.** The draft said Parquet for grids and Arrow
  for charts. The editor holds JSON (§12), so caching anything else would mean a conversion on the way
  in and out and a second shape to keep in step. An entry is the `/api/run` result for the cell, byte for
  byte, and a reopened page goes through exactly the renderers a run does: fresh output looks identical
  because it is identical (the browser test compares the markup).
- **Two keys per cell.** A key that covered chart specs and chart-only selections would make every cell
  below a chart stale when the chart was restyled or brushed. The key is therefore in two layers: what
  the cell computes (chained into its readers' keys) and what it shows on top.
- **Variables are part of the key, so the cache records them.** Freshness is judged with the variables
  as they were last set, kept in `@state` with the value the browser showed. A reopened notebook shows
  its inputs at those values and the session starts with them (values are cast from text to their
  recorded type, no SQL runs), so the first Run all reproduces what is on the page.
- **Until something runs, the page is a picture.** Paging a grid, brushing a chart or redrawing need the
  session's views, and a session that has not run has none. Rather than run behind the reader's back,
  which the trust rule forbids, these say "press Run all", and an input change marks its readers stale.
  After the first run everything is live as before.
- **`notebook_run` writes only when asked, exports never touch it.** A headless run is the source of
  truth for "what does the data say now", so reading the cache would only hide work; writing by default
  would give every scripted run a side effect in the working directory. `cache := true` covers the case
  of a job that prepares a notebook for readers. An export is a document of the data at export time, and
  a stale entry must never leak into one.
- **Fresh means checked, so there is a third state.** The first keys covered only SQL text and
  variables, so a cell stayed "fresh" after the CSV it reads was replaced. Keys now include each literal
  file a cell reads (size, modified time, file id, version tag; a glob's matches), and a cell whose
  inputs cannot be checked (a table no cell defines, a non-literal path, a scan of another system) is
  `unverified` rather than `fresh`. The editor still draws unverified results like fresh ones; the
  states are visible in `notebook_cache()`.
- **Checking costs nothing remote, and happens as each cell reads.** Review rounds 2 and 3 settled how
  files are checked (FORMAT §8). Only local files are looked at: a remote file or a UNC path would cost a
  request on every open, before the reader trusts the notebook, so its cells are `unverified`. A file
  modified in the last two seconds may still change without its modified time changing (git's "racily
  clean" rule); its version is kept with a mark, and a result made from it stays `unverified` rather
  than becoming `stale` once the file is older. Versions are taken just before each cell runs, not once
  per run: versions from before the run made a cell that reads what an earlier cell wrote stale forever,
  and versions after the run would call a file changed under a cell fresh. A view is lazy, so the files
  the views a cell reads select from count as that cell's own, taken when it runs (`KeyBuilder::ViewKey`,
  memoized by those versions, so a chain of views costs its length); files a view cell's other statements
  read keep the versions of when it ran. Table functions are on an
  allow-list (`range` and the like are pure; anything else, an extension's reader or a table macro, is
  `unverified`).
- **Unnamed cells hash their text.** A position would hand a moved cell another cell's result. A hash of
  the text means an edited unnamed cell loses its result, which is the honest outcome; named cells keep
  theirs and go stale.
- **Size is bounded per result (4 MiB of JSON),** checked when written and when read, so a hand-edited
  or foreign entry cannot make the editor parse something huge. Grids already keep one page, so the limit
  is rarely reached.
- **Not done:** entries are not shared between notebooks that compute the same thing, there is no
  total size limit across notebooks, and the html-wasm export does not start from cached data.

## 18. Presenting

A notebook can be shown to a room: one screen at a time, large and centred, with nothing else on the
page but a page number. It is the reading view taken a row at a time, in the editor (gear menu,
**Present**) and in every HTML export (a faint `present` control, or `#present` in the address).
`frontend/render-present.js` (`Present`) holds everything the two share; `app.js` and `viewer.js` still
draw the cells and only ask it which ones.

- **A screen is a row of the reading layout.** One cell, or the consecutive cells whose `span`s share a
  row; an input goes with the row after it, so the audience sees what it changes. An author who laid a
  notebook out for reading has already laid it out for presenting, and `screen=new`, `screen=join` and
  `screen=skip` on a cell cover the rest (FORMAT §1). The obvious alternative, a new screen at each
  heading, fails on both examples: `revenue.sql` has one heading and `charts.sql` one heading
  over thirty charts, so each would be a single screen. Rows need no headings and give `charts.sql` two
  charts to a screen, as its author arranged them.
- **The screen is the reading layout, scaled.** The row is laid out as in the reading view, in a grid
  no wider than what it holds (a title, a 640px chart or three KPIs do not need 1000px), and the whole
  grid is scaled with one CSS transform and centred. Nothing about a cell changes: the same fonts, the
  same charts, the same wobble, only larger. Scaling by font size instead would have meant a second
  set of sizes for every piece.
- **Too tall: shrink a little, then scroll.** A screen that fits is scaled up, to at most 2.5 times.
  One taller than the window is scaled back down so it can be seen whole, but to no less than four
  fifths of the reading view: text smaller than that cannot be read from the back of a room, and a
  table shrunk to fit is worse than a table that scrolls. Past that the screen scrolls (wheel, or the
  up and down arrows, which presenting leaves alone).
- **Moving.** Right arrow, Space, Page Down or a click move on; Left arrow, Page Up or Shift+Space go
  back; Home and End jump; Escape leaves, landing on the cell that was showing; F is full screen.
  Clicks on controls, links, tabs, table paging and charts act on them instead. A picked value gives up
  the keyboard focus so the arrows move between screens again.
- **The pen writes each screen once.** Pieces of a presentation are keyed to it (`Present.prefix`), so
  a screen is written when it first arrives even if the editor had already written the cell. Going
  back, or forward again, to a screen already shown puts it on the page at once: the audience has seen
  it written, and a presenter going back is looking for something. This needs no code: Ink never
  writes the same key and signature twice. Moving on calls `Ink.fresh`, or the key press would make
  the new screen a "quick" update. A key or click still finishes whatever is being written, and the
  same press moves on: one press per screen, however fast the presenter goes.
- **Live.** Presenting changes which cells are built, not how: inputs, brushing, tabs and paging go
  through the same code as the reading view, in the editor and in precomputed and html-wasm exports.
  Cells off screen are re-run all the same and show their new results when their screen comes.
- **State is the address.** `#present=3` is written with `history.replaceState` as the presenter moves,
  so a reload, a restored tab or a shared link opens the same screen, and Back is not filled with
  screens. In the editor a reload keeps the server's session for that notebook: the result cache (§17)
  puts the last results on screen at once, and a session that has run stays live. A page opened at
  `#present` runs nothing until the reader asks (§17); pressing Present runs a notebook that has not
  run, so its inputs and brushes are live.
- **Print is unchanged.** `beforeprint` leaves presenting before the browser lays out the page, so paper
  and PDF get the whole notebook.

What building it taught us:

- **The pen assumed nothing was scaled.** Its canvases are positioned from `getBoundingClientRect`,
  which reports the scaled size, but they live inside the scaled grid and so were scaled a second
  time. `layers()` now measures the scale of the canvas's host and works in the host's own px, with
  the backing store that much denser so the writing stays sharp. At scale 1 the arithmetic is
  unchanged. Shapes needed nothing: they are drawn inside their own SVG.
- **Hidden pieces count as written.** Ink finishes a piece with no size at once, so cells kept in the
  page but hidden would have appeared unwritten when their screen came. Presenting therefore builds
  only the cells of the current screen, which also keeps a screen with thirty charts behind it cheap.
- **"As wide as what it holds" had to be measured.** The first version scaled the 1000px reading
  column, and a title or a chart sat small at the left of a centred, mostly empty box. The grid is now
  measured at its `max-content` width before it is scaled. Outputs are size containers (KPI values are
  sized in `cqw`), which by definition hides their content's width from layout, so containment is
  lifted for the moment of measuring (`.nb-measure`). Tables were the other trap: a table shows the
  columns that fit the width it has, so a screen measured narrow once dropped a column and then
  measured narrower still. While measuring, every column counts.
- **`position: fixed` is not fixed in dark mode.** The dark theme is a filter on `body`, and a filtered
  element becomes the containing block of its fixed descendants. The page number stays in the corner
  only because the presenting page does not scroll: the screen scrolls inside its own box.

## 19. Peek under the pen

While a query cell is being edited, the first rows of the CTE or sub-query the caret is in appear in a
small paper note beside the code, and the part is marked in the code with the highlighter. The cell is
not run and its result does not change. It is the cell-sized half of Instant SQL (§4): cells already
show their own results, so what was missing was a look inside one.

**How it works.**

- **Finding the part** (`src/functions/peek.cpp`, `FindPeekTarget`, also `notebook_peek(sql, cursor)`).
  DuckDB's parser gives the parts: every CTE (with the CTEs it can see, scope by scope) and every
  sub-query in `FROM`, in an expression or under a set operation. The parse tree records where
  expressions and table references are but not where a CTE or a sub-query begins and ends, so each
  part is matched to a pair of parentheses: the innermost pair (from `SqlScanner`, so strings and
  comments are skipped) around something the part contains, whose text parses to the same query.
  A CTE also starts at its name, found with DuckDB's tokenizer, so the caret on `name AS (` counts.
  The innermost part around the caret wins; in the main query there is nothing to show.
- **Building the statement.** A CTE becomes `WITH <the CTEs it reads> SELECT * FROM name`; a sub-query
  becomes `SELECT * FROM (<it>)` with the CTEs it reads. Scopes are kept nested as they were, so a name
  means what it meant in the cell, and a CTE that is not read is left out. It is printed from the parse
  tree (`ToString`), not cut out of the text.
- **Running it** (`Runner::Peek`, `POST /api/peek`). On a copy of the session's state, so the views and
  variables of the cells above are there and nothing is recorded; in `BEGIN TRANSACTION READ ONLY`,
  rolled back; with `max_execution_time` of 1.5 s on that connection. It fetches six rows to show five,
  and counts the rest only if that is quick (a third of the limit).
- **Safety.** Only a plain `SELECT` statement is looked into: `INSERT ... SELECT`, `CREATE TABLE AS`, a
  CTE holding `INSERT ... RETURNING` are refused wherever the caret is. A part that calls a volatile
  scalar function (`nextval`, `setval`, `write_log`...; `random()` and `uuid()` are allowed), or a table
  function that is not on a list of ones known to only read, is refused before it runs. The list
  (`READ_ONLY_TABLE_FUNCTIONS` in `peek.cpp`) has DuckDB's generators and readers (`range`,
  `generate_series`, `unnest`, `glob`, `read_csv`, `read_parquet`, `read_json`...), its `duckdb_*` and
  `pragma_*` information functions and the `notebook_*` functions that only read. DuckDB records no
  side-effect property for table functions, so another extension's (`postgres_execute`), a table
  macro, `query()` and any function added later are refused until they are listed. What the scalar
  check misses in the local database is stopped by the read-only transaction: a macro that hides
  `nextval` fails with "cannot write".
- **Trust.** Opening a notebook does not run it (§3), so the server refuses to peek in a session until
  the reader has run something, and again after the text has been replaced from the file on disk. Its
  own flag (`Session::trusted`) is separate from the result cache's `ran` (§17): a notebook reopened
  showing its cached results has not been run, so nothing is peeked at until it is.
- **Quiet.** The editor asks 320 ms after the caret stops; a newer peek interrupts the one running on
  the server, and a run interrupts any peek. An answer for a caret or a text that has since moved is
  dropped. Errors, SQL that does not parse and "nothing here" all show nothing. Escape puts the note
  away until the caret is in another part; leaving the cell takes it away; the gear menu's "peek"
  switch turns it off, remembered per browser (`localStorage`, `nb.peek`).
- **Look** (`frontend/app-peek.js`, `.nb-peek` in `style.css`). A note like the hover tip, drawn by
  `Sketch.outline`: the part's name (the CTE's, or the sub-query's alias), up to six columns that fit
  in 340 px, five rows, and "first 5 of 320 rows" when the count was cheap. It is `position: fixed` on
  `<body>`, so it never moves the page. It goes in the right margin when the window has room, else at
  the cell's right beside lines of code short enough to leave room, nearest the caret, else under the
  code. It is handed to the pen (`Ink.present(note, 'peek', ...)`); typing never writes anything else.
  Exports and presenting (§18) do not have it.

**What building it taught us.**

- **The parse tree has no spans for query nodes.** DuckDB 2.0's PEG transformer gives expressions and
  table references a location, but a `QueryNode` or a CTE has none. Matching parentheses by
  re-parsing their contents and comparing the result to the node is exact and stays clear of text
  search; it costs a parse per candidate pair, which is fine for a cell.
- **Read-only transactions are the real guard.** Function stability catches the obvious cases with a
  clear message; `BEGIN TRANSACTION READ ONLY` catches the rest (macros, extension functions) without
  a list to maintain. `max_execution_time` is a per-connection setting, so the limit dies with the
  peek's connection.
- **A peek must not disturb the session.** Runner records variables into the state it is given when it
  goes away; peeking on a copy keeps the session exactly as it was.

**Not done.** A part of a recursive CTE that reads the CTE itself (its rows depend on the iteration);
table functions from other extensions cannot be judged, so one that acts on a remote system (for
example `postgres_execute`) inside a CTE could run during a peek: the read-only transaction protects
only the local database; the note can
cover part of the cell's (stale) result in a narrow window when the code leaves no room beside it.
Only Chromium was available for testing.

## 20. Building for every platform

The extension is built by DuckDB's shared pipeline (`extension-ci-tools`) for every native platform
DuckDB distributes for: Linux amd64 and arm64 (glibc and musl), macOS amd64 and arm64, Windows amd64
and arm64 (MSVC) and amd64 with MinGW. The community extension repository requires all of them. The
WebAssembly builds are covered in §21.

### What the binary is made of

The loadable extension on linux_amd64 was 49.2 MB (on the tree with the result cache). Measured, not
guessed, with the link map (`-Wl,-Map`), `nm --size-sort` and test links:

- **DuckDB is about 95% of it, and has to be.** DuckDB 2.0 distributes C++ extensions with
  `EXTENSION_STATIC_BUILD` (the default in `extension-ci-tools`): each `.duckdb_extension` links
  `libduckdb_static.a` with `--gc-sections` and `--exclude-libs,ALL`. The link map puts 39.5 MB of the
  43 MB of mapped sections in objects from that archive: casts 2.2 MB, the PEG transformer 1.8 MB,
  the expression executor 1.7 MB, the optimizer 1.4 MB, and so on through the engine.
- **It is not a linking accident.** `--gc-sections` keeps only what the extension can reach, and an
  extension that runs SQL through a `Connection` reaches the parser, binder, optimizer, every operator
  and every built-in function. A minimal extension whose only code is
  `Connection(db).Query("SELECT 42")`, linked the same way and stripped, is **38.9 MB**. DuckDB's own
  extensions built here agree: `json` 48.8 MB and `loadable_extension_demo` (which opens a
  `Connection`) 47.5 MB unstripped; demos that only register functions are about 13 MB.
- **Leaving DuckDB out does not load.** Linked without `libduckdb_static.a` the extension is 1.8 MB,
  about what the first versions measured, with 132 undefined DuckDB symbols. The released DuckDB 2.0.0
  Python package refuses it (`undefined symbol: _ZTIN6duckdb14SimpleFunctionE`): a host that loads
  DuckDB as a library with `RTLD_LOCAL` leaves no DuckDB symbols to resolve against, which is the
  reason DuckDB's build gives for linking itself into every extension.
- **Our code is about 1.8 MB** at `-O3`: the frontend assets 0.46 MB, the HTTP server (cpp-httplib)
  0.37 MB, the chart compiler 0.19 MB, the rest smaller.
- **The symbol table was 8.6 MB** (`.symtab` and `.strtab`): the binary was not stripped. Release
  builds have no debug information.

What was changed, linux_amd64, bytes (gzip -9, which is how extensions are downloaded):

| Step | Size | gzip |
|---|--:|--:|
| Before | 49,219,368 | 16,925,258 |
| Strip at link time (`-s`; `-Wl,-x` on macOS; MSVC keeps symbols in a PDB) | 40,642,310 | 15,284,437 |
| Our sources at `-Os` (GCC and Clang) | 40,021,158 | 15,009,028 |

The floor is the 38.9 MB minimal extension; ours is 1.1 MB above it. The CI job "Extension sizes"
reports every platform.

Considered and not done, with numbers:

- **Identical code folding** (`--icf` in gold or lld) saved another 1–2 MB, but needs a linker that
  DuckDB's toolchains do not promise on every platform, and folding is only safe when nothing compares
  function addresses, which DuckDB does (casts, table functions).
- **Compressing the frontend** would save about 250 KB: the fonts are already woff2, and the
  JavaScript and CSS gzip from 360 KB to 110 KB. Not worth a decompressor in the server and exports.
- **Building DuckDB for size.** CI links DuckDB's prebuilt `duckdb_static` for each platform, so its
  flags are DuckDB's, not ours.
- **Not linking DuckDB.** The way to a small binary is an extension against DuckDB's C API, which
  links nothing (`cpp_api_demo` here is 0.5 MB). This extension walks DuckDB's parse tree and uses
  `ClientContext`, the object cache and C++ table functions, so that is a rewrite, not a build flag.

### Platform notes

- **No POSIX in portable code.** `unistd.h`, `getcwd` and `/tmp` broke the MSVC build; DuckDB's
  `FileSystem` has what is needed (`GetWorkingDirectory`, `IsPathAbsolute`, `JoinPath`,
  `GetEnvVariable`).
- **PDF export runs a browser through the shell**, so quoting differs: single quotes for `sh`, double
  quotes for `cmd.exe`, which also strips the outer pair of a command line that starts with a quote.
  Chrome and Edge are looked for on `PATH` and in their usual install places on Windows and macOS.
- **Error messages differ by platform.** A test that expected Linux's "No such file or directory"
  failed on Windows ("The system cannot find the file specified"). Match the part DuckDB writes.
- **The pipeline does not test everywhere it builds**: it runs `test/sql` on linux_amd64 (and musl),
  linux_arm64_musl, osx_arm64 and Windows, not on linux_arm64 or osx_amd64.

## 21. The extension in DuckDB-WASM

The extension builds for WebAssembly and loads into DuckDB-WASM 2.0. This answers S5, which §7 had
set aside as not needed.

### Which DuckDB-WASM

- **2.0 is on npm under `next`, not `latest`.** `@duckdb/duckdb-wasm` `1.33.1-dev110.0` and later are
  built with DuckDB at the `v2.0.0` tag, the same commit as our submodule (the npm `gitHead` of each
  release pins `submodules/duckdb` at `d54cbbe3`). `latest` (`1.33.1-dev57.0`) is still engine 1.5.
  The npm version number says nothing about the engine.
- **An extension must match the engine exactly.** Its footer records the DuckDB version and platform,
  and DuckDB-WASM refuses a mismatch. A local build is stamped with a commit hash unless built with
  `OVERRIDE_GIT_DESCRIBE=v2.0.0`.
- **Two bundles, two platforms.** DuckDB-WASM 2.0 ships `base`, whose platform is `wasm_eh`, and
  `threads` (`wasm_threads`, which needs a cross-origin isolated page). There is no `wasm_mvp`
  bundle any more. CI still builds `wasm_mvp`, because the community repository does, but nothing
  can load that build.
- **Unsigned builds load for testing.** `db.open({allowUnsignedExtensions: true})`, then
  `LOAD '<url of sketchbook.duckdb_extension.wasm>'`. DuckDB-WASM fetches extensions by URL. Without
  the flag, only extensions signed with DuckDB's core or community keys load, which a build from the
  community repository would be.

### The boundary

The server needs sockets and threads, and opening a browser or printing a PDF needs processes.
DuckDB-WASM has none of these. One CMake option, `NOTEBOOK_HOST_FEATURES` (off under Emscripten),
draws the line:

| SQL function | In DuckDB-WASM |
|---|---|
| `notebook_cells`, `notebook_sql`, `notebook_chart_query`, `notebook_peek` | Work |
| `notebook_run` | Works. Each run opens its own `Connection`, as natively |
| `notebook_save`, `notebook_checkout` | Built; not exercised by the test |
| `notebook_export` html, md, sql, data | Built; md tested, writing to DuckDB-WASM's virtual file system |
| `notebook_cache`, `notebook_cache_clear`, `notebook_run(cache := true)` | Work, but find no cache: see below |
| `notebook_export` pdf | Fails: "runs a local Chrome, which this build … cannot do" |
| `notebook_start`, `notebook_stop` | Registered with the same signatures; fail: "the notebook server is not available in this build" |

With the option off, `src/functions/server_unavailable.cpp` replaces `server.cpp`, and `export.cpp`
has one `#ifdef` for pdf. `src/core` has no platform conditionals. Paths go through DuckDB's file
system as natively (`ReadNotebook`), so files registered with `registerFileText` and URLs work.

**The result cache needs directories, and DuckDB-WASM's in-memory file system has none.** Creating
a directory there does nothing, and no directory exists. Before the change, `cache := true` wrote
entries that could never be listed back. Now the cache checks that its directory exists after creating
it and is otherwise off: nothing is written, and `notebook_cache` lists nothing. The check is not
specific to WebAssembly. A notebook on `opfs://` paths should have a working cache, since DuckDB-WASM's
OPFS support has directories, but that is untested.

**`wasm_threads` needed a build fix, and loading into the threads bundle is not reliable.**
extension-ci-tools' release branch (`v2.0-cyanoptera`) leaves out two flags when it builds
`wasm_threads`. The first is `-fwasm-exceptions`, lost to a misnamed variable. The second is
`-DUSE_WASM_THREADS=ON`, DuckDB's switch that links a side module against shared memory. Without them
`LOAD` fails with an empty `dlopen` error, and DuckDB's own `icu` extension built the same way fails
too. Its `main` branch has both. Our `Makefile` sets them after including its Makefile. With them,
everything works in the threads bundle as in the base one, except that `LOAD` hangs about one time in
eight. The `icu` extension hangs at the same rate, so this is a race in DuckDB-WASM's threaded
extension loading, not in this extension. The test retries a hung `LOAD` in a fresh page, for
`wasm_threads` only, and reports each retry. A page using the threads bundle would need a timeout
around `LOAD` as well, or should use the base bundle.

`test/wasm/run.cjs` loads the build into DuckDB-WASM in headless Chromium and runs the functions on
`examples/revenue.sql`. CI runs it on the artifacts of the extension pipeline, which itself does not
test wasm builds. The extension is 1.2 MB, or 400 KB gzip-compressed (CI's "Extension sizes" job; against 31 to 48 MB
for the native builds, which link DuckDB in). It loads in about 0.1 to 0.2
seconds once DuckDB-WASM has started (about 1.6 seconds).

### What it means for the html-wasm export

Nothing in the export changed yet. Two separate steps are open:

1. **Move the runtime to DuckDB-WASM 2.0** (`wasm/package.json` to `1.33.1-dev118.0` or later; the
   bundle files are now `duckdb-base.wasm` and `duckdb-browser-base.worker.js`). This removes the 1.5
   workarounds of §12:
   - the `$name` to `getvariable('name')` rewrite (the test checks `$name` works);
   - the typed `NULL::STRUCT(lo T, hi T)` selections (the test checks `.lo` of an untyped `NULL`
     variable returns NULL);
   - writing data with `STORAGE_VERSION 'v1.5.0'` (follows from the engine reading its own default
     storage version; not tested in the browser).
2. **Inline the extension** (about 400 KB compressed, against 12 MB for DuckDB-WASM) and `LOAD` it
   through the same worker shim that serves the module from memory under `file://`. The page could
   then compile chart specs and run notebooks the way the server does, instead of replaying SQL that
   was compiled in advance. That makes an exported page editable: change a spec or a cell and see
   the result. A local build needs `allowUnsignedExtensions`, which a page should not set for SQL it
   did not write. A build signed by the community repository needs no such flag.

Precompiled charts stay right for a read-only export. Inlining is worth doing when the export is
meant to be edited.

## 22. Wide pages

The reading column is 1000px, which suits prose and a 640px chart but wastes most of a large monitor
on a notebook of wide tables and time series. **Wide** lets the page use the window's width. It is a
reader's setting first: the editor's gear menu (*wide*), a quiet *wide* control in every HTML export,
or `?width=wide` / `?width=normal` in the address, remembered in the browser (`nb.width`). An author
can make it the default for readers who have not chosen with `width: wide` in the `[notebook]` cell
(FORMAT §6). `NB.width` in `frontend/render-core.js` holds all of it for the editor and the viewer.

- **What widens.** The cell column, banners and the graph lose their `max-width`; the page keeps its
  gutters (the editor's margins, the viewer's padding of 16 to 48px). Tables show the columns that now
  fit. Spans keep their meaning: `span=6` is half of whatever the row is.
- **Charts are drawn for their width, not stretched.** A chart is compiled for its cell's `width`
  (640px by default) and its SVG scales down to fit a narrower box. Scaling it up would make the
  handwriting and the wobble huge. A wide page instead draws a chart again at the width its box has,
  with the same height, so the axes get more ticks and the marks more room. Cells are built detached,
  so `NB.chart` draws at the compiled width and `NB.width.fit` draws again once the cell is in the page
  and before the pen is handed the piece; the pen only ever sees the final drawing. A window resized
  while wide renders again. A chart never gets narrower than it was compiled for, so a phone sees what
  it always saw, and a cell with `width=` keeps it: the author asked for that size. Downsampling (§16)
  still targets the compiled width; a line of up to 2,560 points is fine at any screen width.
- **Prose keeps its measure.** Paragraphs were already at most 760px; lists and quotes follow them on
  a wide page. Lines much longer than 80 characters are hard to read and the eye loses its way back to
  the start of the next one, and headings, tables and charts beside the prose still use the width.
- **Presenting and paper ignore it.** A screen is already sized by what it holds and scaled to the
  window (§18), so presenting draws the normal page whatever the setting and comes back wide. Paper has
  its own width: `beforeprint` draws the normal page and `afterprint` puts the wide one back, and the
  PDF export marks its page `data-width-fixed="normal"`, as it marks it `data-ink="off"`.
- **The pen needs nothing.** Switching redraws the page through the usual render; a chart drawn at
  a new width has a new signature and is written again, and a written wide page is identical to it
  drawn with `ink=off` (tested, also when the switch comes mid-way and in watch mode). The export's
  header is keyed by its text, so marking the control does not write the header again.
