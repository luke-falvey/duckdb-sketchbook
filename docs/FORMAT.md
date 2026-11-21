# Notebook format, version 1 (draft)

A notebook is a UTF-8 SQL file. Every byte of it is either a DuckDB statement or a SQL comment, so
editors, formatters, linters and DuckDB itself treat it as ordinary SQL. The notebook structure lives
in comments.

The file holds source only. Results, chart images and UI state are never written to it.

See [`examples/revenue.sql`](../examples/revenue.sql) for a complete notebook.

## 1. Cells

A line that starts with `-- %%` begins a cell. The cell runs to the next marker or the end of the file.
Text before the first marker is ignored by the notebook (use it for a licence header).

```
-- %% <head>
<body>
```

The first token of the head decides the cell kind:

| Head starts with | Kind | Body |
|---|---|---|
| `[notebook]` | Notebook metadata (at most one, first cell) | One block comment containing YAML |
| `[markdown]` | Prose | One block comment containing Markdown |
| `input` | Input widget | One `SET VARIABLE` statement |
| `chart` | Visualisation of another cell or table | One block comment containing a YAML chart spec |
| anything else, or nothing | SQL | DuckDB statements, optionally followed by `/* chart */` comments (§2) |

After the kind comes an optional name and then `key=value` options. Quote values that contain spaces:
`label="Minimum order value"`.

```sql
-- %% monthly                       SQL cell named "monthly"
-- %%                               unnamed SQL cell
-- %% orders run=manual             SQL cell with an option
-- %% input region select from=regions.name
-- %% chart revenue_trend from=monthly
```

### Names

- A name matches `[a-z_][a-z0-9_]*`. `input` and `chart` are reserved.
- Names are unique within a notebook. A duplicate is an error reported on both cells.
- **The name is the cell's identity.** There are no generated IDs in the file. Caches, layout state and
  links between cells all key on the name. Unnamed cells are identified by position and so lose their
  cached output when cells above them are added or removed; name any cell you care about.
- Renaming a cell is an ordinary edit. The UI offers "rename and update references".

### Options common to all cells

| Option | Values | Meaning |
|---|---|---|
| `hide` | `code`, `output`, `all`, `none` | What to hide in reading view and exports. The editor collapses the hidden part to a line that shows it on click |
| `span` | `1`–`12` | Width on a 12-column grid in reading view and exports; consecutive cells whose spans fit share a row. Default `12` |
| `rows` | `1`–`10000` | Rows per page of the cell's result table, in the editor and in exports. Default: the reader's own setting, 25 unless changed |
| `screen` | `new`, `join`, `skip` | Where the cell goes when the notebook is presented (see below). Default: the reading layout decides |

`hide`, `rows` and `screen` are checked: any other value is an error on the cell. In the editor, the
code and output switches in a cell's header and the page-size picker under a table write these
options into the marker line, so the choice is saved with the notebook and reviewed like any edit.

### Presenting

A notebook can be presented one screen at a time, from the editor's gear menu or from an HTML export
(open it with `#present` at the end of its address, or `#present=3` for the third screen). Code is
never shown; what a screen holds follows from the reading layout, so a notebook that reads well
presents well without any markup:

- **A screen is a row of the reading layout**: one cell, or the consecutive cells whose `span`s share a
  row (two `span=6` charts, three `span=4` KPIs).
- **An input is shown with what follows it**: input cells share the screen of the next cell, so the
  audience sees what the input changes.
- Cells that show nothing when reading have no screen: `hide=all`, `hide=output`, the `[notebook]`
  cell, and SQL cells that return no rows.

`screen=` on a cell overrides this:

| Value | Meaning |
|---|---|
| `screen=new` | The cell starts a new screen, even if it would share a row or follow an input |
| `screen=join` | The cell stays on the screen of the cell before it: a note under its chart, a table under its KPIs |
| `screen=skip` | The cell is left out of the presentation, though it still shows when reading and in exports |

```sql
-- %% chart trend from=monthly
-- %% [markdown] screen=join
/* What the trend shows, on the same screen as the chart. */
-- %% detail rows=10 screen=skip
SELECT * FROM monthly;
```

A screen is laid out as in the reading view, no wider than what it holds, centred, and scaled up to
fill the window. One that is too tall is
scaled back down, to no less than four fifths of the size of the reading view; past that it scrolls. `screen` changes
nothing but presenting.

## 2. SQL cells

The body is one or more DuckDB statements.

**Query cells.** If the cell is named and its last statement returns rows (`SELECT`, `FROM …`,
`VALUES`, `PIVOT`, `SUMMARIZE`, `DESCRIBE`, …), the runner registers that statement as a temporary view
with the cell's name and displays `SELECT * FROM <name>`. Later cells refer to the result by name:

```sql
-- %% filtered
SELECT * FROM orders WHERE region = $region;

-- %% monthly
SELECT date_trunc('month', ts) AS month, sum(amount) AS revenue
FROM filtered
GROUP BY ALL;
```

The file keeps the bare `SELECT`. The view wrapper is added at run time.

**Charts.** A query cell draws its result as a table. To chart it instead, end the cell with one or
more block comments that start with `chart`, each holding a chart spec (§5):

```sql
-- %% monthly
SELECT date_trunc('month', ts) AS month, category, sum(amount) AS revenue
FROM filtered
GROUP BY ALL;
/* chart
title: Monthly revenue
mark: line
x: month
y: revenue
color: category
*/
/* chart
mark: bar
x: {field: revenue, agg: sum}
y: category
*/
```

The query defines the data and the charts are views of it: a reader sees one tab per chart, in file
order, and a last tab with the table. The first chart is shown until another tab is picked. A chart
comment is a comment like any other, so the file still runs as plain SQL, and the expanded script (§7)
leaves the charts out.

- Only comments after the cell's last statement are charts. A `/* chart */` comment before or inside
  the query is an ordinary comment, with a warning.
- Only a cell whose last statement returns rows can have charts. Its name is optional: the charts of an
  unnamed cell read its query directly.
- The spec is the comment's text after `chart`, on the following lines or on the same line
  (`/* chart mark: bar */`). Leading blank lines and trailing whitespace are dropped.
- The cell's `width` and `height` options size all of its charts.
- A chart's `select` variable is defined by its cell (§5.2). The cell's own SQL cannot read it: that
  would make the query depend on a chart of itself. Its charts may `filter` on it, and on any other
  selection; those filters redraw the charts without running the query.
- Changing a spec redraws that cell's charts; the query does not run again.

A chart cell (§5) draws a chart of another cell or of a table, for when one result feeds several
places or a chart reads a table that no cell defines.

**Effect cells.** A cell containing anything other than queries (`CREATE`, `INSERT`, `UPDATE`,
`DELETE`, `ATTACH`, `COPY`, `INSTALL`, `LOAD`, `SET`, …) is an effect cell. It defines whatever its
statements create. Effect cells run only when asked (see [DESIGN.md §3](DESIGN.md#3-execution-model)).

| Option | Values | Meaning |
|---|---|---|
| `run` | `auto`, `manual` | Default `auto` for query cells, `manual` for effect cells |
| `materialize` | `view`, `table` | `view` (default) keeps the result lazy. `table` stores it as a temp table: use for slow queries that many cells read. The editor keeps the table between requests and computes it again only when the cell runs |

### Ordering

SQL and input cells may only refer to cells **above** them. A forward reference is an error. This keeps
the file readable top to bottom and makes the expanded script (§7) a straight translation. Markdown and
chart cells only display things, so they may refer to any cell.

What a cell defines and reads is found by DuckDB's parser, without running anything:

| Cell | Defines | Reads |
|---|---|---|
| SQL | Its name, if it is a named query cell; tables, views, schemas, macros, sequences and types it creates; databases it attaches; variables it sets; its charts' `select: {as: …}` variables | Tables and views in its SQL that are not CTEs, including the targets of `INSERT`, `UPDATE`, `DELETE`, `COPY` and `DROP`; `$name` and `getvariable('name')`; the selections in its charts' `filter`, which add no dependency |
| Input | Its variable | The cell in `from=<cell>.<column>`; anything its default reads |
| Markdown | Nothing | What its `{{ }}` expressions read |
| Chart | Its `select: {as: …}` variable | Its `from`; the selections in its `filter` |

Names compare case-insensitively, and `main.` and `temp.` qualifiers are ignored, so `main.orders`
reads the cell that creates `orders`. A qualified name such as `wh.orders` depends on the cell that
creates it or, failing that, on the cell that attached `wh` or created schema `wh`. A name that no cell
defines is assumed to be a table or view in the database and needs no cell.

These are errors, reported on every cell involved:

- A name defined by more than one cell.
- An SQL or input cell reading something defined further down. A query cell's chart filters are
  exempt: a selection may come from any chart.
- A dependency cycle. With the rule above, a cycle can only pass through a chart, for example a chart
  drawn `from` a cell that reads the chart's own selection.
- A query cell whose SQL reads the selection of one of its own charts.
- Chart comments on a cell whose last statement does not return rows.

These are warnings, which do not stop a notebook from running:

- A variable that some cell reads but no cell sets. It reads as `NULL` (see Parameters).
- A `/* chart */` comment that is not at the end of its cell, which is not drawn.
- SQL the parser cannot read, such as a syntax error. Its dependencies are unknown, and running it
  reports the error on that cell.

Calls to macros, and uses of sequences and types, are not yet counted as reads.

### Parameters

Parameters are DuckDB session variables, read with `$name`:

```sql
WHERE region = $region AND amount >= $min_amount
```

No notebook-specific syntax is involved. `$name` is DuckDB's own shorthand for `getvariable('name')`,
[introduced in DuckDB 2.0](https://duckdb.org/docs/2.0/sql/statements/set_variable.html), so notebooks
need DuckDB 2.0 or later. The two differ when the variable is unset: `getvariable` returns `NULL`,
while `$name` raises a missing-parameter error.

To keep that error away from cells that read a chart selection nobody has made yet, the runner sets
every variable that some cell reads, but no input cell or `SET VARIABLE` statement defines, to `NULL`
before the first cell runs. Views pick up a changed variable on their next
read, so changing an input does not require redefining the cells that use it.

## 3. Input cells

```sql
-- %% input <name> <widget> [options]
SET VARIABLE <name> = <default>;
```

The body is a real statement that sets the default, so the file still works outside the notebook. When
someone moves the widget, the runner sets the variable for that session and re-runs the cells that read
it. Moving a widget never changes the file: the default is the `SET VARIABLE` statement in the body,
edited like any other text.

| Widget | Variable type | Options |
|---|---|---|
| `text` | `VARCHAR` | `placeholder` |
| `number` | numeric | `min`, `max`, `step` |
| `slider` | numeric | `min`, `max`, `step` |
| `checkbox` | `BOOLEAN` | |
| `date` | `DATE` | `min`, `max` |
| `daterange` | `STRUCT(lo DATE, hi DATE)` | `min`, `max` |
| `select` | scalar | `options=a,b,c` or `from=<cell>.<column>` |
| `multiselect` | `LIST` | `options=…` or `from=<cell>.<column>` |

All widgets accept `label`. `from=` makes the input depend on that cell.

A value set through a widget gets the widget's type: `VARCHAR` for `text`, `DOUBLE` for `number` and
`slider`, `BOOLEAN` for `checkbox`, `DATE` for `date` and for both fields of `daterange`
(`STRUCT(lo DATE, hi DATE)`). A `select` or `multiselect` with `from=` takes the type of that column
(as a list for `multiselect`); with `options=` the values are text.

## 4. Markdown cells

```sql
-- %% [markdown]
/*
# Heading

Total revenue in **{{ $region }}**: {{ (SELECT sum(revenue) FROM monthly) }}
*/
```

- CommonMark plus tables, footnotes, KaTeX maths (`$…$` / `$$…$$` are maths here, not parameters) and
  Mermaid diagrams in ```` ```mermaid ```` fences. The built-in renderer covers headings, emphasis,
  code, links, lists, quotes, rules, tables and fenced code. No KaTeX or Mermaid library is bundled
  yet: maths is shown as written, Mermaid fences as code, and footnotes as plain text.
- `{{ expr }}` is a DuckDB scalar expression. It is evaluated as `SELECT (expr)` and the value is
  formatted inline (`NULL` shows as `NULL`). Dependencies are found the same way as for SQL cells, and
  the expressions are evaluated once the cells they read have run, so a summary may sit above them.
  The closing `}}` is looked for in SQL, so `}}` inside a string or a struct literal does not end the
  expression. An unclosed `{{` or an empty `{{ }}` is an error.
- Raw HTML is escaped and shown as text. Scripts never run.
- DuckDB block comments nest, so `/*` and `*/` inside the prose must be balanced. Write a literal
  `*/` as `*\/`.

## 5. Charts

A chart spec is written in one of two places:

```sql
-- %% monthly
SELECT ...;
/* chart
<YAML spec>
*/

-- %% chart [name] from=<relation> [options]
/*
<YAML spec>
*/
```

A spec (and the `[notebook]` cell's YAML, §6) nests at most 256 levels deep; a deeper one is a format
error.

At the end of a query cell (§2) the chart draws that cell's result. A chart cell draws `from`, which
names a cell, table or view. The spec says how to turn that relation into a picture. The runner
compiles each spec into one aggregate SQL query over the relation plus a small render description, so
the browser receives only the rows the picture needs. Below, `from` means the relation a chart draws,
whichever way it is written.

### 5.1 Grammar charts

For anything that is marks on axes:

```yaml
title: Monthly revenue by category
mark: line
x: month
y: {field: revenue, agg: sum, format: "$,.0f", title: Revenue}
color: category
```

**`mark`**: `line`, `area`, `bar`, `point` (or `circle`, `square`), `rect`, `rule`, `tick`, `text`,
`arc`, `boxplot`, `histogram`, `density`, `hexbin`.

**Mark options**, beside `mark` in a spec or a layer: `opacity` (a number), `strokeWidth` (lines),
`point: true` (dots on every point of a line) and `innerRadius` (an `arc` drawn as a donut, in pixels).
Of Vega-Lite's options, those that match what is drawn are accepted: `stack: true` or `zero` on bars
(bars coloured by a group are always stacked) and `interpolate: linear` on lines. Any other `stack` or
`interpolate`, and `cornerRadius`, are an error. In a `layers:` chart, an option at the top applies to
every layer.

**Channels**: `x`, `y`, `x2`, `y2`, `theta`, `color`, `size`, `opacity`, `shape`, `text`, `tooltip`, `order`,
and `facet` (`{row: …, col: …}`) for small multiples. A channel is a column name or a mapping:

| Key | Meaning |
|---|---|
| `field` | Column in `from` |
| `agg` (or `aggregate`) | `sum`, `avg` (or `mean`), `min`, `max`, `count`, `count_distinct` (or `distinct`), `median`, `p05`…`p99` |
| `bin` | `true` or `{step: …}` / `{maxbins: …}` |
| `timeunit` | Truncates: `year`, `quarter`, `month`, `week`, `date`, `hour`, `minute`, `second`. Extracts a cyclic part: `monthofyear`, `dayofmonth`, `dayofweek` (or `day`, 0 = Sunday), `hours` (or `hourofday`), `minutes` |
| `sort` | `asc`, `desc`, or a channel with optional `-` (`-x`) |
| `type` | `quantitative`, `temporal`, `ordinal`, `nominal`. Inferred from the column type if omitted |
| `scale` | `{type: log}`, `{domain: [0, 100]}`, `{zero: true}` (include zero; bars and areas always do unless `{zero: false}`). On a categorical `color`: `{domain: [PASS, WARNING, ERROR], range: ["#2f9e44", "#f08c00", "#e03131"]}` fixes the order and colour of each value |
| `scheme` | Named colour scheme |
| `format`, `title` | d3-format string; axis or legend title |

Channels without `agg` become `GROUP BY` keys when any channel has one. A bare name that is not a
column of `from` is a constant (`color: red`); `{value: …}` says so explicitly. `arc` (pie and donut)
takes its angle from `theta` (or `y`) and its slices from `color`.

Spec-level keys: `title`; `limit` (rows a query may return, default 10,000); `sample: <n>`;
`facet: {row: …, col: …}`; `rules`, `bands` and `notes` (below).

**Layers.** `layers:` is a list of specs drawn in one frame, sharing `from` and scales:

```yaml
layers:
  - {mark: area, x: month, y: {field: revenue, agg: sum}, opacity: 0.3}
  - {mark: rule, y: {field: revenue, agg: avg}, color: red}
```

**Rules and bands.** `rules:` draws reference lines and `bands:` shades ranges, behind the marks in
muted ink:

```yaml
mark: line
x: month
y: {field: revenue, agg: sum, format: "$,.0f"}
rules:
  - {y: avg, label: "average {value}"}
  - {y: 150000, label: Target}
  - {x: 2026-06-01, label: Launch}
bands:
  - {y: [p25, p75], label: Middle half}
```

Each item names one axis, `x` or `y`. A rule takes one value there and a band takes two, `[from, to]`.
A value is:

| Written | Meaning |
|---|---|
| A number, date or category (`150000`, `2026-06-01`, `Hardware`) | That position on the axis. On a category axis a rule runs through the category and a band covers from one category to the other |
| `avg` (or `mean`), `median`, `min`, `max`, `p05`…`p99` | That aggregate of the values the chart draws on the axis, across all series and panels: with `y: {field: revenue, agg: sum}` over months, `avg` is the average monthly sum. Needs a numeric or date axis; dates have no `avg` |
| `{field: budget, agg: max}` | An aggregate of a column of `from`, with any channel `agg` |

Aggregates are computed by DuckDB in one extra query, over the marks before any downsampling.
Optional keys: `label` (short text by the line or in the band; `{value}`, or `{from}` and `{to}` in a
band, is replaced by the value in the axis's format) and `color`. The axis grows to include a rule or
band outside the data. Quote a category that reads as an aggregate (`x: "max"`). `rules` and `bands`
are read from the top level of the spec only and need a chart with axes (not `arc`); `boxplot` takes
values but not aggregates. Any other key or shape is a chart error, and so are `rules`, `bands` and
`notes` on a typed chart (§5.3), apart from the `bands` of a `kpi` or `gauge`, which colour its value.

**Notes.** `notes:` writes short remarks on the chart, each attached to a data position, so they stay
with the data when the chart is resized or its query returns other rows:

```yaml
notes:
  - {x: 2026-02-01, text: "February is a short month", circle: true}
  - {x: 2026-06-01, y: 182000, text: Launch week}
  - {x: [2026-05-01, 2026-08-01], text: steady summer}
  - {y: Hardware, text: new supplier, side: right}
```

| Key | Meaning |
|---|---|
| `x`, `y` | The position, in the values of each axis. With only one, the note points at the mark there: the top of the bar or the highest line at that `x`, the end of the bar at that `y`. One of them may be a range, `[from, to]`, which is drawn as a bracket over the marks in it |
| `text` | A few words. Handwriting is wide: it is wrapped at about 18 characters and cut after four lines. Required unless the note is a `circle` or a range |
| `circle` | `true` draws a hand-drawn ring round the point |
| `side` | `above`, `below`, `left` or `right`: where the text goes. Without it the renderer picks the side with most room, clear of the marks, rules and other notes |
| `dx`, `dy` | Pixels to move the text from where it was placed (right and down are positive); the arrow follows |
| `color` | Ink colour of the note |

A note is text with a curved arrow to its point. Notes are drawn, and written by the pen, after the
marks; they appear in every export; the pointer passes through them to the marks. A position outside
the axes is drawn at the edge, and a category the data does not have draws nothing. In a faceted
chart a note is drawn in every panel. `notes` is read from the top level of the spec only and needs a
chart with axes. Positions are values, not aggregates. Any other key or shape is a chart error.

**Large data.** The compiler never ships raw rows for a mark that would overplot: `line` over a
continuous x is reduced to at most four points per horizontal pixel (M4) once `from` has more than four
rows per pixel, and `point`, `rect` and `tick` over two numeric axes are binned to a grid of 4-pixel
cells once `from` has more than 5,000 rows (`hexbin` always is), each cell drawn by its row count.
`histogram` bins `x` and counts; `density` bins `x` and normalises the counts. `sample: 5000` asks for
a row sample instead.

### 5.2 Selections

```yaml
select: {as: picked_months, type: interval, on: x}
```

A selection writes the user's brush or click into the session variable named by `as`. Cells that read
`$picked_months` re-run. With no selection the variable is `NULL`.

| `type` | Variable value |
|---|---|
| `interval` on one channel | `STRUCT(lo, hi)` |
| `interval` on `[x, y]` | `STRUCT(x STRUCT(lo, hi), y STRUCT(lo, hi))` |
| `point` | `LIST` of the selected values of the `on` channel's field |

```sql
WHERE $picked_months IS NULL
   OR ts BETWEEN $picked_months.lo AND $picked_months.hi
```

A chart can take the filter without any SQL: `filter: picked_months` in another chart's spec applies
the selection to that chart's `from` (cross-filtering), matching the selection's fields by name.
`filter` takes one selection or a list, `[a, b]`. `select` and `filter` are read from the top level of
the spec only.

The values have the type of the selected field. A brush on a `DATE` field gives `TIMESTAMP` bounds,
because a brush ends between days. In the UI, dragging on a chart with an `interval` selection
brushes, a click clears it; clicking a mark of a `point` selection selects it (Shift adds to it).

### 5.3 Typed charts

Charts that are not marks on axes use `type:` with their own short vocabulary:

| `type` | Required keys | Notes |
|---|---|---|
| `kpi` | `value` | `label`, `compare` (delta against another channel), `sparkline: {x, y}`, `bands: [{from, to, color}]` (colours the value by the band it falls in; ends inclusive, either may be left out) |
| `table` | | `columns`, per-column `format: {col: spec}`, `bar` and `heat` column decorations, `tip: {col: source}` (hovering a cell of `col` shows that row's value of `source`; a list shows several, one per line. `link: {col: "https://…/{col}"}` (each cell of `col` links to the URL with every `{name}` filled in from the row, URL-encoded; a column name instead of a URL links to the URL it holds; a list value links each item; only `http(s)` and `mailto`). Columns used only by `tip` or `link` are not shown unless `columns` lists them) |
| `pivot` | `rows`, `columns`, `values` | `heat: true` shading |
| `map` | `geometry` or `lat` + `lon` | `layer: point \| heat \| hex \| arc` (`arc` needs `lat2`, `lon2`), `color`, `size`. `geometry` needs the `spatial` extension; polygons draw as a choropleth. No basemap: tiles are not available offline |
| `sankey` | `source`, `target`, `value` | |
| `chord` | `source`, `target`, `value` | |
| `treemap`, `sunburst`, `icicle` | `path` (list of columns), `value` | |
| `network` | `source`, `target` | `weight` |
| `funnel` | `stage`, `value` | |
| `candlestick` | `x`, `open`, `high`, `low`, `close` | |
| `calendar` | `date`, `value` | Calendar heatmap |
| `gantt` | `task`, `start`, `end` | |
| `gauge` | `value` | `min`, `max`, `bands: [{from, to, color}]`, `label` |

```yaml
type: kpi
value: {field: revenue, agg: sum, format: "$,.0f"}
label: Revenue
sparkline: {x: month, y: revenue}
```

```yaml
type: map
geometry: geom
color: {field: population, scheme: viridis}
```

### 5.4 Raw specs

When the built-in vocabulary runs out, embed a native spec. The rows of `from` are supplied as the
dataset (up to `limit`, default 5,000); no pushdown or downsampling is applied, so aggregate in SQL
first. No Vega or ECharts library is bundled: a Vega-Lite spec's `mark` and `encoding` (`x`, `y`,
`color`, `size`, `text`, `theta`) are drawn by the built-in renderer, as is an ECharts `series` of
type `line`, `bar`, `scatter` or `pie` with `encode`; anything else is shown as a table.

```yaml
type: vega-lite
spec:
  mark: {type: bar, cornerRadius: 4}
  encoding:
    x: {field: category, type: nominal}
    y: {field: revenue, type: quantitative}
```

`type: echarts` works the same way with an ECharts `option` object under `spec`.

## 6. Notebook metadata

```sql
-- %% [notebook]
/*
title: Revenue review
format: 1
attach:
  - {path: warehouse.duckdb, as: wh, read_only: true}
extensions: [spatial]
timing: false
cache: false
width: wide
*/
```

`format` is the version of this specification. `timing` (`true` by default) shows how long each cell
took to run, in the bottom right corner of its box in the editor; `timing: false` hides it. Any other
value is an error. The editor's gear menu has a switch that writes this line. `cache` (`true` by
default) keeps each cell's last result for the editor to show when the notebook is next opened
(§8); `cache: false` keeps none and removes what was kept. Any other value is an error, and the gear
menu's *keep results* switch writes this line. `width` (`normal` by default) is how wide the page is for a reader who has not chosen:
`normal` keeps the cells in a reading column, `wide` lets cells, tables and charts use the width of the
window. Any other value is an error. A reader's own choice (the editor's gear menu, an export's *wide*
control, or `?width=wide` / `?width=normal` in the address) wins over it; presenting, print and PDF
always use the normal width. A chart cell or query with `width=` keeps that width when the page is
wide; the others are drawn at the width they have. `attach` and `extensions` are applied before the first
cell runs (`LOAD` for each extension, `ATTACH` for each database). Paths are relative to the notebook
file. A database already attached under the same name is used if it is the same path in the same mode
(`read_only`), and is an error otherwise. `notebook_run` detaches the databases it attached when it
returns; the editor keeps them attached while it runs.

## 7. Running without the notebook

Four ways, from least to most capable:

1. **The file as-is in any DuckDB client.** It always parses. It runs to completion only if cells do
   not refer to each other by cell name, because the view wrapper is added by the runner. Notebooks
   written in "explicit" style (`CREATE OR REPLACE TEMP VIEW monthly AS …` in the cell body) run
   unchanged.
2. **The expanded script.** `SELECT sql FROM notebook_sql('revenue.sql')` returns the same notebook
   with the wrappers spelled out and undefined variables initialised. It runs anywhere with no
   extension. [`examples/revenue.expanded.sql`](../examples/revenue.expanded.sql) is that output for
   the example.
3. **Headless with the extension.** `FROM notebook_run('revenue.sql')` runs every cell in a session
   of its own and returns one row per cell with its status, in the order the cells ran. Tables the
   notebook creates remain; its views and variables do not reach the caller's connection. Markdown
   cells come back rendered, with each `{{ }}` replaced by its value.
   `notebook_run('revenue.sql', params := {region: 'US'})` runs with other values: a parameter
   replaces an input's default, or sets a variable that nothing in the notebook sets, such as a chart
   selection. A variable set by an SQL cell cannot be a parameter, because the cell would overwrite it.
4. **Exported.** `notebook_export('revenue.sql', file := 'revenue.html')` writes a page that needs
   neither DuckDB nor a server; see [DESIGN.md §7](DESIGN.md#7-export) for the formats.
   `hide := 'code'` (or `'output'`, `'all'`) hides that part of every cell on top of each cell's own
   `hide=`; prose and inputs are always kept. An exported table pages through the rows the export
   includes (`page_rows`, at least each cell's `rows=`).

## 8. Sidecar cache

The editor keeps the last result of every cell, so a notebook reopens showing its output at once,
without running anything. The cache is disposable: deleting it loses nothing but time, and the
notebook file is never touched by it.

**Where it lives.** For a notebook file, in `.duckdb-sketchbook/` beside it, one directory per notebook
(the file name without its extension). The extension writes a `.gitignore` there that ignores the
whole directory, so it never needs adding to a project's own:

```
.duckdb-sketchbook/
  .gitignore                     *
  revenue/
    monthly.json                 a named cell's result
    @3f1c0a9d2e4b7c10.json       an unnamed cell's result, by a hash of its text
    @state.json                  the variables the results were made with
```

For a notebook stored in a database (`db:<name>`), in that database's `_sketchbook.cache` table (the
database it is read from: the current one, which `USE` chooses, for the SQL functions; for the editor, the
one `notebook_start` named in the page's URL, its current database),
`(name, cell, format, key, data, saved_at, result)` with one row per cell and one for `@state`. A notebook
that DuckDB's file system does not have on the local file system (a URL, an object store, `hdfs://`, any
file system an extension adds) and a notebook not saved yet have no cache. A location that cannot be
written means no cache, quietly; a run never fails because of it.

**Caches from before the rename.** The extension was called `notebook` before it became `sketchbook`,
and kept its cache in `.duckdb-notebook/` and `_notebook.cache`. When the new place has nothing for a
notebook, those are read instead; `notebook_cache_clear` clears both. Nothing is written there. Likewise,
a stored notebook's cache was kept in the default database before it followed `USE`: when the current
database has nothing for it and the default database stores no notebook of that name (whose cache it would
be), the default database's `_sketchbook.cache` (then `_notebook.cache`) is read, and cleared with it,
never written.

**An entry** is JSON: `{format, cell, key, data, saved_at, result}`. `key` and `data` are the cell's keys when the
result was made (a redraw of its charts may refresh it only while `data` still matches). `result` is exactly
what the editor was sent when the cell ran: its status and error, row count and timing, the rendered prose, the first page
of a query's rows (as many as the page shows), each chart's render description and data, an input's
value and choices. Rows and chart data are JSON, as everywhere else in the editor (DESIGN §12), not
Parquet or Arrow. A result larger than **4 MiB** of JSON is not kept: that cell reopens empty, as
every cell did before there was a cache. A chart's data is already bounded by the chart compiler's
aggregation, and a grid keeps one page, so in practice only very wide pages or point clouds reach it.

**Identity.** A named cell's result is stored under its name; an unnamed cell's under `@` and a hash
of its marker line and body, so it keeps its result when cells around it move and loses it when it is
edited. This is why naming cells you care about matters (§2).

**The key.** Every entry records the key of the cell when its result was made. The key is a hash of:

- the cell's kind, name, widget and SQL (or text), and its options except the ones that only lay it
  out (`hide`, `span`, `run`, `screen`);
- the keys of the cells it reads, so a change anywhere upstream reaches every cell below it;
- the type and value of every variable its SQL reads (unset, `NULL` and a value are all different);
- the version of every file it names by a literal path: the first argument of a file reader (`read_csv`,
  `read_csv_auto`, `read_parquet`, `parquet_scan`, `read_json`, `read_json_auto`, `read_ndjson`,
  `read_json_objects`, `read_ndjson_objects`, `read_text`, `read_blob`, `read_xlsx`, `sniff_csv`, `glob`
  and the `parquet_*` metadata functions), as one string or a list of strings, and a string after `FROM`
  or `JOIN` (`FROM 'events.csv'`). A file's version is its size, its last-modified time, its file id and
  its version tag (an ETag, on file systems that have one) as DuckDB's file system reports them; a glob's
  is the list of files it matches with each one's version; a file that does not exist has the version
  *missing*. Versions are taken when the notebook is opened and, during a run, just before each cell
  runs: a cell is keyed with the files as it read them, so a file that changes after a cell read it (a
  later cell writes it, or anything else does) leaves that cell stale, and a file an earlier cell of the
  same run wrote does not. A view cell (one that only makes views: its statements are `CREATE VIEW`s,
  plain queries and a final named query that is not `materialize=table`) is read when a cell selects from
  it, so that cell is keyed with the files the views it reads select from as they were when it ran. A
  cell with any other statement (a table, a variable, an `INSERT`) keeps what it read when it ran, and is
  keyed with its own versions throughout, the views it reads included. If such a cell also makes a view
  (a `CREATE VIEW`, or its final named query), that view is still read as its reader runs, so the reader
  is *unverified* at best. This is per cell: a reader of a table such a cell makes is unverified too.
  Looking at a file opens it and reads none of it. Only files on this machine are looked at: a remote one
  (any `scheme://` path but `file://`), a UNC path (`\\host\share\x.csv`, `//host/share/x.csv`), a
  Windows device path (`\\.\…`, `\\?\…`, which POSIX reads as a pattern in a directory, `/?/C:/…`), a
  path with a `net` or `network` segment in any case (the automounter's `/net/host/…`, `/Network/…`), a
  glob with a pattern in a directory segment or a `**` (`/n?t/host/…`, `data/*/x.csv`, `/data/**`: such a
  glob can go into whatever a pattern matched, wherever a link, the working directory or `~` leads; only
  a pattern in the last segment, as in `data/*.csv`, lists just the directory the path names, though a
  link in it is followed as any linked path is), a path starting with `~` not followed by a separator
  (DuckDB reads `~net/…` as `$HOME` then `net/…`), and a `file:` URL other than `file:/…`, `file:///…` or
  `file://localhost/…` to a local path have no known version, and cost no request. Nor has a relative
  path while `file_search_path` is set, or a path starting with `~` while `home_directory` is set, for
  the database or by a cell of the notebook (`SET`, `RESET`), since DuckDB may read it from somewhere
  else.

What a cell shows on top of what it computes is a second layer: its chart specs, `rows`, `width`,
`height`, `label` and `placeholder`, and the selections only its charts filter on. Editing a query's chart or brushing a chart
that filters it changes that cell's key, not the keys of the cells that read it.

**Fresh, unverified or stale.** When a notebook is opened, each cell's key is computed from the text as
it is now, the variables recorded in `@state` (the last values of its inputs and selections) and the
files' versions now. A cell whose key differs from its entry's is *stale*: the cell, something it reads,
a variable it reads or a file it names changed after the result was made. Its last result is shown in
fainter ink with the note *from an earlier run* until it runs. A cell whose key equals its entry's is
*fresh*, unless it reads data the key cannot cover, here or in a cell it reads; then it is
*unverified*:

- a relation that no cell defines under exactly that name: a table already in the database, a table
  of a database or schema a cell attached or created (`wh.orders`), a catalog view;
- a view made by a cell that keeps state (see the key above);
- a remote file, a UNC path, a relative path while `file_search_path` is set or a `~` path while
  `home_directory` is set (by the database or the notebook); a file whose version
  the file system cannot report (neither a modified time nor a version tag); a file modified in the last
  two seconds, which could still change without its modified time changing (modified times are often
  whole seconds); or a glob matching more than 1000 files. A result made while one of its files was
  that new stays unverified, even once the file is older: it was made from a version nothing could
  vouch for. A cell that reads a file the cell before it wrote is usually in this case;
- a file reader given anything but literal paths (`read_csv($path)`), or another scan of external data:
  any other `read_*`, `*_scan` or `*_query` function, `iceberg_*`, `query` and `query_table`;
- any other table function, an extension's (`st_read`) or a table macro alike, except `range`,
  `generate_series`, `unnest`, `json_each`, `json_tree` and `repeat_row`.

The editor shows an unverified result as it shows a fresh one; `notebook_cache` lists it as
`unverified`. What no key covers: data a later cell changes (an `INSERT` into a table an earlier cell
made) and functions such as `now()` and `random()`. A cell with an error shows nothing from the cache.

**Trust.** Showing cached output executes nothing (DESIGN §3): opening a notebook reads files or rows
and computes hashes, and makes no network request beyond reading the notebook itself. The inputs show the values the results were made with, and the session is given
those values, but no cell runs until the reader runs one. Until then, changing an input marks what reads
it stale instead of running it, a brush asks for a run, and only the remembered page of a grid can be
seen.

**Disposable and safe.** An entry that is not whole (truncated, not JSON, the wrong shape, another
`format`, another cell's) is ignored, never an error. Entries are written to a temporary file of the
writer's own (`<entry>.json.<uuid>.tmp`) and moved into place; they are not synced to disk, since an entry
lost in a crash only means that cell reopens empty. An entry file is readable and writable by its owner
only (mode `0600` where the file system has modes): it holds query results, which the notebook does not. Entries of cells that no longer exist are removed on the next run. The `format` number
is raised whenever what a key covers or how an entry is laid out changes, and an entry with another
number is simply ignored.

**Who reads and writes it.**

| | reads | writes |
|---|---|---|
| The editor | when a notebook is opened | after every run, redraw and input change |
| `notebook_run` | never: a run computes from the data as it is | only with `cache := true` |
| `notebook_export` | never: an export shows the data as it is now | never: an export has no side effects |
| `notebook_cache(path)` | lists entries with `status` fresh, unverified or stale | |
| `notebook_cache_clear(path)` | | removes the notebook's entries |

`notebook_run(path, cache := true)` is how a scheduled job or a colleague prepares a notebook so it
opens with results. In the editor, the gear menu's *Forget* clears the cache and *keep results* writes
`cache:` in the `[notebook]` cell (§6).

Teams who want reviewable outputs can commit an export (HTML or Markdown) instead of the cache.

## 9. Storage inside a database

The same text can be stored in a DuckDB database: see
[DESIGN.md §5](DESIGN.md#5-notebooks-inside-the-database). The stored `content` is byte-identical to
the file, so this specification is the only format.

A stored notebook is named `db:<name>` (the newest version) or `db:<name>@<version>`, where the version
is a whole number from 1; the name is everything between `db:` and the last `@`. It is read from the
current database. A name a notebook is saved under holds no `@` and is not blank: `notebook_save`
refuses one (a file `report@2.sql` is saved with `name := '...'`). A name saved with an `@` before this
rule is still read: `db:report@2@1` is version 1 of `report@2`, since the version follows the last `@`.
The editor does not save `db:<name>@<version>`: an old version opens, and saving it is refused rather
than made the newest. `notebook_save` and `notebook_checkout` commit on a connection of their own (they
are not part of the caller's transaction); a save takes the next version number in the same
transaction as its insert.

## 10. Verified against DuckDB 2.0.0

These properties were checked with the DuckDB 2.0.0 CLI and Python client while writing this draft:

- `-- %%` lines and block-comment bodies (including `;` inside them) are ignored by the parser.
- Block comments nest; an unbalanced `*/` in a body is a parse error.
- `$name` resolves to `SET VARIABLE name` when no value is bound, in a plain query and inside a view,
  and a view reflects a later `SET VARIABLE`. This is documented 2.0 behaviour.
- `STRUCT` and `LIST` variables work as specified: `$brush.lo`, `'EU' IN $regions`.
- Variables and temporary views are private to a connection.
- `examples/revenue.sql` parses; run as-is it stops at the first cross-cell reference, as §7 says.
  `examples/revenue.expanded.sql` runs to completion.
