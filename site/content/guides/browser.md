# Running in the browser

The extension also builds for WebAssembly and loads into **DuckDB-WASM 2.0**, DuckDB running in a web
page. There it parses, analyses and runs notebooks, compiles chart specs and exports, all inside the
page. The editor's server cannot run there, so this is for building your own pages and tools on top
of DuckDB-WASM, not for opening the editor.

!!! note "Not the same as the html-wasm export"
    An [`html-wasm` export](exporting.md#inputs-that-still-work) is a finished page that replays a
    notebook's SQL in DuckDB-WASM. It does not load this extension. This page is about loading the
    extension itself into DuckDB-WASM.

## What works there

| Function | In DuckDB-WASM |
|---|---|
| `notebook_cells`, `notebook_sql`, `notebook_chart_query`, `notebook_peek` | Work |
| `notebook_run` | Works; each run has a session of its own, as natively |
| `notebook_export` with `html`, `md`, `sql` or `data` | Work, writing to DuckDB-WASM's virtual file system (`md` is tested) |
| `notebook_save`, `notebook_checkout` | Built, not tested there |
| `notebook_cache`, `notebook_cache_clear`, `notebook_run(cache := true)` | Run, but keep nothing: the cache needs directories, which the in-memory file system does not have. `notebook_cache` lists nothing |
| `notebook_export` with `pdf` | Fails: printing needs a local Chrome |
| `notebook_start`, `notebook_stop` | Fail with *the notebook server is not available in this build*: the server needs sockets and threads |

Paths go through DuckDB's file system as natively, so notebooks registered in DuckDB-WASM (for
example with `registerFileText`) and notebooks at a URL both work.

## Which DuckDB-WASM

- **Use DuckDB-WASM 2.0.** It is published on npm under the **`next`** tag, not `latest`:
  `@duckdb/duckdb-wasm` version `1.33.1-dev121.0` or later is built from DuckDB v2.0.1. The `latest`
  tag is still DuckDB 1.5 and cannot load the extension. The npm version number does not say which
  DuckDB is inside.
- **Pick the build for your bundle.** DuckDB-WASM 2.0 has two bundles:

    | Bundle | Extension platform | Notes |
    |---|---|---|
    | `base` | `wasm_eh` | Recommended |
    | `threads` | `wasm_threads` | Needs a cross-origin isolated page. `LOAD` hangs about one time in eight, for DuckDB's own extensions too, so wrap it in a timeout and retry, or use `base` |

    CI also builds `wasm_mvp`, because the community repository does, but no DuckDB-WASM 2.0 bundle
    loads it.

The WebAssembly extension is about 1.2 MB (400 KB compressed) and loads in 0.1 to 0.2 seconds once
DuckDB-WASM has started.

## Get the build

Every push to `main` builds `wasm_eh`, `wasm_threads` and `wasm_mvp` with the other platforms; see
[install](../start/install.md#download-the-ci-build). The artifact
`sketchbook-v2.0.1-extension-wasm_eh` holds `sketchbook.duckdb_extension.wasm`. Serve that file from your
site: DuckDB-WASM loads extensions by URL.

To build it yourself you need Emscripten 6.0.9, the version DuckDB's pipeline uses; the
[contributor notes](https://github.com/luke-falvey/duckdb-sketchbook/blob/main/CLAUDE.md#webassembly-duckdb-wasm)
have the commands. Build it stamped for v2.0.1 (`OVERRIDE_GIT_DESCRIBE=v2.0.1`): DuckDB-WASM refuses an
extension whose recorded version or platform does not match exactly.

## Load it

The extension is not yet signed by DuckDB's community repository, so DuckDB-WASM must be opened with
`allowUnsignedExtensions`. Then `LOAD` it by URL:

```js
--8<-- "site/notebooks/wasm-load.js"
```

`allowUnsignedExtensions` lets the page load any unsigned extension. Set it only on a page whose SQL
you control, never on one that runs SQL from its visitors. Once the extension is in the community
repository it will be signed, and the flag will not be needed.

## Use it

After `LOAD sketchbook`, the functions take SQL as usual. Hand DuckDB-WASM the notebook's text, then
call them:

```js
await db.registerFileText('revenue.sql', notebookText);
const cells = await conn.query(`SELECT idx, kind, name, errors FROM notebook_cells('revenue.sql')`);
const runs  = await conn.query(`SELECT name, status, row_count FROM notebook_run('revenue.sql')`);
await conn.query(`FROM notebook_export('revenue.sql', format := 'md', file := 'revenue.md')`);
const markdown = new TextDecoder().decode(await db.copyFileToBuffer('revenue.md'));
```

`notebook_chart_query` is the piece to build your own chart rendering on: it turns a spec and a table
into the SQL to run and a description of what to draw.

## Not yet

- The `html-wasm` export still uses its own DuckDB-WASM 1.5 runtime and does not load the extension.
  Moving it to DuckDB-WASM 2.0, and later inlining the extension so an exported page can recompile
  charts and be edited, are planned; see
  [DESIGN §21](https://github.com/luke-falvey/duckdb-sketchbook/blob/main/docs/DESIGN.md#21-the-extension-in-duckdb-wasm).
- A notebook on DuckDB-WASM's `opfs://` storage should have a working result cache, since OPFS has
  directories, but that is untested.
