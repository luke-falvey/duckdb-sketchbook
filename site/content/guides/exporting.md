# Exporting and sharing

`notebook_export` runs a notebook and writes the result in a form other people can open without
DuckDB. Every export runs the notebook from the top in a session of its own, so what readers see
is what the file produces now.

```sql
--8<-- "site/outputs/export_html.sql"
```

```text
--8<-- "site/outputs/export_html.txt"
```

It returns one row: the format, the path written, its size in bytes, and a note.

## Formats

| `format :=` | Writes | Default file |
|---|---|---|
| `'html'` (default) | One self-contained page: results, charts, fonts and the hand-drawn renderer inline. Needs no server and no network | `<name>.html` |
| `'html-wasm'` | The same page with DuckDB-WASM and the data inside, so inputs and selections re-run real SQL in the reader's browser | `<name>.html` |
| `'pdf'` | The HTML page printed by headless Chrome, Chromium or Edge | `<name>.pdf` |
| `'md'` | Markdown: prose, code, result tables and chart data as tables | `<name>.md` |
| `'sql'` | The expanded script, the same as `notebook_sql`. Runs in any DuckDB with no extension | `<name>.expanded.sql` |
| `'data'` | A folder with one file per named query cell, Parquet unless `data_format := 'csv'` | `<name>_data/` |

`<name>` is the notebook's path without its extension, so the file goes next to the notebook. For a
stored notebook (`db:<name>` or `db:<name>@<version>`) it is `<name>` or `<name>@<version>` in the
working directory: `db:revenue@3` writes `./revenue@3.html`. Pass `file :=` to choose the path.

The editor's ⚙ menu exports too: pick a format and tick **code** and **output** to choose what to
include. Exports from the editor use the text in the editor, saved or not.

## See them

These exports of the repository's examples were made by this build and are published with the site:

- [revenue.html](../examples/revenue.html): `examples/revenue.sql`, inputs precomputed.
- [charts.html](../examples/charts.html): `examples/charts.sql`, every chart type.


## Choosing what readers see

```sql
FROM notebook_export('report.sql', file := 'report.html', hide := 'code');
```

`hide :=` takes `'code'`, `'output'`, `'all'` or `'none'` and applies to every cell, on top of each
cell's own `hide=`. Prose and inputs are always kept. A results-only report is `hide := 'code'`.

Add `#present` to an exported page's address to [present](reading.md#presenting) it one screen
at a time.
Its **wide** control, or `?width=wide` in the address, lets it use the whole window
([wide pages](reading.md#wide-pages)). A PDF is always printed at the normal width.

Tables keep `page_rows` rows per query cell (default 100, and at least the cell's `rows=`), and the
page lets readers page through them.

## Running with other values

```sql
FROM notebook_export('examples/revenue.sql', file := 'revenue_us.html', params := {region: 'US'});
```

`params :=` works as in [`notebook_run`](headless.md): it replaces input defaults and sets
selections.

## Inputs that still work

A plain HTML export shows inputs frozen at the values it ran with. Two ways to keep them working:

**Precomputed.** `interactive := 'precomputed'` runs the notebook once for every combination of input
values and puts every result in the page. The inputs then switch between them, with no DuckDB
involved.

```sql
--8<-- "site/outputs/export_precomputed.sql"
```

```text
--8<-- "site/outputs/export_precomputed.txt"
```

Only some inputs can be enumerated: a `select` (up to 50 choices), a `checkbox`, and a `slider` or
`number` with `step=` and at most 50 steps. The number of runs is capped by `budget :=` (default 16);
inputs that do not fit are frozen, and the note names them. In the example, the slider's 51 steps do
not fit, so only the region switches.

**Live, with DuckDB-WASM.** `format := 'html-wasm'` embeds DuckDB-WASM and the notebook's data, so
every input and chart selection runs real SQL in the reader's browser. It needs a runtime built once
from the repository with Node and npm:

```sh
sh wasm/build.sh            # writes wasm/dist
```

```sql
FROM notebook_export('examples/revenue.sql', format := 'html-wasm', wasm_runtime := 'wasm/dist');
```

You can set `NOTEBOOK_WASM_RUNTIME` instead of passing `wasm_runtime :=`. Things to know:

- The page carries DuckDB-WASM, compressed: the revenue example comes to 12.5 MB, and takes a few
  seconds to start.
- The tables the notebook creates are copied into the page. Keep them small.
- DuckDB-WASM is version 1.5, a release behind: `$name` is rewritten to `getvariable('name')` for it.
- It works offline and opened straight from disk (`file://`).

## PDF

```sql
FROM notebook_export('report.sql', format := 'pdf');
```

The PDF is the HTML page printed by a Chromium-based browser in headless mode. The extension looks,
in order, at `chrome :=`, the `NOTEBOOK_CHROME` environment variable, the `PATH` (`chromium`,
`google-chrome`, `microsoft-edge`, `chrome`, `chrome-headless-shell` and the like; `chrome.exe` and
`msedge.exe` on Windows), and then the usual install places: `Program Files` and `AppData\Local` on
Windows, `/Applications` on macOS. So on most machines with Chrome or Edge installed it needs no
setting. Handwriting is off in PDFs. Printing starts another program, so it is refused when
`enable_external_access` is false or `lock_configuration` is set. The PDF is printed to a file of its
own and moved over `file :=` once printed, so a failed print leaves an older PDF as it was. On Windows the output path cannot contain `%`, `#` or `?`, which
the command line that starts the browser cannot pass safely.

## Markdown

`format := 'md'` writes prose, code, result tables and each chart's data as a table, for a pull
request, a wiki or an LLM. The start of the revenue example's export:

````markdown
--8<-- "site/outputs/revenue_export.md::40"
````

## Data

```sql
--8<-- "site/outputs/export_data.sql"
```

```text
--8<-- "site/outputs/export_data.txt"
```

One Parquet file per named query cell, named after the cell. `data_format := 'csv'` writes CSV.

## Arguments

| Argument | Default | Meaning |
|---|---|---|
| `file` | next to the notebook; for `db:` notebooks, in the working directory ([formats](#formats)) | Output path. For `data`, a folder |
| `format` | `'html'` | `html`, `html-wasm`, `pdf`, `md`, `sql`, `data` |
| `hide` | `'none'` | `code`, `output`, `all` or `none`, for every cell |
| `params` | | A struct of variable values: `{region: 'US'}` |
| `interactive` | | `'precomputed'`: precompute input combinations |
| `budget` | `16` | Most runs `interactive := 'precomputed'` may make |
| `page_rows` | `100` | Rows kept per query cell |
| `wasm_runtime` | `NOTEBOOK_WASM_RUNTIME` | Folder of the DuckDB-WASM runtime, for `html-wasm` |
| `chrome` | `NOTEBOOK_CHROME`, the `PATH`, then the usual install places | Browser for `pdf` |
| `data_format` | `'parquet'` | `parquet` or `csv`, for `data` |

`to` is a reserved word in DuckDB, which is why the output path is `file :=`.
