---
name: sketchbook-preview-export
description: 'Open, preview, screenshot, export and store duckdb-sketchbook sketchbooks. Use when asked to open a notebook or sketchbook in the browser, start or restart the notebook server (notebook_start/notebook_stop), check how it looks, take screenshots, export to HTML, HTML with DuckDB-WASM, PDF, Markdown, SQL or Parquet data (notebook_export, hide, interactive precomputed), save notebooks into the database with history (notebook_save, notebook_checkout, db:name), or troubleshoot a server that will not start or shows an old look.'
argument-hint: 'The notebook path, and whether to open, export or store it'
---

# Previewing and exporting a sketchbook

Everything runs from the DuckDB shell built in this repository, `./build/release/duckdb`, which has the
extension linked in. Build it first with `GEN=ninja make release` (about 15 minutes the first time,
seconds afterwards). See [CLAUDE.md](../../../CLAUDE.md) for the build.

## Open it in the browser

Interactively, in the shell:

```sql
CALL notebook_start('notebooks/sales.sql');                 -- opens the browser
FROM notebook_start('notebooks/sales.sql', port := 8765, open := false);   -- url, port, token
CALL notebook_stop();
```

The server lives inside that DuckDB process: it stops when the shell exits or the database closes.
`duckdb -notebook <file>` is not a CLI flag; use `-cmd`.

From an agent or a script, keep the shell alive with a pipe that never closes, and run it as an
**attached** background process (async, not detached):

```sh
(sleep infinity | ./build/release/duckdb -init /dev/null -no-agent -csv \
   -cmd "SELECT url FROM notebook_start('notebooks/sales.sql', port := 8765, open := false)" \
   > /tmp/notebook_server.log 2>&1)
```

Then read the URL from the log (wait a few seconds; the old log may still be there), check the port with
`ss -ltn | grep 8765`, and open the URL. The URL carries a token; requests without it are refused.

- `-init /dev/null` skips `~/.duckdbrc`, which may load extensions this build lacks and stop the shell.
- `-c` instead of `-cmd` exits after the query and takes the server with it.
- After rebuilding, **restart the server**: the frontend is compiled into the binary, so a running
  server keeps the old look. Stop it by killing that shell's PID, not by name.
- To reuse a port, stop the old server first.
- **Never detach preview servers, and stop every server you started before finishing.** Detached
  shells outlive the turn, pile up across a session (one per restart), and got in the way of
  approval prompts. If the user needs a server to keep running, say so and let them start it in
  their own DuckDB shell with `CALL notebook_start('<file>')`. Before stopping a server, make sure its notebook is
  saved: a session can hold edits that exist nowhere else (`GET /api/notebook` returns the text).

## Using the editor

- **▶ Run all**, or Shift+Enter in a cell to run it and (with auto-run) what depends on it.
- Ctrl+S or **Save** writes the `.sql` file. Edits made in another editor appear in the browser.
- The ⚙ gear menu: open another notebook, save, auto-run, **Reading** view (code hidden, `span` grid),
  **Graph** (dependencies), **Catalog** (tables), ◐ dark mode, default rows per page, export, and
  **handwriting**: gentle (the page is written quickly as it appears), watch, or off.
- **Watch** (or `?ink=watch` on the editor's or an export's URL; an export also has "✎ watch" in its
  header) writes the page again slowly with one pen, in reading order, the page following the pen. Space
  or p pauses, + and − change speed, n finishes the piece being written, e everything, Escape stops
  watching; the controls sit small in the bottom-left corner while the pen has work. Scrolling yourself
  stops the following until you leave the pen in view or press a key. Use it to show a notebook to
  someone, or to record it; `?ink=off` is still the way to check the finished page.
- In a cell's button strip: `</>` and `◫` write `hide=code` / `hide=output`; under a table the page-size picker
  writes `rows=N`, and "+ chart" opens the chart picker. Under a query's charts: tabs, "edit chart
  spec", "+ chart".
- Buttons to add a cell appear when the pointer is between two cells. A cell's kind symbol (☰ settings,
  ¶ text, ◉ input, ▦ query, ✎ SQL that changes data, ∿ chart) and name are in its left margin; click
  either to edit the `-- %%` line (Enter keeps, Escape cancels). Its state dot is in the right margin;
  its buttons appear on hover in a row at its top right, in line with the dot. Its rows, what it defines and its run time are in the bottom right;
  the gear's "timing" switch writes `timing: false` into the `[notebook]` cell.
- Pointing at a chart shows the values under the pointer (the nearest point on lines).
- Peek under the pen: once the notebook has run, the caret in a CTE or sub-query of a query cell shows
  its first rows in a note beside the code. Escape hides it; the gear's "peek" switch turns it off.

## Presenting

- In the editor: ⚙ → **Present**. In an HTML export: the faint `present` control at the top right, or
  open the file with `#present` at the end of its address (`revenue.html#present=3` starts on the third
  screen). The address keeps the current screen, so a reload or a shared link comes back to it.
- **→**, **Space**, **Page Down** or a click on the page move on; **←**, **Page Up** go back; **Home**
  and **End** jump; **F** is full screen; **Esc** (or the faint ✕ by the page number) leaves, landing on
  the cell that was showing.
- Inputs, chart brushing, tabs and table paging work as usual and do not move on. In a static export
  inputs are frozen as always; export with `interactive := 'precomputed'` or `format := 'html-wasm'` for
  a presentation whose inputs work without the editor.
- Code is never shown. A notebook that has not run is run first (editor).
- Each screen is written by the pen when it first arrives; going back to one already shown is instant.
  Any key or click finishes the writing. `ink=off` (`revenue.html?ink=off#present`) presents without it.
- Printing leaves presenting: paper and PDF always get the whole notebook.
- What goes on a screen: sketchbook-design, "Laying out for presenting".

## Looking at it (agents)

- To check a presentation with Playwright, open `...#present=N` for each screen and screenshot; the page
  number is `.nb-present-page`, and `Present.count` is the number of screens.

- Check in the **reading view** and in an **HTML export**, which is what readers get.
- Screenshots from the VS Code browser tools can be one render behind: take a second one before
  judging. A tab in the background cannot be clicked; open a new page instead.
- `file://` pages may be blocked; serve exports with `python3 -m http.server 8772 --bind 127.0.0.1`
  from their folder and add `?v=N` to the URL to defeat caching.
- For scripted checks, Playwright is used by `test/browser/test_browser.py` (run with
  `make browser_test`); copy its patterns (`window.__NB_READY` for exports, `#status` text "Ran" for
  the editor).
- Headless correctness first: `.github/skills/sketchbook-authoring/scripts/check.sh <file>`.

## Export

```sql
FROM notebook_export('nb.sql', file := 'nb.html');                          -- static page, offline
FROM notebook_export('nb.sql', file := 'report.html', hide := 'code');      -- results only
FROM notebook_export('nb.sql', file := 'nb.html', interactive := 'precomputed');  -- inputs work
FROM notebook_export('nb.sql', file := 'nb.html', format := 'html-wasm', wasm_runtime := 'wasm/dist');
FROM notebook_export('nb.sql', file := 'nb.pdf', format := 'pdf');          -- needs Chrome/Chromium
FROM notebook_export('nb.sql', file := 'nb.md', format := 'md');
FROM notebook_export('nb.sql', file := 'nb_plain.sql', format := 'sql');    -- runs without the extension
FROM notebook_export('nb.sql', file := 'out', format := 'data');            -- a folder: one Parquet file per named query cell
```

| Option | Use |
|---|---|
| `hide := 'code' \| 'output' \| 'all'` | Leave parts out of every cell; cells' own `hide=` always apply. Prose and inputs are never hidden by `output` |
| `params := {region: 'US'}` | Run with other input values |
| `interactive := 'precomputed'`, `budget := 16` | Run once per input value combination (up to `budget`) so inputs switch results offline |
| `format := 'html-wasm'`, `wasm_runtime := ...` | DuckDB-WASM in the page: inputs and selections re-run real SQL. Build the runtime once with `sh wasm/build.sh` (needs Node) |
| `chrome := '/path/to/chrome'` | Browser for PDF |
| `page_rows := 100` | Rows kept per table (a cell's `rows=` raises it) |
| `data_format := 'parquet' \| 'csv'` | For `format := 'data'` |

Exports carry the fonts and the hand-drawn renderer inline (about 250 KB for a small notebook) and
need no network. Static exports show inputs frozen. The editor's gear menu exports too, with "code"
and "output" boxes for `hide`.

## Store it in the database

```sql
FROM notebook_save('nb.sql', message := 'first draft');        -- name defaults from the file
SELECT name, version, saved_at FROM _sketchbook.versions;
FROM notebook_checkout('nb', file := 'nb_copy.sql');           -- version := N for an older one
CALL notebook_start('db:nb');                                  -- db:<name>[@version] works anywhere a path does
```

Saving from the editor of a `db:` notebook adds a version. Use a file-backed database (not
`:memory:`) to keep them.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Shell exits at once, error about an extension in `~/.duckdbrc` | Add `-init /dev/null` |
| Server gone right after starting | Used `-c`, or the shell's stdin closed: use `-cmd` with `sleep infinity \|` |
| Old look after a rebuild | Restart the server; hard-reload the page |
| Port in use | Stop the old server (find its PID with `ss -ltnp`) or pick another port |
| `Forbidden` / 403 from the API | Missing token: use the full URL that `notebook_start` returned |
| Extension will not load into a released DuckDB | Local builds are stamped with the commit; install the `v2.0.1`-stamped build (CLAUDE.md, "Build and test") and `LOAD sketchbook` in `duckdb -unsigned` |
| Notebook reading `hdfs://` fails | Run it in a DuckDB that has the `hdfs` extension (the repo's shell does not), with the preprod Hadoop config in `HADOOP_CONF_DIR` and no proxy variables in the environment |
| PDF export fails | Pass `chrome :=` pointing at Chrome or Chromium |
