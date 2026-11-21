# SQL functions

Everything the extension does is a table function. Call them with `FROM f(...)`,
`SELECT … FROM f(...)` or `CALL f(...)`. Named arguments use `:=`.

In DuckDB-WASM, `notebook_start`, `notebook_stop` and PDF export are not available and the result
cache keeps nothing; the rest works ([running in the browser](../guides/browser.md)).

Every argument that takes a notebook path also accepts `db:<name>` (the latest stored version) or
`db:<name>@<version>`, for [notebooks stored in the database](../guides/storing.md), read from the
current database (the one `USE` chose). A version is a whole number from 1; anything else after the `@`
(`db:x@abc`, `db:x@0`) is an error, as is a `NULL` path. Paths are read through DuckDB's file system,
so remote paths work when `httpfs` is loaded.

Files the functions write (`notebook_export`, `notebook_checkout`, saving in the editor) are replaced
whole: the text goes to a temporary file beside the target, which is then moved over it, so a failed
write leaves the old file as it was. A symbolic link stays a link: the file it points to is the one
replaced (and must be one `allowed_directories` allows). The new file has the default mode, not the old
one's, and a hard link to the old file keeps the old text. An object store's target (`s3://`, `gs://`,
`az://`, `http(s)://`) is written directly: its upload is already all or nothing. Every other file
system (a network share, `hdfs://`, an extension's) gets the temporary file and the move.

| Function | Does |
|---|---|
| [`notebook_cells(path)`](#notebook_cells) | Parses a notebook: one row per cell, with dependencies and problems |
| [`notebook_run(path)`](#notebook_run) | Runs every cell in a session of its own |
| [`notebook_sql(path)`](#notebook_sql) | The notebook as a plain SQL script |
| [`notebook_export(path)`](#notebook_export) | Writes HTML, html-wasm, PDF, Markdown, SQL or data files |
| [`notebook_chart_query(spec, from)`](#notebook_chart_query) | The SQL and render description a chart spec compiles to |
| [`notebook_peek(sql, cursor)`](#notebook_peek) | The CTE or sub-query at a cursor position, and the query that selects from it |
| [`notebook_cache(path)`](#notebook_cache) | Lists the results the editor keeps for a notebook, fresh, unverified or stale |
| [`notebook_cache_clear(path)`](#notebook_cache_clear) | Removes them |
| [`notebook_save(path)`](#notebook_save) | Stores a version of a notebook in the database |
| [`notebook_checkout(name)`](#notebook_checkout) | Reads a stored version back, optionally to a file |
| [`notebook_start(path)`](#notebook_start) | Starts the editor's server and opens the browser |
| [`notebook_stop()`](#notebook_stop) | Stops the server |

## notebook_cells

```sql
notebook_cells(path VARCHAR)
```

Parses the notebook and analyses its dependencies with DuckDB's parser. Nothing runs. It never fails
on a malformed notebook: problems are returned in `errors` and `warnings`.

| Column | Type | Meaning |
|---|---|---|
| `idx` | `INTEGER` | Position, from 0 |
| `kind` | `VARCHAR` | `notebook`, `markdown`, `input`, `chart` or `sql` |
| `name` | `VARCHAR` | The cell's name, or `NULL` |
| `widget` | `VARCHAR` | An input's widget, or `NULL` |
| `options` | `MAP(VARCHAR, VARCHAR)` | The `key=value` options from the `-- %%` line |
| `body` | `VARCHAR` | The cell's text after the `-- %%` line, chart comments included |
| `content` | `VARCHAR` | For metadata, markdown and chart cells: the text inside the block comment |
| `charts` | `VARCHAR[]` | The chart specs at the end of a query cell |
| `is_query` | `BOOLEAN` | For SQL cells: whether the last statement returns rows |
| `variables` | `VARCHAR[]` | Variables the cell reads |
| `defines` | `VARCHAR[]` | Names the cell defines: relations in lower case, variables with a leading `$` |
| `reads` | `VARCHAR[]` | Names the cell reads, in the same form |
| `depends_on` | `INTEGER[]` | `idx` of the cells it depends on |
| `start_line`, `end_line` | `INTEGER` | Lines of the file the cell spans |
| `errors` | `VARCHAR[]` | Problems that stop the notebook running |
| `warnings` | `VARCHAR[]` | Problems that do not |

```sql
--8<-- "site/outputs/cells_hello.sql"
```

```text
--8<-- "site/outputs/cells_hello.txt"
```

## notebook_run

```sql
notebook_run(path VARCHAR, params := STRUCT, cache := BOOLEAN)
```

Runs every cell, charts included, in a connection of its own, and returns one row per cell in the
order the cells ran. Tables the notebook creates persist; its views and variables do not reach the
caller's session. The whole notebook runs before the first row is returned, so a `LIMIT` or `WHERE`
around the call does not change how many cells run. Databases its `[notebook]` cell attached are
detached when it returns (by `notebook_export` too). The caller's `max_execution_time` bounds the whole
call, `notebook_export` too: every query the notebook runs ends by the call's deadline, whatever a cell
`SET`s, and once the deadline has passed the call fails with *Query exceeded maximum execution time*,
whichever cell ran out of time. An export then writes nothing, except a `data` export whose deadline passes
while its files are written, which leaves those written so far. It holds when `lock_configuration` is set.
An interrupt stops the call before the next cell; a cell already running finishes or reaches the
deadline. See
[running headless](../guides/headless.md).

| Argument | Meaning |
|---|---|
| `params` | A struct of variable values, `{region: 'US', min_amount: 100}`. Replaces input defaults; sets variables nothing sets, such as selections |
| `cache` | `true` writes every cell's result to the [result cache](../guides/cache.md), so the notebook opens in the editor showing them. Default `false`: a run neither reads nor writes the cache |

| Column | Type | Meaning |
|---|---|---|
| `idx`, `kind`, `name` | | As in `notebook_cells` |
| `status` | `VARCHAR` | `ok`, `error`, or `skipped` (nothing to run) |
| `error` | `VARCHAR` | The error message, if any |
| `row_count` | `BIGINT` | Rows a query cell returned, or rows of a chart's first data query |
| `elapsed_ms` | `DOUBLE` | Time the cell took |
| `output` | `VARCHAR` | Markdown cells: the text with each `{{ }}` replaced by its value |

## notebook_sql

```sql
notebook_sql(path VARCHAR)
```

Returns one row with one column, `sql`: the notebook as a plain script. Inputs become their
`SET VARIABLE` defaults, named queries become
`CREATE OR REPLACE TEMP VIEW` (or `TABLE` with `materialize=table`), variables that nothing sets are
initialised to `NULL`, and markdown, chart cells and chart comments are left out. The script runs in any DuckDB 2.0 client
without the extension. Fails on a notebook with errors.

```sql
--8<-- "site/outputs/sql_hello.sql"
```

```sql
--8<-- "site/outputs/sql_hello.txt"
```

## notebook_export

```sql
notebook_export(path VARCHAR, file := …, format := …, hide := …, params := …, interactive := …,
                budget := …, page_rows := …, wasm_runtime := …, chrome := …, data_format := …)
```

Runs the notebook and writes it in another form. Returns one row: `format`, `path`, `bytes` and
`note`.

| Argument | Type | Default | Meaning |
|---|---|---|---|
| `file` | `VARCHAR` | the notebook's path with the format's extension (`.html`, `.md`, `.expanded.sql`, `.pdf`, `_data/`); for `db:<name>[@version]`, `<name>[@version]` and that extension in the working directory | Output path; a folder for `data` |
| `format` | `VARCHAR` | `'html'` | `html`, `html-wasm`, `pdf`, `md`, `sql` or `data` |
| `hide` | `VARCHAR` | `'none'` | `code`, `output`, `all` or `none`, applied to every cell |
| `params` | `STRUCT` | | Variable values, as for `notebook_run` |
| `interactive` | `VARCHAR` | | `'precomputed'`: run every input combination, so inputs work in a static page |
| `budget` | `UBIGINT` | `16` | Most runs `interactive := 'precomputed'` may make |
| `page_rows` | `UBIGINT` | `100` | Result rows kept per query cell (at least the cell's `rows=`) |
| `wasm_runtime` | `VARCHAR` | `$NOTEBOOK_WASM_RUNTIME` | Folder holding the DuckDB-WASM runtime, for `html-wasm` |
| `chrome` | `VARCHAR` | `$NOTEBOOK_CHROME`, the `PATH`, then the usual install places | Browser that prints PDFs. `pdf` is refused when `enable_external_access` is false or `lock_configuration` is set |
| `data_format` | `VARCHAR` | `'parquet'` | `parquet` or `csv`, for `data` |

See [exporting and sharing](../guides/exporting.md).

## notebook_chart_query

```sql
notebook_chart_query(spec VARCHAR, from VARCHAR)
```

Compiles a chart spec against a relation (a table or view name) without drawing it.

| Column | Type | Meaning |
|---|---|---|
| `queries` | `VARCHAR[]` | The SQL queries the chart runs, one per layer or dataset |
| `render` | `VARCHAR` | The render description sent to the browser, as JSON |

The relation's columns are looked up on a separate connection, so a temporary view of your own
session cannot be described; the spec is then compiled without column types. A spec that does not
compile raises an error with the reason.

## notebook_peek

```sql
notebook_peek(sql VARCHAR, cursor BIGINT)
```

Finds the CTE or sub-query that a cursor position (a 0-based byte offset into `sql`) is inside, and
builds the query the editor's [peek](editor.md#peek-under-the-pen) runs to show its first rows. It
runs nothing. One row:

| Column | Type | Meaning |
|---|---|---|
| `kind` | `VARCHAR` | `cte` or `subquery`, or `NULL` when there is nothing to peek at |
| `name` | `VARCHAR` | The CTE's name, or the sub-query's alias |
| `query` | `VARCHAR` | A `SELECT` of the part, with the CTEs it reads |
| `start`, `end` | `BIGINT` | Where the part is in `sql`, as byte offsets |
| `reason` | `VARCHAR` | Why there is nothing to peek at, or why the part is refused |

```sql
--8<-- "site/outputs/peek.sql"
```

```text
--8<-- "site/outputs/peek.txt"
```

## notebook_cache

```sql
notebook_cache(path VARCHAR)
```

Lists the results [kept](../guides/cache.md) for a notebook, and whether each is still fresh against
the notebook as it is now. Runs nothing.

| Column | Type | Meaning |
|---|---|---|
| `cell` | `VARCHAR` | The name the result is kept under: the cell's name, or `@` and a hash of an unnamed cell's text |
| `idx` | `INTEGER` | The cell's position |
| `kind` | `VARCHAR` | The cell's kind |
| `status` | `VARCHAR` | `fresh`; `stale` when the cell, something it reads, a variable it reads or a file it reads changed since; `unverified` when nothing did but the cell reads data the cache cannot check, such as a table no cell creates ([keeping results](../guides/cache.md#what-decides-fresh-or-stale)) |
| `saved_at` | `TIMESTAMP` | When the result was kept, UTC |
| `bytes` | `BIGINT` | Size of the kept result |

```sql
--8<-- "site/outputs/cache.sql"
```

```text
--8<-- "site/outputs/cache.txt"
```

## notebook_cache_clear

```sql
notebook_cache_clear(path VARCHAR)
```

Removes every result kept for the notebook. Returns one row, `removed`: the number of entries
removed. Nothing else is lost: running the notebook keeps them again.

## notebook_save

```sql
notebook_save(path VARCHAR, name := …, message := …, database := …, force := …)
```

Reads the notebook at `path` and stores it as a new version in the `_sketchbook` schema, creating the
schema on first use.

| Argument | Default | Meaning |
|---|---|---|
| `name` | the file name without extension | The stored notebook's name. It must not be blank or hold `@` |
| `message` | | A note stored with the version |
| `database` | the current database | An attached database to store it in |
| `force` | `false` | Save even if the text equals an older version |

| Column | Type | Meaning |
|---|---|---|
| `name` | `VARCHAR` | The stored name |
| `version` | `INTEGER` | The version saved, or the latest one if nothing changed |
| `saved` | `BOOLEAN` | Whether a new version was stored |
| `note` | `VARCHAR` | `first version`, `new version`, or `unchanged since version N` |

`notebook_save` and `notebook_checkout` work on a connection of their own and commit straight away:
they are not part of the caller's transaction, and a `ROLLBACK` does not undo a save. A save reads the
latest version and stores the next one in one transaction, so concurrent saves of the same notebook
get different version numbers.

## notebook_checkout

```sql
notebook_checkout(name VARCHAR, file := …, version := …, database := …, force := …)
```

| Argument | Default | Meaning |
|---|---|---|
| `file` | | Write the text to this path. Without it, the text is only returned |
| `version` | the latest | The version to read: 1 or more |
| `database` | the current database | The attached database to read from |
| `force` | `false` | Overwrite a file whose text is not a stored version of this notebook |

| Column | Type | Meaning |
|---|---|---|
| `name` | `VARCHAR` | The stored name |
| `version` | `INTEGER` | The version read |
| `path` | `VARCHAR` | The file written, or `NULL` |
| `content` | `VARCHAR` | The notebook's text |

## notebook_start

```sql
notebook_start(path VARCHAR, port := …, open := …)
notebook_start(port := …, open := …)
```

Starts the notebook server inside this DuckDB process and returns the editor's URL. The server
listens on 127.0.0.1 only and every request must carry the random token in the URL. It stops when
the database closes or on `notebook_stop()`. Calling it again while it runs keeps the same server
and returns a URL for the new notebook. It is refused when `enable_external_access` is false or
`lock_configuration` is set: the server runs the SQL its page sends and opens a browser. The path is
checked as every function checks it (an empty path or a bad `db:` version is an error). The URL
names the current database (`&db=…`): stored notebooks the page opens are read from, saved to and
cached in it, whatever a later `USE` and `notebook_start` do. A stored notebook in another database is
another notebook, with a session of its own; a file is one notebook however its path is written
(`a.sql`, `./a.sql`, a link to it). The editor does not save an old version (`db:x@1`).

| Argument | Default | Meaning |
|---|---|---|
| `path` | | The notebook to open. Without it, the editor opens with a notebook picker |
| `port` | any free port | The port to listen on, 1 to 65535 (`0` is any free port) |
| `open` | `true` | Open the URL in the default browser (on Linux only with a display) |

| Column | Type | Meaning |
|---|---|---|
| `url` | `VARCHAR` | `http://127.0.0.1:<port>/?token=…`, plus the notebook (`&nb=…`) and the current database (`&db=…`) |
| `port` | `INTEGER` | The port |
| `token` | `VARCHAR` | The token requests must carry |

```sql
FROM notebook_start('report.sql', port := 8765, open := false);
```

## notebook_stop

```sql
notebook_stop()
```

Stops the server. Returns one row, `stopped`: `true` if a server was running.

Edits in the editor that were not saved are lost when the server stops.
