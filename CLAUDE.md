# Working on duckdb-sketchbook

A DuckDB extension that makes a plain `.sql` file a notebook. Read `docs/DESIGN.md` for what is being
built and why, and `docs/FORMAT.md` for the file format. This file is about how to work in the code.

## Where things stand

Every roadmap step of `docs/DESIGN.md` §9 has a first implementation; §12 records what building it
taught us and where it departs from the design.

| SQL function | What it does | File |
|---|---|---|
| `notebook_cells(path)` | One row per cell: kind, name, options, body, charts, defines, reads, depends_on, errors, warnings | `src/functions/notebook_cells.cpp` |
| `notebook_sql(path)` | The notebook as a plain SQL script | `src/functions/notebook_sql.cpp` |
| `notebook_run(path, params := {…})` | Runs every cell (charts too) in its own session; one row per cell | `src/functions/notebook_run.cpp` |
| `notebook_chart_query(spec, from)` | The SQL and render description a chart spec compiles to | `src/functions/notebook_chart_query.cpp` |
| `notebook_save`, `notebook_checkout` | Notebooks in the `_sketchbook` schema, with history | `src/functions/storage.cpp` |
| `notebook_export(path, file := …, format := …)` | html, html-wasm, pdf, md, sql, data | `src/functions/export.cpp` |
| `notebook_start`, `notebook_stop` | The HTTP server and browser UI (not in WASM builds) | `src/functions/server.cpp` |
| `notebook_peek(sql, cursor)` | The CTE or sub-query at a byte offset and the query that selects from it (DESIGN §19) | `src/functions/peek.cpp` |
| `notebook_cache`, `notebook_cache_clear` | The result cache: what is kept, fresh or stale; clearing it | `src/functions/result_cache.cpp` |

Every path argument also accepts `db:<name>[@version]` for a stored notebook.

The graph is split in two: `src/functions/references.cpp` walks DuckDB's parse tree to say what SQL
defines and reads, and `src/core/graph.cpp` does everything else (resolution, ordering errors, cycles,
run order). `ReadNotebook` runs both, so every function sees an analysed notebook.

Running cells goes through one place, `Runner` in `src/functions/session.cpp`, used by `notebook_run`,
the exports and the server: `Runner::Prepare` before the first cell, `Runner::RunAll` for every cell,
`RunForEditor` for one cell with the result the editor is sent. The chart compiler is
`src/core/chart*.cpp`: the `Compiler` class in `chart_compiler.hpp`, with grammar layers, rules and
notes, and typed charts each in a file of their own. The browser draws its output
with `frontend/render-*.js`, all of it hand-drawn through the primitives in
`frontend/render-sketch.js` (the "sketchbook" look, DESIGN §6 Themes).

Every piece of the page is written by a pen as it appears (`frontend/render-ink.js`, DESIGN §15,
`docs/HANDWRITING.md`). Watch mode (`ink=watch`, gear menu "watch") is the same pen under another
scheduling policy (`planWatch`, `timelineWatch`, the pen's own clock): one piece and one stroke at a time,
slowly, in reading order, with the page following the pen. The pen strokes for the fonts are generated offline by `tools/font_strokes.py`
into `frontend/ink-strokes.js`.

Presenting (DESIGN §18) is `frontend/render-present.js` (`Present`), shared by the editor and exports:
it decides which cells make the current screen and scales them to the window; `app.js` and `viewer.js`
still build the cells.

The result cache (FORMAT §8, DESIGN §17) keeps each cell's last `/api/run` result beside the notebook
(`.duckdb-sketchbook/`) or in `_sketchbook.cache`. Keys and the fresh/stale decision are in
`src/core/cache.cpp`; the store and the session's copy in `src/functions/result_cache.cpp`. The editor
reads it on open and writes it after every run; `notebook_run` writes it only with `cache := true`.

Not built: a CodeMirror editor, charting libraries (see DESIGN §6).

## Layout

```
src/core/          Plain C++17, no DuckDB headers. Lexer, format parser, script generation, graph,
                   YAML and JSON, chart compiler.
src/functions/     The SQL surface (one file per function), the runner, the server. DuckDB C++ API.
tools/            Development tools whose output is checked in (pen strokes for the handwriting). Not run by CI.
frontend/          The browser UI, export viewer, chart renderers and fonts: plain JS, CSS and woff2, no build
                   step. CMake compiles every file here into the extension (GetAsset).
wasm/              The DuckDB-WASM runtime for html-wasm exports, built separately with Node.
src/include/       Headers: notebook/*.hpp for core; functions.hpp, session.hpp and result_cache.hpp (which
                   include DuckDB) for the SQL surface.
test/core/         Unit tests for src/core. No DuckDB needed.
test/sql/          sqllogictest files: end-to-end tests through SQL.
test/browser/      Playwright tests of the server, UI and exports in Chromium.
test/wasm/         The WebAssembly build loaded into DuckDB-WASM in headless Chromium (Node).
test/notebooks/    Notebook files the SQL tests read.
examples/          Notebooks for people to read. Tests check they stay valid and runnable.
.github/skills/    Agent skills for writing sketchbooks (notebooks) and making them look good: authoring
                   (with a template and check.sh), charts, design, preview/export, and the look itself.
duckdb/            Submodule, pinned to the DuckDB release we build against (v2.0.1).
extension-ci-tools/ Submodule: DuckDB's shared build and CI scripts.
third_party/       Licences of bundled assets (fonts).
spikes/            Throwaway experiments kept as evidence. Not built or tested by CI.
site/              The user documentation, published to GitHub Pages: MkDocs sources, theme, the
                   notebooks the docs show, generated outputs, screenshots and exports.
```

## Build and test

```bash
git submodule update --init --recursive --depth 1
```

```bash
make core_test
```

Compiles and runs the `src/core` tests in a few seconds. Use this loop for anything in `src/core`.

```bash
GEN=ninja make release
```

Builds DuckDB and the extension. **The first build compiles all of DuckDB and takes about 15 minutes
on 4 cores**; later builds only recompile the extension and take 5 to 15 seconds. Start it in the
background as soon as a session begins.

```bash
make test
```

Runs `test/sql/*.test` against the release build.

```bash
./build/release/duckdb
```

A DuckDB shell with the extension already linked in, for trying things by hand.

The loadable file is `build/release/extension/sketchbook/sketchbook.duckdb_extension`. A local build
stamps it with DuckDB's commit hash, because the submodule is a shallow clone with no tag, so it
loads into the shell built here but not into a released DuckDB. CI builds are stamped `v2.0.1`.

To use the extension in an installed DuckDB v2.0.1 (for example one with the `hdfs` extension), build a
copy stamped `v2.0.1` and install it; a running server must be restarted afterwards:

```bash
ninja -C build/v2.0.1 sketchbook_loadable_extension   # configured once with -DOVERRIDE_GIT_DESCRIBE=v2.0.1
duckdb -unsigned -init /dev/null -c "FORCE INSTALL '$PWD/build/v2.0.1/extension/sketchbook/sketchbook.duckdb_extension'"
```

Then `LOAD sketchbook` in `duckdb -unsigned`. A quicker check that a local build loads into a released
DuckDB is the Python package, which also loads extensions with `RTLD_LOCAL` as most clients do:
`pip install duckdb==2.0.1`, connect with `allow_unsigned_extensions` and
`allow_extensions_metadata_mismatch` set, and `LOAD` the file.

The loadable extension is about 40 MB on linux_amd64 (15 MB gzipped), and about 95% of that is DuckDB
itself, linked statically as DuckDB 2.0 distributes every C++ extension. It is stripped at link time
(`CMakeLists.txt`). Do not expect a flag to make it much smaller: `docs/DESIGN.md` ("Building for every
platform") has the breakdown and what was tried. CI's "Extension sizes" job reports every platform.

Run one test file with `./build/release/test/unittest "test/sql/notebook_run.test"`.

```bash
make frontend_check
```

Checks that `frontend/*.js` parses (needs Node). The frontend is compiled into the extension, so a
change to it needs `make release` (CMake notices the change and regenerates the asset table).

```bash
make browser_test
```

Runs `test/browser/test_browser.py`: starts the server from a DuckDB shell and drives the editor in
Chromium (run all, inputs, brushing, editing, saving, file sync), then checks every export format.
Needs `pip install playwright` and `playwright install chromium`. Set
`NOTEBOOK_WASM_RUNTIME=$PWD/wasm/dist` after `make wasm_runtime` (Node and npm) to include the
html-wasm export. On a machine without Chromium's system libraries, `apt-get download` the missing
packages, `dpkg -x` them into a directory and point `LD_LIBRARY_PATH` at its `usr/lib/x86_64-linux-gnu`.

### WebAssembly (DuckDB-WASM)

The extension also builds for DuckDB-WASM (`wasm_eh`, `wasm_threads`; DESIGN §21). Install the
Emscripten version DuckDB's extension pipeline uses, 6.0.9, once:

```bash
git clone --depth 1 https://github.com/emscripten-core/emsdk /opt/emsdk
/opt/emsdk/emsdk install 6.0.9 && /opt/emsdk/emsdk activate 6.0.9
```

```bash
source /opt/emsdk/emsdk_env.sh
OVERRIDE_GIT_DESCRIBE=v2.0.1 make wasm_eh
```

Writes `build/wasm_eh/extension/sketchbook/sketchbook.duckdb_extension.wasm`. It compiles DuckDB for
WebAssembly too: about 12 minutes on 4 cores the first time. `make wasm_threads` builds the threaded
variant. Then:

```bash
(cd test/wasm && npm ci) && node test/wasm/run.cjs
```

Loads every built architecture into DuckDB-WASM 2.0 in headless Chromium and runs the notebook
functions on `examples/revenue.sql` (`test/wasm/README.md`). CI runs it on the pipeline's artifacts
(the `wasm-test` job).

## The documentation site

`site/` is the user documentation, published at https://luke-falvey.github.io/duckdb-sketchbook/ by
`.github/workflows/docs.yml` (builds on every pull request, deploys from `main`). It is MkDocs with a
small theme of its own (`site/theme/`); the toolchain is `site/requirements.txt`.

```bash
pip install -r site/requirements.txt
mkdocs build --strict -f site/mkdocs.yml && python3 site/tools/check_links.py   # into build/site
mkdocs serve -f site/mkdocs.yml        # preview at http://127.0.0.1:8000; stop it when done
```

- `site/content/reference/format.md` is `docs/FORMAT.md`, rendered by `site/hooks.py`: there is one
  specification, and it is `docs/FORMAT.md`.
- Pages include files rather than copying them: notebooks with `--8<-- "path"` and single cells with
  `<!-- cell NAME in PATH -->` (default `examples/charts.sql`; the chart reference is built this way).
- Queries the docs show live in `site/outputs/<name>.sql`; `python3 site/tools/outputs.py` runs them
  with the release build and writes `<name>.txt`. Screenshots (WebP), the live exports in
  `site/content/examples/` and the Markdown sample come from `python3 site/tools/assets.py` (needs
  Playwright and Pillow; it starts one notebook server and stops it). Both are committed, so the
  docs workflow never builds DuckDB. Rerun them when output or the look changes.
- The notebooks in `site/notebooks/` are run by `test/sql/docs_notebooks.test`; add new ones there.

**A change to the format, a chart key, an SQL function (arguments, defaults, columns) or a setting
needs the matching change in the site** (the guide that explains it and the reference page that
lists it), in the same pull request, as it does in `docs/FORMAT.md`.

## Conventions

**Put logic in `src/core`.** Anything that does not need a database (parsing, validation, generating
SQL text) goes there, takes strings in and returns plain structs, and gets a unit test in
`test/core/core_test.cpp`. This keeps the fast test loop useful and the code portable. `src/functions`
should stay thin: read the file, call core, shape the result.

Small string helpers (`Trim`, `Lower`, `Contains`, `AddUnique`, `ReplaceAll`) are in
`notebook/text.hpp`; use them rather than writing another. What an export makes as text (the Markdown
document, the title) is in `src/core/document.cpp`.

**Format errors are data, not exceptions.** `ParseNotebook` never throws. Problems are attached to the
cell in `Cell::errors` so a UI can show all of them. Functions that cannot proceed on an invalid
notebook call `ThrowIfInvalid`, which reports the first error and points at `notebook_cells`.

**Adding a SQL function.** Declare `RegisterX` in `src/include/notebook/functions.hpp`, implement it in
`src/functions/x.cpp` as a table function (bind reads and parses, scan emits rows), add the file to
`CMakeLists.txt`, call it from `src/sketchbook_extension.cpp`, and add `test/sql/x.test`. Copy
`notebook_sql.cpp` as the smallest example. A function that returns its rows in one call uses `OnceState`
and `OnceInit`, and reads named parameters with `NamedString`/`NamedBool` (`common.cpp`).

**The frontend has no build step.** Plain browser JavaScript (no modules, no npm), so the extension
build needs nothing but CMake. Draw lines, boxes and fills with `Sketch` (`render-sketch.js`), not
raw SVG, so everything keeps the hand-drawn look; seed it (`Sketch.reseed`) so a drawing is stable.
Use `currentColor` for ink in SVG: attributes do not resolve CSS variables. Fonts in `frontend/` are
SIL OFL; their licence is in `third_party/fonts/LICENSE.md`.
Shared rendering lives in `frontend/render-*.js` and is used by both
the editor (`app.js`, `app-extra.js`) and exports (`viewer.js`). Results travel as JSON. What both
show of a cell is in `render-core.js` (`NB.inputWidget`, `NB.queryTabItems`, `NB.hides`, `NB.span`,
`NB.kindName`): change it there, not in `app.js` or `viewer.js` alone. `app-extra.js` always loads with
the editor; it is a split for size, not an optional part.

**Every new piece of the page is handed to the pen.** After building a piece, the editor and the viewer
call `Ink.present(element, key, signature)`; see `write()` in `app.js` and `viewer.js`. Give it a key
that names the piece stably across renders and a signature of what it shows (omit it to hash the
element's markup). Draw shapes with `Sketch` and text as ordinary text, and they are animated for free.
Never animate a component yourself, never store animation state on DOM nodes, and never leave anything
behind: a written piece must be identical to the same piece drawn with `ink=off` (tested).

**Never keep a `Connection` alive past a query.** A stored `Connection` owns the database instance.
If the extension holds one, the database is not closed when the user closes it: the file stays locked
and is not checkpointed. This was measured, not assumed (see DESIGN §11). Create connections where
they are used and let them go, as `notebook_run` does. The server holds a
`weak_ptr<DatabaseInstance>`, and a notebook session is replayable state (`SessionState`), not a
connection: each request opens one with `Runner`, which replays the session's variables and views.
The server object lives in the database's object cache, so it stops when the database closes.

**Release a `QueryResult` before the next query on the same connection.** In DuckDB 2.0 a result keeps
its connection busy until it is consumed or destroyed.

**Reading files** goes through `ReadNotebook`, which uses DuckDB's file system, so remote paths work
when `httpfs` is loaded. Do not use `std::ifstream`.

**The format spec and the parser move together.** A change to `src/core/format.cpp` that changes what
is accepted needs the matching change in `docs/FORMAT.md`, and the reverse.

**Style.** DuckDB's: tabs, 120 columns, `PascalCase` functions, `snake_case` variables.
`.clang-format` is DuckDB's own. Run `clang-format -i` on files you touch.

**Tests.** Every behaviour change gets a test. Prefer a core unit test; add a SQL test when the
behaviour involves DuckDB (execution, file access, result shape). SQL tests must clean up tables they
create, because all test files share one database.

## Things that are easy to get wrong

- Code in `src/functions` is compiled with MSVC, MinGW, Apple clang and musl as well as glibc gcc. No
  POSIX headers (`unistd.h`), `getcwd` or hard-coded `/tmp`: use DuckDB's `FileSystem`
  (`GetWorkingDirectory`, `IsPathAbsolute`, `JoinPath`, `GetEnvVariable`). Anything passed to
  `std::system` is quoted per shell (`ShellQuote` in `export.cpp`). A SQL test must not expect an
  OS-specific error message: Windows words "file not found" differently.

- Watch mode keeps the gentle mode's rules: nothing new animates outside `Ink`, and a watched page
  ends identical to `ink=off`. Its times are on the pen clock (`Ink.penNow()`), which `Ink.advance(ms)`
  moves at the reader's speed; `test_ink_watch` shows how to drive it. A piece is taken apart only when
  the pen reaches it, so a component that changes size or content while waiting needs nothing special.
- Chromium writes `el.style` changes into the `style` attribute lazily: `removeAttribute('style')` after an
  `el.style` write that nothing has read since leaves `style=""` behind. Ink's `restore()` reads the
  attribute first; do the same anywhere that puts an element's attribute back.
- Browser tests start a server with `serve()` and stop it with `stop()` (`test/browser/test_browser.py`),
  which make sure no DuckDB shell is left running when a check fails.
- Browser tests check the finished page and load every page with `ink=off` (`still()` in
  `test/browser/test_browser.py`). To test the animation itself, stop its clock (`window.__INK_PAUSE`
  in an init script, then `Ink.advance(ms)`); `test_ink` shows how. After changing the fonts, run
  `tools/font_strokes.py` (`tools/README.md`).

- Agents: never start detached background shells (servers, previews, watchers). Use attached async
  shells and stop everything you started before finishing a turn; stop servers by PID. Leftover
  detached shells piled up and got in the way of approval prompts.

- While presenting, only the cells of the current screen are in the page, inside a grid scaled by a CSS
  transform. Anything that measures the page there must allow for the scale (`getBoundingClientRect`
  is scaled, `offsetWidth` is not; Ink's `layers()` shows how), and anything fixed to the window must
  not rely on the page scrolling. A cell's place in a presentation is decided before it is built
  (`A.presentable` in `app.js`, `parts` in `viewer.js`): keep those in step with what the cell shows.
  A new control inside a cell must be matched by `ACTS` in `render-present.js`, or a click on it moves
  to the next screen. Test with `#present=N` in the URL; `ink=off` goes in the query string.
- Wide pages (DESIGN §22) are `NB.width` in `render-core.js`. A chart whose cell has no `width=` is drawn
  again at its box's width by `NB.width.fit`, which must run once the cell is in the page and before
  `Ink.present` (see `A.render` and the viewer's `render`). Pass `fluid` to `NB.chart` for such charts.
  Presenting and print always get the normal width; test with `?width=wide`, `ink=off` as usual.
- A SQL cell's `body` is its full text, chart comments included; what runs is `Cell::sql`, and its charts
  are `Cell::charts`. Use `sql` for anything that executes or analyses SQL, and `ChartFrom` for the
  relation a cell's charts read. The editor edits `sql` and the specs separately and rebuilds `body`.
- A `-- %%` line inside a block comment or string literal is content, not a marker. All text scanning
  goes through `SqlScanner` in `src/core/sql_lexer.cpp`, which tracks that state. Do not use regular
  expressions or `find` on SQL text.
- DuckDB block comments nest. `$$…$$` and `$tag$…$tag$` are string literals, while `$name` is a
  variable reference.
- Graph names are lower-case relation names or variables with a leading `$` (`monthly`, `$region`).
  Core tests use a fake extractor (`FakeExtract` in `test/core/core_test.cpp`); behaviour that depends
  on DuckDB's real parse tree needs a SQL test.
- Whether a cell is a query is decided by the lexer (`IsQueryStatement`), not by DuckDB's parser, so
  it works without a database. If you replace it with the real parser, keep the core tests passing.
- `notebook_run` executes in a session of its own. Tables it creates persist; its temp views and
  variables are gone when it returns. To run a notebook in your own connection, execute the output of
  `notebook_sql`.
- DuckDB 2.0's C++ API differs from older examples and from most of what you will find online.
  Table functions write with `output.data[col].Append(Value(...))` and do not set a cardinality;
  bind receives `vector<Identifier> &names`; `Connection::Query` returns `QueryResult`. Read
  `duckdb/AGENTS.md` and copy from `duckdb/src/function/table/system/` when unsure.
- sqllogictest expected results cannot contain blank lines or multi-line values. Flatten them in the
  query, as `test/sql/notebook_sql.test` does.
- The result cache must never fail anything: every store operation catches and gives up quietly.
  Opening a notebook must run no SQL, so cached variables are restored by casting text to their
  recorded type, not by `SET VARIABLE`. A change to what a cache key covers or to the entry layout
  raises `CACHE_FORMAT` in `src/core/cache.cpp`.
- The extension was called `notebook` before it became `sketchbook` (`LOAD sketchbook`,
  `sketchbook.duckdb_extension`, CMake target and CI artifacts). The SQL functions keep their `notebook_*`
  names and a file is still a notebook. Stored data moved to `_sketchbook` and `.duckdb-sketchbook/`, and
  the old `_notebook` schema and `.duckdb-notebook/` cache are still read: `StorageSchema` in
  `storage.cpp` (the first save copies an old history over) and the `legacy` store in `result_cache.cpp`
  (read when the new place has nothing, cleared with it, never written). `test/sql/legacy_names.test`
  and `test_cache` cover both; keep them.
- `to` is a reserved word: a table function cannot take `to :=`. Output paths are `file :=`.
- `ExtensionCallback::OnConnectionClosed` runs with DuckDB's connection-manager lock held. Do not
  create or destroy connections from it.
- Variables set from the browser arrive as JSON. Give them the SQL type of what produced them
  (`VariableSqlType`), or a brush on a timestamp axis compares text with timestamps.
- Peek under the pen (`frontend/app-peek.js`, `POST /api/peek`, DESIGN §19): cursor positions sent to the
  server are byte offsets, not JavaScript string indexes. A peek runs on a copy of the session's state,
  in a read-only transaction with `max_execution_time`; keep all three if you change `Runner::Peek`.
  The server refuses to peek until the reader has run something (`Session::trusted`, set by a run and cleared when the text is reloaded from disk; separate from the cache's `Session::ran`).
- The html-wasm export runs on DuckDB-WASM 1.5: `$name` is rewritten to `getvariable('name')`, data
  is written with `STORAGE_VERSION 'v1.5.0'`, and selection variables start as typed NULLs.
  The extension itself targets DuckDB-WASM 2.0 (npm `next`, `1.33.1-dev121.0` and later); see DESIGN §21
  for which of these workarounds 2.0 makes unnecessary.
- The WebAssembly build has no server, no PDF export and no process spawning: `NOTEBOOK_HOST_FEATURES`
  is off under Emscripten, `server_unavailable.cpp` replaces `server.cpp`, and `export.cpp` has one
  `#ifdef NOTEBOOK_NO_HOST_FEATURES`. Keep anything that needs sockets, threads or processes behind
  that boundary, and keep `src/core` free of platform conditionals.
- Build wasm targets without `GEN=ninja` (they run `emmake make`) and with `OVERRIDE_GIT_DESCRIBE=v2.0.1`:
  DuckDB-WASM refuses an extension whose stamped version or platform differs from its own, and a local
  build is otherwise stamped with a commit hash.
- DuckDB-WASM 2.0 has two bundles: `base` is platform `wasm_eh`, `threads` is `wasm_threads`. No bundle
  loads `wasm_mvp`, though CI still builds it.
- `wasm_threads` builds need `-fwasm-exceptions` and `-DUSE_WASM_THREADS=ON`; without them `LOAD`
  fails with an empty dlopen error. extension-ci-tools' Makefile passes the second; the `Makefile`
  adds the first (`WASM_CXX_THREADS_FLAGS`) after including it. Even with them, DuckDB-WASM's threads bundle hangs in `LOAD` about one time in eight
  (DuckDB's own extensions too), which is why `test/wasm/run.cjs` retries it there.
- Do not build natively from a shell that has sourced `emsdk_env.sh`: `make release` then fails with
  error 127. Each Bash call starts a fresh shell, so source it only in the call that builds wasm.
- clang-format re-indents lambdas inside `server.cpp` with mixed tabs and spaces; match on content,
  not indentation, when scripting edits there.
- `vcpkg.json` must exist at the repository root even with no dependencies, or CI fails at configure.
- An extension is only linked into the build's `duckdb` shell if `extension_config.cmake` calls
  `duckdb_extension_statically_link`.
- The version of DuckDB in the `duckdb` submodule, the `duckdb_version` in
  `.github/workflows/MainDistributionPipeline.yml`, and the `extension-ci-tools` branch must all name
  the same release. extension-ci-tools has one branch per release line, not per patch release:
  `v2.0-cyanoptera` serves every v2.0.x (`ci_tools_version`, the reusable workflow's ref and the
  submodule's branch). Artifact names carry the version (`sketchbook-v2.0.1-extension-<platform>`),
  so the jobs that download them change with it, as do `test/ci/test_loadable.py`, the pip pin and
  DuckDB-WASM in `test/wasm/package.json` (an npm release whose `submodules/duckdb` is the same commit).
- CI artifacts are the extension binaries only. This repository's workflows upload nothing of their
  own (the docs site goes to Pages with `upload-pages-artifact`, which is a deployment, not a build
  artifact). The reusable pipeline uploads `sketchbook.duckdb_extension` (`.wasm` for wasm targets),
  plus, on platforms it tests, `test-support/<id>/test-support.tar.gz` (DuckDB's shell, test runner
  and library) inside the same artifact; it has no input to turn that off except `skip_tests`.

## Rules, bands and notes on charts

`rules:`, `bands:` and `notes:` are spec-level keys of grammar charts (FORMAT §5.1, DESIGN §16).

- Aggregates for rules and bands (`y: avg`) are compiled in `Compiler::References` in
  `src/core/chart.cpp` from each layer's query *before* downsampling (`LayerInfo::base`), into one extra
  query whose index is `render.reference_data`. Never compute them in the browser.
- Notes need no query: `Compiler::Notes` only validates them and types their values by axis. Placement
  is `drawNotes` in `frontend/render-chart.js`, which avoids what the marks leave for hover
  (`plot.__points`, `__bars`, `__dots`, gathered by `markObstacles`) plus rules and their labels
  (`refs.boxes`). Rule and band labels pick the least-covered of a few spots the same way
  (`coverage`). A new mark kind that text should avoid must push its geometry there too.
- Order in the chart's SVG is the pen's order: `nb-refs` (behind), `nb-plot`, `nb-ref-labels`,
  `nb-notes`. Keep it, and keep `pointer-events: none` on notes so hover and brushing still reach the
  marks.
- Browser tests find gallery charts by cell position. Add new gallery charts at the end of
  `examples/charts.sql`, or the positional checks move.

## Git

Develop on a feature branch and open a pull request against `main`. A pull request builds and tests a
reduced matrix (linux_amd64, windows_amd64 and wasm_eh, plus the `loadable-test` job that loads the
stripped extension into a stock DuckDB and checks the server API); a push to `main` runs the core tests
and DuckDB's extension pipeline on every native platform: Linux amd64 and arm64 (glibc and musl), macOS
amd64 and arm64, Windows amd64 and arm64 (MSVC) and amd64 with MinGW, and the three WebAssembly
targets, whose `wasm_eh` and `wasm_threads` builds the `wasm-test` job loads into DuckDB-WASM. The pipeline runs `test/sql` on most of them (not linux_arm64 or osx_amd64), so
a test must pass on Windows too. A job takes about 5 to 15 minutes because DuckDB comes prebuilt.
