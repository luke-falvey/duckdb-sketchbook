# Storing notebooks in a database

A notebook can live inside a DuckDB database as well as in a file. The stored text is byte for byte
the same as the file, with every saved version kept. That suits notebooks that belong with their
data: ship one `.duckdb` file and the reports come with it.

Use a file-backed database. Notebooks saved into `:memory:` are gone when DuckDB exits.

## Save a version

```sql
--8<-- "site/outputs/storing.sql"
```

```text
--8<-- "site/outputs/storing.txt"
```

- `notebook_save(path)` reads the notebook at `path` and stores it as a new version. The name
  defaults to the file name without its extension; pass `name :=` to choose another.
- `message :=` records why, like a commit message.
- Saving text identical to the latest version stores nothing (`saved` is false).
- Saving text identical to an **older** version is refused, because it would bury newer work. Check
  out the newer version first, or pass `force := true`.
- A name holds no `@`, which starts a version in `db:<name>@<version>`: save `report@2.sql` with
  `name := 'report-2'`. A name saved with an `@` before this rule is still read, as
  `db:report@2@1` (the version follows the last `@`).

## Use a stored notebook

Anywhere a function takes a path, `db:<name>` reads the latest stored version and
`db:<name>@<version>` a particular one:

```sql
CALL notebook_start('db:revenue');
SELECT name, status FROM notebook_run('db:revenue@1');
FROM notebook_export('db:revenue', file := 'revenue.html');
```

In the editor, saving a `db:` notebook adds a version. The ⚙ menu's notebook list shows stored
notebooks next to the files in the working directory.

## Get a file back

```sql
FROM notebook_checkout('revenue', file := 'revenue_copy.sql');
FROM notebook_checkout('revenue', version := 1, file := 'revenue_v1.sql');
```

`notebook_checkout` returns the text, and with `file :=` also writes it. It refuses to overwrite a
file whose text is not one of the stored versions of that notebook, so unsaved work is never lost;
pass `force := true` to overwrite anyway. Without `file :=` it only returns the content.

## Another attached database

Both functions use the current database, the one `USE` chose, and take `database :=` to use the
`_sketchbook` schema of another attached database.

```sql
ATTACH 'reports.duckdb' AS reports;
FROM notebook_save('revenue.sql', database := 'reports');
FROM notebook_checkout('revenue', database := 'reports', file := 'revenue.sql');
```

The editor also uses the current database: `notebook_start('db:revenue')` after `USE reports` opens,
saves and caches `revenue` in `reports`. The page's URL names that database (`&db=reports`), so the
page keeps to it whatever a later `USE` and `notebook_start` do: after `USE work`,
`notebook_start('db:revenue')` opens `work`'s `revenue`, a notebook of its own. The editor opens an old
version (`db:revenue@1`) but does not save it, which would make it the newest: open `db:revenue` to save,
or check the version out to a file.

## The tables

The first save creates a `_sketchbook` schema:

| Object | Columns |
|---|---|
| `_sketchbook.notebooks` | `name` (primary key), `created_at` |
| `_sketchbook.versions` | `name`, `version`, `content`, `content_hash` (sha256), `saved_at`, `message`; primary key `(name, version)` |
| `_sketchbook.current` | A view: the latest version of each notebook |
| `_sketchbook.cache` | Results the editor keeps for stored notebooks: `name`, `cell`, `format`, `key`, `saved_at`, `result`. Created on first use; see [keeping results](cache.md) |

A database saved to before the extension was renamed from `notebook` to `sketchbook` has its
notebooks in a `_notebook` schema instead. They are read from there, and the first save copies their
history into `_sketchbook`, so version numbers carry on. The copy is one transaction: if it fails,
nothing of `_sketchbook` is left behind, the old history is still read, and the next save tries again.
The `_notebook` schema is left as it was; drop it once you no longer need it.

## Transactions

`notebook_save` and `notebook_checkout` commit on their own, on a connection of their own: inside
`BEGIN … ROLLBACK`, a save is not undone. Each save reads the latest version and stores the next one
in one transaction, so two saves of the same notebook at once never get the same version number.

They are ordinary tables, so you can query history with SQL:

```sql
SELECT name, max(version) AS versions, max(saved_at) AS last_saved
FROM _sketchbook.versions GROUP BY name;
```
