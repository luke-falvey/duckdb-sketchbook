# Keeping results

The editor keeps the last result of every cell, so a notebook opens showing its output at once,
without running anything. Close the editor tomorrow, reopen it, and the tables and charts are still
there. The cache is disposable: deleting it loses nothing but time, and it never changes the notebook
file.

## What you see when you reopen

Each cell's last result comes back in one of three states:

- **Fresh**: nothing it depends on has changed since it ran, the files it reads included. It looks
  exactly like a result just computed.
- **Unverified**: nothing the cache can see has changed, but the cell reads data whose changes it
  cannot see, such as a table no cell in the notebook creates. The editor shows it like a fresh result;
  `notebook_cache` lists it as `unverified`.
- **Stale**: the cell, something it reads, a variable it reads or a file it reads has changed since.
  Its last result is drawn in fainter ink with the note *from an earlier run*, until you run it.

A cell whose last run failed shows nothing from the cache.

<figure class="shot" markdown>
![The hello notebook reopened, with the City input changed to Oslo: the prose and the bar chart still show the Lisbon results in faint ink, marked from an earlier run, and the status says nothing has run yet](../images/cache-stale.webp)
<figcaption>Reopened, then the input changed to Oslo: the cells that read it keep their earlier result, faint and marked, until they run.</figcaption>
</figure>

Opening a notebook **runs nothing**. Inputs show the values the results were made with, and the
session is given those values, but no cell runs until you run one. Until then, changing an input marks
what reads it stale instead of running it, a brush asks for a run, and a table shows only the page
that was kept. Nor does the `[notebook]` cell load its `extensions:` or attach its `attach:` databases:
the catalog shows the database as it is, and a select input whose choices come `from=` a query offers
them once something has run. The same holds again after the file is reloaded from disk.

## What decides fresh or stale

Each kept result records a key: a hash of the cell's kind, name, SQL or text and options (except
`hide`, `span` and `run`, which only lay it out), the keys of the cells it reads, and the type and value
of every variable its SQL reads, and the version (size, modified time, ETag) of every file it names by
its path, in `read_csv('events.csv')`, `read_parquet(['a.parquet', 'b/*.parquet'])` or
`FROM 'events.csv'`. So editing a cell makes it and everything below it that reads it stale; changing
an input makes the cells that read it stale; rewriting a CSV file makes the cells that read it stale;
moving a cell around does not.

The cache cannot see every change. A result is **unverified** rather than fresh when the cell, or a
cell it reads, reads:

- a table that no cell in the notebook creates (one already in the database, or in a database a cell
  attaches, like `wh.orders`);
- a file named by a variable (`read_csv($path)`), a file whose file system reports neither a
  modified time nor a version tag, or a file modified in the last two seconds (modified times are often
  whole seconds, so it could still change unseen). A result made from such a file stays unverified;
  a cell that reads a file the cell before it wrote is usually one;
- a remote file (`https://`, `s3://` and any other `scheme://` but `file://`), a UNC path
  (`\\server\share\x.csv`), a Windows device path (`\\.\…`, `\\?\…`), a
  path with a `net` or `network` segment in it (`/net/host/…`), a glob with a pattern in a folder name or
  a `**` (`data/*/x.csv`, `/data/**`; `data/*.csv` is fine),
  or a `file:` URL to one of those: the cache never looks at one, so opening or running a notebook costs
  no request;
- a relative path while `file_search_path` is set, or a `~` path while `home_directory` is set, for the
  database or by a cell of the notebook, which DuckDB may read from somewhere else;
- another kind of external data: `delta_scan`, `iceberg_scan`, `postgres_query` and the like, and any
  table function but `range`, `generate_series`, `unnest`, `json_each`, `json_tree` and `repeat_row`
  (an extension's reader such as `st_read`, or a table macro).

Files are looked at just before each cell runs, so a cell is kept with the files as it read them: one
that changes after the cell read it (a later cell rewrites it, or anything else does) leaves it stale.
A cell that selects from a view (a named query, or a `CREATE VIEW`) reads the view's files as it runs,
and is kept with those too. That holds for a cell that only makes views (a named query, not
`materialize=table`, or `CREATE VIEW`s and plain queries). A cell that also makes a table, sets a
variable or inserts rows keeps what it read when it ran, so its readers are kept with the files as they
were then; and a reader of a view such a cell makes is unverified, since the view is read as it runs.

Run such a cell when its data may have changed. [FORMAT §8](../reference/format.md#8-sidecar-cache)
has the exact rules.

What a cell shows on top of what it computes counts only for that cell: editing a query's chart spec or
brushing a chart that filters it changes that cell, not the cells that read it.

A **named** cell's result is kept under its name. An **unnamed** cell's is kept under a hash of its
text, so it survives cells moving around it but not an edit. Name the cells you care about.

## Where results are kept

For a notebook file, in a `.duckdb-sketchbook/` folder beside it, one folder per notebook:

```text
.duckdb-sketchbook/
  .gitignore                     *
  revenue/
    monthly.json                 a named cell's result
    @3f1c0a9d2e4b7c10.json       an unnamed cell's result, by a hash of its text
    @state.json                  the variables the results were made with
```

The extension writes the `.gitignore`, so the cache is never committed by accident. For a notebook
stored in a database (`db:<name>`), results go in that database's `_sketchbook.cache` table.

A notebook that is not on the local file system (a URL, an object store, `hdfs://`) and a notebook not
saved yet have no cache. If the
location cannot be written, there is quietly no cache; a run never fails because of it.

Results kept before the extension was renamed from `notebook` to `sketchbook`, in `.duckdb-notebook/`
or `_notebook.cache`, are still shown until the notebook is next run; clearing the cache clears them
too. So are the results of a stored notebook kept in the default database before its cache followed
`USE`, unless the default database stores a notebook of that name, whose results they are.

Entry files are readable by their owner only: they hold query results, which the notebook itself
does not.

Each entry is the result the editor was sent: status, row count, timing, rendered prose, the first page
of a query's rows, each chart's data, an input's value and choices. A result larger than **4 MiB** is
not kept, and that cell reopens empty. Charts are already aggregated and a grid keeps one page, so few
cells reach that.

## Turning it off and clearing it

- In the editor's ⚙ menu, **keep results** writes `cache: false` (or removes it) in the `[notebook]`
  cell. With `cache: false` nothing is kept, and what was kept is removed. The setting travels with the
  file, so everyone opening it gets the same behaviour.
- **Forget** in the ⚙ menu clears the kept results for this notebook. Running it brings them back.
- From SQL, `notebook_cache_clear(path)` does the same, and returns how many entries it removed.

```sql
--8<-- "site/outputs/cache.sql"
```

```text
--8<-- "site/outputs/cache.txt"
```

## Preparing results ahead of time

`notebook_run` normally neither reads nor writes the cache: a run computes from the data as it is.
With `cache := true` it writes every cell's result, so the next person to open the notebook in the
editor sees them at once. Use it in a scheduled job:

```sql
FROM notebook_run('reports/weekly.sql', cache := true);
```

`notebook_cache(path)` lists what is kept and whether each entry is fresh, unverified or stale against
the notebook and its files as they are now. Exports never use the cache: an export always shows the data as it is now.

| | Reads the cache | Writes the cache |
|---|---|---|
| The editor | when a notebook is opened | after every run, redraw and input change |
| `notebook_run` | never | only with `cache := true` |
| `notebook_export` | never | never |
| `notebook_cache(path)` | lists entries, fresh, unverified or stale | |
| `notebook_cache_clear(path)` | | removes the notebook's entries |

Teams who want results that can be reviewed in a pull request should commit an
[export](exporting.md) (HTML or Markdown), not the cache.
