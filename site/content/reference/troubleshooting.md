# Troubleshooting

Start with the notebook itself. These two queries find almost every problem:

```sql
-- Problems in the file, found without running anything
SELECT idx, name, errors, warnings FROM notebook_cells('report.sql')
WHERE len(errors) + len(warnings) > 0;

-- Cells that fail when run
SELECT idx, name, error FROM notebook_run('report.sql') WHERE status = 'error';
```

## Errors in a notebook

The messages below are exactly what `notebook_cells` reports.

| Message | Fix |
|---|---|
| `'b' is defined further down, at line 4; SQL and input cells can only use cells above them` | Move the cell that defines `b` above the one that reads it. Markdown and chart cells may read anything |
| `duplicate cell name 'b'` | Rename one of the cells |
| `hide=yes is not valid; …` | `hide=` takes `code`, `output`, `all` or `none`; `rows=` takes 1 to 10000 |
| `chart cell needs from=<cell or table>` | Add `from=` to the chart cell's `-- %%` line |
| `only a cell that ends in a query can draw a chart; …` | Make the cell's last statement a `SELECT`, or move the chart to a chart cell |
| `unterminated {{ in markdown; …` | Close the expression with `}}` |
| A dependency cycle | Usually a chart drawn from a cell that reads that chart's own selection. Read the selection in a new cell below |
| A query reading its own chart's selection | Put the filtered query in a new cell below |

Functions that need a valid notebook (`notebook_run`, `notebook_sql`, `notebook_export`) stop at the
first error:

```text
Invalid Input Error: Notebook "broken.sql" has errors. First one, at line 1: 'b' is defined further
down, at line 4; SQL and input cells can only use cells above them
List them all with: SELECT start_line, errors FROM notebook_cells('broken.sql') WHERE len(errors) > 0
```

`notebook_cells` never stops: it lists every problem.

## Warnings

| Warning | Meaning |
|---|---|
| `variable 'x' is not set by any cell, so it reads as NULL` | Usually a typo in `$x`. Fine for a chart selection read before any chart sets it |
| A `/* chart */` comment that is not at the end of its cell | It is not drawn. Move it after the last statement |
| SQL the parser cannot read | The cell's dependencies are unknown; running it shows DuckDB's error |

## Running cells

| Symptom | Cause and fix |
|---|---|
| A cell reports a missing parameter for `$x` | The variable is not set. Add an input, or set it with `SET VARIABLE`. Variables nothing sets are set to `NULL` for you; a variable set further down is not |
| A chart selection filters nothing or everything | Guard it: `WHERE $picked IS NULL OR …`. An interval is `$picked.lo` / `$picked.hi`, a point selection is a list |
| A brush on dates compares wrongly | Brushes on `DATE` axes give `TIMESTAMP` bounds. Compare with `BETWEEN` on the column, or cast |
| The file runs as a script but stops at the second cell | Run as plain SQL, cells cannot read each other by name. Use `notebook_sql` to get the expanded script |
| A chart shows ⚠ | The chart's error is in its tab. The cell's table still shows. `notebook_chart_query` shows what the spec compiled to |
| A chart draws a flat colour or one mark | A column name was misspelt and read as a constant. Write `{field: name}` to get an error instead |
| `notebook_run` or `notebook_export` fails with `Query exceeded maximum execution time` | `max_execution_time` bounds the whole call, every cell and query together, whichever cell ran out of time. Raise it (`SET max_execution_time = …`, in milliseconds), or `RESET` it |

## The server and the editor

| Symptom | Cause and fix |
|---|---|
| No browser opens | On Linux without `DISPLAY` or `WAYLAND_DISPLAY` the browser is not opened. Open the URL `notebook_start` returned |
| `Forbidden host` or a refused request | The URL needs its `token=`. Use the full URL `notebook_start` returned, on `127.0.0.1` or `localhost` |
| The server stops right after starting | The DuckDB process exited (for example, the CLI with `-c` exits after the query). Use an interactive shell, and keep it open |
| `notebook_start: cannot listen on 127.0.0.1:8765 (in use?)` | Another server has that port. Stop it, or pass another `port :=`, or leave it out for any free port |
| `port :=` is ignored | A server is already running in this DuckDB process; `notebook_start` reuses it. Call `notebook_stop()` first |
| `notebook_start: the notebook server cannot be started with external access disabled …` or `… while the configuration is locked …` | The database has `enable_external_access = false` or `lock_configuration = true`. Start the editor from a DuckDB without them |
| A select input has no choices, the catalog lacks an attached database, or paging says "run the notebook first" | Nothing has run in this session yet, or the file was reloaded from disk: opening runs no SQL, the `[notebook]` cell's included. Press **Run all** |
| The editor shows an old look after updating the extension | The frontend is compiled into the extension. Restart DuckDB, then reload the page |
| Edits disappeared | They were not saved before the server stopped. <kbd>Ctrl</kbd>+<kbd>S</kbd> saves |

## Installing and loading

| Symptom | Cause and fix |
|---|---|
| `INSTALL sketchbook FROM community` fails | It is not in the community repository yet. See [install](../start/install.md) |
| The extension refuses to load: not signed | Start DuckDB with `-unsigned` (or `allow_unsigned_extensions` in a client) |
| The extension refuses to load: built for another platform | Download the artifact whose name ends in your platform; `PRAGMA platform;` tells you which |
| The extension refuses to load: built for another version | It must match your DuckDB exactly. CI builds are for v2.0.0; a local build is stamped with DuckDB's commit and only loads into the shell built with it |
| The shell exits at once, with an error about another extension | A `~/.duckdbrc` loads something this DuckDB lacks. Start with `-init /dev/null` |
| `$name` is a syntax error | You are on DuckDB before 2.0. Notebooks need 2.0 |

## In DuckDB-WASM

| Symptom | Cause and fix |
|---|---|
| `LOAD` fails: the extension was built for another version or platform | Use DuckDB-WASM 2.0 (npm tag `next`, `1.33.1-dev110.0` or later; `latest` is DuckDB 1.5), the `wasm_eh` build for the `base` bundle and `wasm_threads` for `threads`, stamped `v2.0.0` |
| `LOAD` refuses an unsigned extension | Open the database with `allowUnsignedExtensions: true` ([running in the browser](../guides/browser.md#load-it)) |
| `LOAD` never returns, in the `threads` bundle | A race in DuckDB-WASM's threaded extension loading, about one time in eight, DuckDB's own extensions too. Retry in a fresh page with a timeout, or use the `base` bundle |
| `notebook_start: the notebook server is not available in this build …` | The server needs sockets and threads, which DuckDB-WASM does not have. Use the other functions |
| `notebook_export: format 'pdf' runs a local Chrome, which this build … cannot do …` | Export `html` and print it from the browser |
| `notebook_cache` lists nothing after `cache := true` | DuckDB-WASM's in-memory file system has no directories, so the cache stays off there |

## Exports

| Symptom | Cause and fix |
|---|---|
| `notebook_export: PDF needs Chrome, Chromium or Edge…` | Install one, or pass `chrome := '/path/to/browser'` or set `NOTEBOOK_CHROME` |
| `notebook_export: format 'pdf' runs a browser to print the page, which is not allowed with external access disabled …` or `… while the configuration is locked …` | The database has `enable_external_access = false` or `lock_configuration = true`. Export `html` and print it from a browser |
| html-wasm export asks for a runtime | Build it once with `sh wasm/build.sh` (needs Node and npm) and pass `wasm_runtime := 'wasm/dist'` |
| Inputs do nothing in an exported page | Plain HTML exports freeze inputs. Use `interactive := 'precomputed'` or `format := 'html-wasm'` |
| Some inputs are frozen in a precomputed export | They cannot be enumerated (text, dates, sliders without `step=`, more than 50 values) or did not fit the `budget`. The export's `note` names them |
| A table in an export stops at 100 rows | Exports keep `page_rows` rows per cell (default 100). Raise it, or set `rows=` on the cell |
| `notebook_export: hide must be 'code', 'output', 'all' or 'none', not 'x'` | As it says |

## Stored notebooks

| Symptom | Cause and fix |
|---|---|
| `No notebooks are stored in database "x": there is no _sketchbook.versions table` | Nothing has been saved in that database. Check you opened the right file and `USE`d the right database, or pass `database :=` |
| `"db:x@abc" has version "abc": a version is a whole number from 1 to 2147483647…`, `version 0 is not valid: …` | Versions are numbered from 1. Leave `@…` (or `version :=`) out for the latest |
| `notebook_save: the name "report@2" holds '@', which starts a version…` | A name cannot hold `@`. Pass another `name :=`, such as `'report-2'` |
| `db:x@1 is version 1 of 'x', not the newest, and cannot be saved…` (in the editor) | Saving would make the old version the newest. Open `db:x` to save changes, or write the old version to a file with `notebook_checkout('x', version := 1, file := …)` |
| The editor opened a stored notebook from another database than the one `USE` chose | The page's URL names its database (`&db=…`). Open the URL the latest `notebook_start` returned |
| `notebook_cells: the notebook path must not be NULL` | The path argument was `NULL`, often from a column or variable that is not set |
| `notebook_save: "x.sql" matches version 1 of 'x', but version 2 is newer…` | You are saving an old version over newer work. Check out the latest first, or pass `force := true` |
| `notebook_checkout: "x.sql" has changes that are not saved in 'x'…` | The file has unsaved work. Save it with `notebook_save`, or pass `force := true` |
| Saved notebooks vanished | They were saved in an in-memory database. Open a database file |

Still stuck? Open an [issue](https://github.com/luke-falvey/duckdb-sketchbook/issues) with the output
of the two queries at the top of this page.
