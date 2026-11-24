# The extension in DuckDB-WASM

`run.cjs` loads the WebAssembly build of the extension into DuckDB-WASM 2.0 in headless Chromium and
runs `notebook_cells`, `notebook_sql`, `notebook_chart_query`, `notebook_run` and a markdown
`notebook_export` on `examples/revenue.sql`, which it hands to DuckDB-WASM's virtual file system. It
checks that the result cache stays off there (no directories), and that `notebook_start`, `notebook_stop` and the pdf export fail with a clear message, since
the WebAssembly build has no server (docs/DESIGN.md §21).

The files are served by an HTTP server inside the test process, on 127.0.0.1 and a free port, closed
when the run ends, with the headers that make the page cross-origin isolated (the threads bundle needs
`SharedArrayBuffer`). Playwright's request routing would avoid the server, but it cannot answer the
threads bundle, which starts its pthread workers from inside a worker.

## Run it

Build the extension with Emscripten 6.0.9, the version DuckDB's extension pipeline uses (see
CLAUDE.md for installing it):

```bash
source /path/to/emsdk/emsdk_env.sh
OVERRIDE_GIT_DESCRIBE=v2.0.1 make wasm_eh
```

`OVERRIDE_GIT_DESCRIBE=v2.0.1` stamps the extension for DuckDB v2.0.1; without it a local build is
stamped with a commit hash and DuckDB-WASM refuses it. Do not set `GEN=ninja`: the wasm targets
run `emmake make`. The first build compiles DuckDB for WebAssembly and takes about 12 minutes on 4
cores. `make wasm_threads` builds the threaded variant (the `Makefile` adds two flags extension-ci-tools'
release branch leaves out; DESIGN §21). In the threads bundle `LOAD` hangs about one time in eight,
for DuckDB's own extensions too, so the test retries a hung `LOAD` there in a fresh page and says so.

Then, with Node and the Playwright package (pre-installed in Claude's cloud sessions; otherwise
`npm install --no-save playwright` here and `npx playwright install chromium`):

```bash
cd test/wasm && npm ci && cd ../..
node test/wasm/run.cjs            # every architecture that has been built
node test/wasm/run.cjs wasm_eh    # or name them
```

CI runs the same script on the artifacts the extension pipeline builds (`wasm-test` in
`.github/workflows/MainDistributionPipeline.yml`).

## Which DuckDB-WASM

`package.json` pins `@duckdb/duckdb-wasm` `1.33.1-dev121.0`, published under npm's `next` tag and
built from DuckDB v2.0.1. An extension must match the engine's version and platform exactly (its
footer records both), so it cannot load into npm's `latest`, which is still engine v1.5. DuckDB-WASM
2.0 has two bundles: `base`, whose platform is `wasm_eh`, and `threads`, `wasm_threads`. Nothing
loads `wasm_mvp` builds any more.
