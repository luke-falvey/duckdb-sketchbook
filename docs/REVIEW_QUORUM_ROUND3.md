# DuckDB review quorum, round 3

The third review of `main` (58c6017, after round 2 was merged as PR #20) by the 15 simulated reviewer lenses in
luke-falvey/duckdb-webhdfs `.github/agents/`. Each lens ran as its own read-only subagent with the same scope: what
rounds 1 and 2 missed, what round 2's fixes broke or left half done, new problems, and a view on every item round 2
left open. No lens saw another's answer. They are lenses built from public review activity, not the people they are
named after.

Consensus means raised by 3 or more lenses, or by 2 with high severity traced the same way. Every claim below was
checked against the code before it was acted on, and where it could be, settled by running it.

## Verdict tally

14 lenses: **request changes**; 1 (carlopi): **approve with nits**. Nobody found a crash or a security hole on the
scale of round 1. Most findings are round-2 fixes that stopped part-way: C7 (`USE`) and C15 (time limit) reached
only some entry points, and C2 (versions before the run) fixed one order of write and read but broke the other.

| Lens | Verdict | Main points |
|---|---|---|
| mytherin | request changes | Sessions keyed by source, not database; the limit is per cell, not per call, and exports have none; attach race now in every export; `ResultCache::database` as a field contract |
| hannes | request changes | UNC paths count as local (SMB before trust); C7 key; save widens 0600 to 0644; limit dropped under `lock_configuration`; export detach race |
| lnkuiper | request changes | Every request copies every temp table, and C8 made it dearer; C7 key; exports unbounded; limit set after Prepare; failed-copy replay clashes |
| carlopi | approve with nits | Limit fails open under `lock_configuration`; C7 key; old `db:` caches orphaned under `USE`; html-wasm test checks less than it says |
| tishj | request changes | Write-then-read stale forever; limit per cell; C7 key; orphaned caches; store restores tables alphabetically (foreign keys) |
| taniabogatsch | request changes | C7 key; failed-copy fallback fails itself; no restart test; no directory fsync; `CanonicalizePath` exists, so C10 needs no platform code |
| hawkfish | request changes | Write-then-read stale forever; untrusted brush stored as VARCHAR; TIMESTAMPTZ brushes (proposed answer); integer bounds rounded outward; FAT's 2 s mtimes |
| samansmink | request changes | Session settings (TimeZone, s3_region, file_search_path) do not reach `notebook_run`; C7 key; two "is local" predicates; remote `WriteFile` uploads twice |
| maxxen | request changes | Write-then-read stale forever; C10 and C13 left open on a false premise (`CanonicalizePath`); alphabetical restore; `CacheLocation` struct |
| pdet | request changes | C7 key; exports ignore interrupt and limit; interrupt path untested; a failed PDF export deletes the previous PDF |
| tmonster | request changes | Per-request restore rebuilds indexes; `duckdb_tables()`/`duckdb_columns()` scan every attached catalog per cell; write-then-read stale; timing-dependent tests |
| dtenwolde | request changes | C8 fallback erases a table another cell made; C7 key as hidden global state; a temp macro shadowing `range`; listing failure treated as copy failure |
| szarnyasg | request changes | Write-then-read stale (undocumented); C7 key contradicts the docs; C16 docs half done; limit wording; version messages |
| kryonix | request changes | Write-then-read stale; CTE scoping (reuse peek's); `sample:` before the brush and unseeded; paging without order; temp sequences restart |
| dentiny | request changes | Write-then-read stale, and `""` meaning two things; C7 key; `missing` under `file_search_path`; two "is local" predicates |

## Consensus (implemented unless marked open)

| # | Finding | Lenses | Change |
|---|---|---|---|
| R1 | Server sessions are keyed by `source` alone, but carry the database of whichever `notebook_start` created them: `USE a; notebook_start('db:x'); USE b; notebook_start('db:x')` opened and saved `a`'s `x`, while the picker listed `b`'s. `Start` then `SetDatabase` left a window with no database. `a.sql` and `./a.sql` were two sessions | 15 (every lens; 11 on the database, the rest on the key) | Sessions are keyed by (database, `db:` name) for stored notebooks and by `FileSystem::CanonicalizePath` for files. `notebook_start` puts its current database in the URL (`&db=`), the page sends it with every request, and the picker keeps it; `Start(port, database)` sets it before the server answers. A session is built outside the server's lock and inserted only if no other request got there first (C13, in part). Browser test `test_session_keys` |
| R2 | C2 takes every file version once, before the first cell: a cell that reads a file an earlier cell writes (`COPY … TO 'x'`, then `FROM 'x'`) is stale after every run, forever. Before round 2 it was fresh | 8: szarnyasg, tmonster, dentiny, hawkfish, tishj, maxxen, kryonix, taniabogatsch | Versions are taken just before each cell runs (`RunFileVersions`, `CacheKeys(…, cell_files)`), so a cell is keyed with the files as it read them. A later change still makes it stale |
| R2a | …and per-cell versions alone would not help: the file was written milliseconds before, so the two-second rule gives it no version, which recorded a key that can never match: `stale` again (`""` meant both "unknown" and, once recorded, "certainly different") | 2: dentiny, tishj | A version within the two seconds is kept with a `?` mark: the key covers it but is not verified, and the entry is stored as such (`CacheEntryKey`, a `?` before the key). It reads `unverified` while the file is unchanged, `stale` once it changes, and never `fresh`. No layout change; `CACHE_FORMAT` stays 3, since old entries compare as before. Core and SQL tests |
| R3 | C15 is half done: the limit is set after `Prepare` (the `[notebook]` cell's ATTACH is unbounded); the `SET` is refused under `lock_configuration` and the failure ignored, so the limit fails open (`Runner::Peek` fails closed); it bounds each cell, not the call (N cells take N × limit); a cell's own `SET max_execution_time = 0` lifts it; exports (and the precomputed export's up to `budget` runs) have no interrupt check and no limit at all; nothing tests the interrupt path | 10: mytherin, carlopi, hannes, tishj, pdet, lnkuiper, tmonster, dtenwolde, szarnyasg, samansmink | `Runner` takes the caller's `ClientContext`. Before the replay, before `Prepare`, before every cell and after the last it throws if the caller was interrupted or its deadline passed; and a `ClientContextState` on the runner's connection lowers every query's deadline to the caller's as the query begins, so no query outlives the call whatever a cell `SET`s (and no `SET` is needed, so the lock does not apply). `notebook_run` and every export pass it. SQL tests: one or two slow cells fail the call; a cell's `SET max_execution_time = 0`, in an earlier cell or the same one, does not lift it; an export is bounded and writes nothing; the limit holds under `lock_configuration`. (Reworked in the PR review loop, iteration 1) |
| R4 | C8's fallback for a failed store copy fails itself: a cell that made A (copied) and B (not) replays `CREATE TEMP TABLE A` over the A the store restored, which stops the replay before B; a table an earlier cell made whose re-copy fails (after an `INSERT`) is erased with nothing to make it again; a listing failure got the same treatment | 3: lnkuiper, taniabogatsch, dtenwolde | When a table the cell made cannot be copied, none of the cell's tables comes from the store and its `CREATE TEMP TABLE` statements are replayed, and no later cell copies them in (`SessionState::replayed`); a table an earlier cell made keeps its older copy. Browser test through a test-only hook, the `SKETCHBOOK_TEST_FAIL_STORE_COPY` environment variable (PR review loop, iteration 1) |
| R5 | Two "is local" predicates (`notebook::IsLocalPath`, `IsLocalFile`) that agree only by accident, and the core one counts UNC paths (`\\host\share\x.csv`) as local: opening an untrusted notebook with a cache folder would make Windows connect to the host over SMB before the reader trusted anything | 5: samansmink, dentiny, maxxen, mytherin, hannes | `IsLocalPath` refuses `\\…` and `//…`; `IsLocalFile` and the atomic save use it. Core test |
| R6 | `WriteFile` on an object store uploaded a temporary file, then copied and deleted it (or, without `MoveFile`, uploaded everything twice), and could leave the temporary object behind | 3: samansmink, pdet, lnkuiper | An object store's target (`FileSystem::IsRemoteFile`) is written directly: its upload is already all or nothing. (Narrowed in the PR review loop, iteration 1: other schemes and UNC shares keep the move) |
| R7 | C10 (links and modes on save) was left open on a false premise: DuckDB 2.0's `FileSystem::CanonicalizePath` resolves links (`realpath`, `GetFinalPathNameByHandleW`) through the opener, and works for a file that does not exist yet | 2 found it (taniabogatsch, maxxen); 7 wanted links kept, 7 would document only (to avoid platform code) | `WriteFile` writes the temporary file beside the canonical target and moves it there: a link stays a link, with no platform code, and the target is checked against `allowed_directories` (a link out of them is refused, not replaced). Cache entries are written `FILE_FLAGS_PRIVATE` (0600), since they hold query results (hannes, dentiny). Browser test. **Open**: the notebook file's own mode is reset and hard links broken (no `FileSystem` primitive); documented |
| R8 | Saving from the editor on `db:x@N` quietly forced a new newest version | 9: mytherin, samansmink, lnkuiper, taniabogatsch, dtenwolde, maxxen, hannes, tishj (refuse); pdet (save and re-key) | Refused, with what to do instead. Browser test |
| R9 | Old `db:` caches: before C7 every stored notebook's cache was in the default database; under `USE` it was neither read nor cleared | 2: carlopi, tishj (low, traced the same way) | Read from the default database when the current one has nothing, cleared with it, never written; unless the default database stores a notebook of that name, whose cache it is (PR review loop, iteration 1). SQL tests |
| R10 | The store restores temp tables in name order, so a table with a foreign key to one later in the alphabet fails its DDL and comes back without its constraints | 2: tishj, maxxen | Checked: DuckDB allows foreign keys between temp tables. Tables whose statement fails are tried again while any succeeds. Browser test |
| R11 | Replay and stored tables keyed by cell index (open from round 2) | 9: dtenwolde, mytherin, pdet, lnkuiper, taniabogatsch, maxxen, tishj, hannes, tmonster | **Open**. Everyone agrees on `CacheCellIds`. But `Restore` runs without the notebook and replays in index order, which is cell order; keyed by id it needs an order of its own, plus pruning of ids no longer in the notebook. A change of its own |
| R12 | Shared attaches: a one-off run detaches an alias another session found attached and uses; two runs race on check-then-attach. Round 2's C5 made every export detach, so it happens more | 7: mytherin, hannes, carlopi, pdet, lnkuiper, maxxen, taniabogatsch | **Open**. Agreed answer: a per-`DatabaseInstance` registry in the object cache, a reference count per alias, check-and-attach under its lock, detach at zero. It changes who owns an attach (DESIGN §11), so a change of its own |
| R13 | Each server request restores every temp table from the store (now through its DDL, rebuilding keys), and `KeepTempTables` lists `duckdb_tables()`/`duckdb_columns()`, which scan every attached catalog, after every writing cell | 2, one high: tmonster (high), lnkuiper | **Open**: needs a benchmark first (both said so). Directions: restore only what a request reads; look entries up through the catalog API instead of the table functions |

## Unique flags

Implemented, because they were cheap and clearly right:

- **dentiny, hannes: `file_search_path`.** DuckDB applies it when reading, not when opening, so a relative path was
  recorded as `missing`, a known version, and stayed fresh however the file it read changed. With
  `file_search_path` set, a relative path has no known version (`unverified`). SQL test.
- **kryonix: a `{{ }}` expression ending in a line comment** (`{{ total -- EUR` then `}}` on the next line) was a
  syntax error: `SELECT (` + expression + `)` put the `)` in the comment. Now `+ "\n)"`, in the runner and in the
  graph. SQL test. (A comment on the same line as `}}` swallows the `}}`; that is the lexer working as specified.)
- **pdet: a failed PDF export deleted the previous PDF.** It now prints to a file of its own and moves it over the
  target once printed.
- **hannes: `combinations * values.size()`** in the precomputed export could overflow with an absurd `budget`; now
  compared by division.
- **szarnyasg: the two version errors.** One rule for both: "a version is a whole number from 1 to 2147483647; leave it
  out for the newest" (`notebook_checkout(…, version := 3000000000)` used to say "versions start at 1").
- **szarnyasg: docs.** C16 reached the reference only: the exporting guide and DESIGN §3 now say `pdf` is refused
  under `lock_configuration`; troubleshooting has rows for that, the `@` name, the time limit and the old-version
  save; the headless guide explains the limit; FORMAT §8 says opening makes no request "beyond reading the notebook
  itself"; FORMAT §9 says names saved with `@` before the rule are still read (`db:report@2@1`); DESIGN §17 records
  how files are checked.

Left open, needing a decision or more work than a review fix:

- **samansmink (high): session settings do not reach a run.** `SET TimeZone`, an extension option (`s3_region`) or
  `file_search_path` at SESSION scope stays on the caller's connection; `notebook_run` and the exports run on a
  fresh one. Copying the caller's `ClientConfig::user_settings` is the general answer (and replaces the
  `max_execution_time` special case), but which settings a notebook should inherit, and the server's equivalent
  (a snapshot at `notebook_start`), are a design decision DESIGN §3 has to make.
- **mytherin: an interrupt does not stop a cell already running** in `notebook_run` or an export, only the next one.
  Forwarding it means running through `PendingQuery` and interrupting the runner's connection; the deadline now
  bounds the running cell, which covers the case that matters most.
- **mytherin: `/api/page`, `/api/options` and `/api/catalog` replay with no limit** (still open from round 2).
- **C13 eviction.** Keys and building outside the lock are done (R1). The lenses split on what to evict: most would
  drop only the in-memory store after an idle time, but dtenwolde showed that replay rows name the store, so dropping
  it alone leaves views that dangle; evicting whole sessions resets trust. Sessions live until the server stops
  (now stated in DESIGN §11).
- **hawkfish: brushes.** A brush made before the reader trusts the notebook is stored as `VARCHAR` and survives
  reopening; integer and DECIMAL bounds are cast to the column type, rounding outward; TIMESTAMPTZ bounds are browser
  wall time without an offset. hawkfish's proposed rule (send TIMESTAMPTZ as UTC with `Z`, treat naive values as UTC
  in the browser) needs no type plumbing, but changes every temporal axis; it deserves a change and tests of its own.
  Also: FAT keeps 2-second mtimes, which the 2-second margin only just covers.
- **kryonix:** the references walker's cell-wide CTE filter (reuse peek's CTE scoping); `sample:` applied before the
  brush filter and without `REPEATABLE` (and `sample: 10%` read as 10 rows); paging with `LIMIT/OFFSET` and no total
  order; temp sequences replayed from the start while their tables come back with rows.
- **dtenwolde:** a temp macro shadowing an allow-listed table function (`range`) counts as pure.
- **taniabogatsch:** no storage test survives a restart (open since round 2); a save syncs the file but not its
  directory; a crash between write and move leaves `<file>.<uuid>.tmp` beside the notebook.
- **pdet, tmonster:** `notebook_cache_sources.test` sleeps over 2 s several times, and its "unverified right after
  writing" checks assume the run starts within two seconds of the write. An injectable clock would remove both.
- **pdet:** `notebook_export(file := NULL)` treats NULL as not given and writes beside the notebook; it should be a
  bind error, as `notebook_run(NULL)` is (older than this PR).
- **carlopi:** the html-wasm failure test does not check that the export's temporary database file is gone (where
  it lives depends on `TMPDIR`, which a portable test cannot glob); `TempDirectory()` falls back to `/tmp` and `.`.
- **maxxen:** `ResultCache::database` is a field callers must set before `Load` (mytherin too); `IsConflict` still
  matches "duplicate key" for CONSTRAINT errors; two mechanisms classify table functions.
- **samansmink:** nothing is logged through DuckDB's logger, so a missing cache or an unverified remote file leaves
  no trace.
- **tishj:** `notebook_lock.test` locks the configuration for the rest of its file, which CLAUDE.md's "all test
  files share one database" would make unsafe. It passes because the sqllogictest runner opens a fresh in-memory
  database for each file; what files do share is what they write to disk (`__TEST_DIR__`, cache folders). The
  CLAUDE.md sentence should be corrected; left to whoever owns that file.

## Disagreements

- **C10: keep links, or document.** Half the lenses would only document that saving replaces a link, to keep
  platform code out of `src/functions`. `CanonicalizePath` removes that objection, so links are kept. hannes worried
  that resolving a link lets a save write outside `allowed_directories`; it is the other way round, since the opener
  checks the resolved target and refuses it.
- **`db:x@N` save: refuse or re-key.** pdet would save it as the newest and re-key the session so no edit is lost; the
  other eight would refuse. Refusing won: the page keeps the text, and the message says how to keep it.
- **Eviction: store only, or whole sessions** (above).

## PR review loop

After PR #21 was opened with CI green, the 15 lenses reviewed the PR itself: its diff and the code around it, each
as its own read-only subagent, each with a verdict. Findings were fixed and the loop repeated until no lens asked for
changes, at most five times.

### Iteration 1 (fc16b80)

**Verdicts: 12 request changes, 3 approve with nits** (carlopi, mytherin, hannes). Most lenses reproduced their
findings on the release build.

Blocking, all fixed:

| Finding | Lenses | Fix |
|---|---|---|
| R3 bounds each query, not the call: the time left is written as a duration, which every query of a cell gets afresh (several statements, the count, the page, the charts, the replay), and a `SET max_execution_time = 0` earlier in the same cell lifts it. The outcome also depended on which cell ran out (an error row for the last, a failed call otherwise) | 12: tmonster, lnkuiper, maxxen, kryonix, dtenwolde, hawkfish, mytherin, hannes, taniabogatsch, szarnyasg, pdet, carlopi | `CallerDeadline`, a `ClientContextState` whose `QueryBegin` lowers each query's deadline to the caller's (maxxen's suggestion: the deadline itself, not a duration); `Runner::CheckCaller` after the last cell too, and before an export writes. Tests: a single slow cell and a same-cell `SET max_execution_time = 0` now fail the call; an export leaves no file |
| `WriteFile` wrote every non-`file://` scheme and, through R5, every UNC and `//` path directly: saves to `hdfs://`, `webhdfs://` or a network share were no longer atomic | 7: pdet, tishj, taniabogatsch, samansmink, maxxen, dentiny, hannes | Direct only for `FileSystem::IsRemoteFile` (object stores); everything else keeps the move |
| R9's fallback read, and cleared, the live cache of the default database's own notebook of the same name | 3: szarnyasg, tishj, taniabogatsch | Only when the default database stores no notebook of that name. SQL test with the name in both databases |
| `file_search_path` set by a cell of the notebook (session scope, on the runner's connection) was not seen, so the cell stayed fresh | 5: samansmink, dentiny, carlopi, hannes, szarnyasg | `notebook::SetsSetting` finds a `SET`/`RESET file_search_path` in the notebook's cells (no SQL needed, so opening agrees with running). Core and SQL tests |
| R2 missed views: a view is read when a later cell selects from it, so a cell reading a view of a file an earlier cell rewrote was still stale forever | 1, blocking: kryonix | `CacheViewClosure`, `CacheReadFiles`: a cell's versions cover the views it reads, and its key takes them with its own versions. Core and SQL tests |

Nits fixed:

- `\\?\C:\…` (extended-length local) and `\\.\…` (device) paths are local again (carlopi, dentiny, hannes, pdet).
  `//x` stays refused on POSIX: a path cannot say which system it was written for (commented).
- The printed PDF is removed if the final move fails (carlopi, dentiny, hannes, taniabogatsch, pdet); a test that a
  failed print keeps an older PDF and leaves nothing beside it (pdet; `require notwindows`).
- `notebook_run` no longer looks at every file twice: versions for cells that did not run are taken when the run is
  recorded, and only for those (tmonster, lnkuiper, dentiny).
- Database names in session keys are matched without case (dtenwolde, tishj). Browser test.
- R4 across cells: a table a cell's replay makes again is not copied in by a later cell (dtenwolde). Tested with the
  hook above (asked for by mytherin, taniabogatsch, dtenwolde, tmonster).
- The `?` mark has one helper set, `IsUnsure`, `StripUnsure`, `MarkUnsure`, with tests, and a note on why it needs no
  `CACHE_FORMAT` (hawkfish, maxxen).
- `notebook_lock.test` keeps its limit generous and checks it last (maxxen, tishj); the docs and the stale test
  comment say what the limit does now (szarnyasg, pdet, mytherin, hannes).

Nits declined:

- `ExportOptions::caller` as an explicit argument (mytherin): only `Scan` sets it, on a local copy, and the server
  deliberately passes none; threading it through four signatures buys little.
- A typed field instead of the `?` mark (maxxen): the mark changes no layout and old readers fail safe; the helpers
  keep it in one place.
- A struct for session keys (maxxen); resolving the path once per request rather than per session (lnkuiper): a
  `realpath` per request is cheap.
- The restore retry loop's quadratic worst case (tmonster, lnkuiper, kryonix, hannes): bounded by the depth of a
  foreign-key chain among temp tables; part of R13's per-request cost, still open.
- An interrupt test (pdet): sqllogictest cannot interrupt, and the browser suite has no Python DuckDB; the interrupt
  check is the deadline's.
- The older copy an earlier cell's table keeps after a failed re-copy is not marked unverified (taniabogatsch): stated
  in DESIGN §11; a mark would need a per-table record in the cache keys.
- One PR per consensus item (mytherin): agreed for the next round.

### Iteration 2 (beded48)

**Verdicts: 6 request changes** (hannes, carlopi, dentiny, taniabogatsch, tishj, kryonix), **9 approve with nits**
(mytherin, lnkuiper, maxxen, samansmink, hawkfish, pdet, tmonster, dtenwolde, szarnyasg). Every lens confirmed the
iteration-1 deadline (`CallerDeadline`), `WriteFile`, R9 and `replayed` fixes against DuckDB's source, most by running
them.

Blocking, all fixed:

| Finding | Lenses | Fix |
|---|---|---|
| An iteration-1 nit fix reopened R5: every `\\.\…` device path counted as local, and `\\.\UNC\host\share\x` and `\\.\GLOBALROOT\Device\Mup\host\…` name an SMB share, so opening an untrusted notebook could connect to the host again | 5: hannes, carlopi, dentiny, taniabogatsch, tishj | `\\?\` and `\\.\` count as local only when a drive letter follows; `\\??\` (NT namespace) is refused (tishj). Core tests for each spelling |
| **The view finding came back** (kryonix, iteration 1's R2 follow-up): the overlay fixed the key of a cell that reads a view, but not of the cells below it. A `materialize=table` reading the view, and a cell reading that table, were still stale forever | 1, blocking: kryonix | The overlay is gone; the key builder computes a view's key as each reading cell sees it, with that cell's versions, memoized per version map (`KeyBuilder::ViewKey`). This also removes the quadratic rebuild (kryonix, tmonster). Core and SQL tests for the chain |

Nits fixed: `SetsSetting` also finds `PRAGMA file_search_path = …` and a quoted `SET "file_search_path"` (9 lenses);
the data export makes its directory only after the run, and an out-of-time `COPY` raises the same INTERRUPT error as
every other export (lnkuiper, mytherin, samansmink, tmonster, szarnyasg), with the docs saying that a deadline passed
while its files are written leaves those written; the PDF is moved over the file a link points to, and its cleanup
cannot hide the move's error (dentiny, carlopi, pdet); a cell that did not run takes the files of the views it reads
too (pdet); the test hook is read once and named in DESIGN §11 (8 lenses); DESIGN §3 records how the deadline is
enforced and why not as a setting (szarnyasg), with a comment on the DuckDB ordering it relies on (hawkfish, maxxen);
the deadline tests write their notebooks before the limit is set (hawkfish); stale comments and the test-file count.

Nits declined: `StoredInDefault`'s `duckdb_tables()` scan, and each reader of a view opening the view's files again
(lnkuiper, dentiny, tmonster): both belong with R13's measured work, local only. `replayed` outliving a cell that both
failed and lost a copy (dtenwolde), and not being cleared when a cell is edited (mytherin): part of R11, which keys
this state by cell id. `notebook_export(file := NULL)` writing beside the notebook (pdet): older than this PR, added to
"Left open". The PDF error's raw wait status (szarnyasg): older than this PR. `FileSystem::IsRemoteFile` is
case-sensitive (dentiny): DuckDB's own rule.

### After iteration 2

The loop first stopped here, under the safeguard "stop when a finding comes back after it was fixed": kryonix's view
finding had. The user then narrowed the safeguard: a follow-up fix one step further on is progress, so the loop goes on
to the cap of five iterations, and stops early only if the view or the device-path finding comes back a third time, or
two lenses want opposite things.

### Iteration 3 (191719c)

**Verdicts: 15 request changes.** Every lens confirmed `KeyBuilder::ViewKey` correct (views of views, tables between
views, diamonds, cycles, charts, inputs and `{{ }}` reading views, cells that did not run), and the rest of iteration 2.
All fifteen found the same blocking hole, in the device-path rule: the second return of the R5 path finding.

| Finding | Lenses | Fix |
|---|---|---|
| R5 still open by two spellings. A `file:` URL passed whatever followed the scheme, but DuckDB strips `file:/`, `file:///` and `file://localhost/` before opening, leaving `\\host\share\x` or `//host/share/x` (`file:////host/…`, `file:/\\host\…`). And `\\.\C:\..\UNC\host\…` passed the drive rule, though Windows normalises `\\.\` paths (and DuckDB passes every path through `GetFullPathNameW`, carlopi), so `..` climbs off the drive onto a share | 15 (`file:`: hannes, samansmink, pdet, taniabogatsch, lnkuiper, hawkfish, tishj, dtenwolde, dentiny; `..`: szarnyasg, mytherin, carlopi, tmonster, maxxen, kryonix, dentiny) | `IsLocalPath` rewritten to fail safe: a `file:` URL is judged by what DuckDB opens, with both platforms' offsets, and a host other than `localhost` is refused; after two separators only `\\?\` + drive + separator is local, with no `.` or `..` segment; `\\.\…` is refused. Core tests for every spelling the lenses tried |
| A view a cell `CREATE`s was not a view: its readers stayed stale forever, as named queries did before iteration 1 | 1 blocking (samansmink), 6 as a nit | `IsLazyView`, one predicate for the key builder and the closure (mytherin, maxxen, taniabogatsch, hawkfish, dtenwolde): a named query, or a cell with a `CREATE VIEW` statement. Core and SQL tests |

Nits fixed:

- A named query whose earlier statement reads a file (`CREATE TEMP TABLE e AS FROM 'e.csv'; SELECT * FROM e`) had that
  file keyed with the reader's versions, though it was read when the view's cell ran: a reader could be fresh over old
  rows (hawkfish; the unsafe direction). Only the files of a view's final query and its `CREATE VIEW` statements take the
  reader's versions (`LazyFiles`); the others keep the view's own. Core test.
- `ViewKey`'s memo was keyed by the address of a version map, and every cell of a run has its own, so a chain of views
  was quadratic (kryonix measured 6.7 s for 2000 views; iteration 2's "removes the quadratic rebuild" was wrong). It is
  keyed by the versions of the files that matter now (`ViewSignature`), and the files of a view are built from those of
  the views it reads: 6 ms for 2000 views sharing one file (kryonix's shape). A chain where each view reads a file of
  its own still grew faster than linearly (lnkuiper, tmonster; iteration 4).
- `SetsSetting` skips comments between words (`SET /* x */ file_search_path`; tishj).
- The PDF's temporary files are beside the file a link points to, so the final move never crosses file systems (pdet,
  dentiny; pdet reproduced the cross-device failure). The core test covers a view of a view, a diamond and a cycle
  (taniabogatsch).

Nits declined: a test of a data export whose deadline passes while its files are written (pdet): it cannot be timed
reliably. A mapped network drive (`Z:\x`) counts as local (dentiny): it cannot be told from a local drive; commented.
More test-suite sleeps (samansmink): with the injectable clock item, left open.

### Iteration 4 (fd790e9)

**Verdicts: 4 request changes (kryonix, taniabogatsch, dentiny, lnkuiper), 11 approve with nits.** Every lens tried
the iteration-3 path rule again (DuckDB's `GetFileUrlOffset` on both platforms, `GetFullPathNameW`, lower case) and
found no spelling that reaches another machine: the device-path finding did not come back.

| Finding | Lenses | Fix |
|---|---|---|
| **The view finding came back a third time.** A cell that creates a view and then fills a table from it (`CREATE VIEW cv AS FROM 'x.csv'; CREATE TABLE t AS FROM cv`) had `x.csv` keyed with a reader's versions, though `t` was filled when the cell ran: a reader of `t` run after the file changed was unverified (shown as current) over old rows. Iteration 3's `LazyFiles` covered a named query's earlier statements, not a `CREATE VIEW`'s later ones | 4 blocking (kryonix, taniabogatsch, lnkuiper reproduced; hannes as a nit with a driver), and hawkfish's sibling: one file read both ways in a cell | A view cell's files are split by when they are read (`SplitViewReads`): a `CREATE VIEW`'s files are also read at once when a later statement of the cell may keep what it reads (anything but another `CREATE VIEW`, a plain query and the final view), and such a file is keyed with both versions (one when they are equal, so nothing changes when nothing changed). Core tests (view then table, one file both ways, a plain query after the view, `materialize=table`) and a SQL test |
| `home_directory`: DuckDB expands `~` with it, the cache with `$HOME`, so a cell's `SET home_directory` left a `~` reader fresh over old rows; the same hole as iteration 1's `file_search_path` | 2 blocking (taniabogatsch, dentiny) | A `~` path has no version while `home_directory` is set, for the database or by a cell (`NotebookPathSettings`). SQL test; FORMAT §8 and the cache guide |

Nits fixed:

- `IsCreateView` missed `CREATE /* x */ VIEW`, `CREATE RECURSIVE VIEW` and `CREATE SECURE VIEW` (all 15 lenses in some
  form): `LeadingWords` skips comments between words as `LeadingNames` does, which also fixes `CREATE /* x */ TEMP
  TABLE` (tishj), and the modifiers are accepted. Core tests and a SQL test.
- `materialize=table` hid a `CREATE VIEW` in the same cell (taniabogatsch, tishj, maxxen, pdet, carlopi, szarnyasg,
  lnkuiper, samansmink, dtenwolde): it now applies to the final query only. Since the table is made after the view and
  might read it, the view's files are keyed both ways in such a cell: stale when they changed, never wrongly fresh.
- `ViewFiles` dedupes with a set and takes inner closures by reference; the view's own signature and each cell's split
  are memoized; `ViewKey` checks the version map's address before building a signature (tmonster, lnkuiper, mytherin,
  dtenwolde).
- A path segment of only dots and spaces after `\\?\C:\` is refused, since Windows trims them (dentiny); paths under
  `/net/` and `/Network/` are refused, since looking at one contacts the automounted host (hannes); `IsLocalPath`'s
  comment says `~` is judged as written (dentiny).
- FORMAT §8's path wording (szarnyasg) and the "6 ms" claim, which now names its shape (szarnyasg, lnkuiper).

Nits declined or left open:

- Recording "creates a view" from DuckDB's parse tree in `references.cpp` (maxxen, dtenwolde, samansmink): better, but
  the key builder is core code and runs without a database; left open with the lexer predicate as the fallback.
- A table macro (`CREATE MACRO m() AS TABLE FROM 'x.csv'`) is read lazily too (mytherin): its readers are unverified
  already (a table function the cache does not know), so safe; left open.
- `AddSources` does not take a file after a comma (`FROM t, 'x.csv'`) (tmonster): older than this PR and safe
  (unverified); left open.
- A browser test of the PDF export through a symlink (pdet): it needs Chrome printing in the suite; checked by hand by
  pdet, left open.
- A combined SQL chain of all view kinds (tmonster): the core tests cover the combinations; the SQL tests cover each kind.

### After iteration 4

The loop was set to stop after iteration 4, when the view finding came back a third time. The parent session then asked
for one final iteration (the cap is five): if the view finding came back a fourth time, it was not to be patched again
but written up as a design decision; any other blocking finding was to be fixed as before.

### Iteration 5 (81e7f8b), the last

**Verdicts: 14 request changes, 1 approve with nits (taniabogatsch).** All fourteen blocking findings are one finding:
**the view finding came back a fourth time**, now across cells. It is not patched; see the design decision below.
Nothing else blocks. Every lens confirmed the iteration-4 fixes for what they cover: the same-cell split (`CREATE VIEW`
then `CREATE TABLE`, `INSERT` or `SET VARIABLE`; one file read both ways; a plain query after the view;
`materialize=table`), the both-versions key collapsing to the cell's own when nothing changed, the memos, `IsCreateView`
spellings, and `home_directory` (a cell's `SET` and the database's setting, failing closed).

| Finding | Lenses | Outcome |
|---|---|---|
| A view cell's statements that keep state read **other cells' views** with the reader's versions. `SplitViewReads` splits only the files a cell names; each view it depends on goes through `ViewKey(dep, reader's versions)`. So `a: CREATE VIEW cv AS FROM 'x.csv'` and `b: CREATE VIEW d AS SELECT 1; CREATE TABLE ct AS FROM cv` (or a named query `b: CREATE TABLE ct AS FROM cv; SELECT * FROM ct`) leave a reader of `ct`, run after `x.csv` changed, unverified (shown as current) or, when the file is old enough, fresh over old rows. Iteration 4 made part of it reachable: a `materialize=table` cell with a `CREATE VIEW` became a view | 14, blocking, all reproduced (core drivers; release builds by mytherin, hannes, carlopi, lnkuiper, tishj, maxxen, pdet, kryonix and others; lnkuiper and hawkfish got `fresh`) | **Not patched** (the fourth return): written up as a decision below |

Nits fixed:

- The `/net/` rule matched a literal prefix only: `file:///net/h`, `/./net/h`, `/tmp/../net/h`, and `/NETWORK` on
  macOS's case-insensitive file system got past it (mytherin, lnkuiper, tishj, taniabogatsch, tmonster, dentiny). It now
  matches any `net` or `network` segment, in any case, inside `NamesThisMachine`, so `file:` URLs are judged the same
  way. A local directory of that name only makes its files unverified. The test that asserted `/network/x.csv` was
  local now asserts the opposite.
- The second version of a file read both ways is labelled `now`, not `then` (taniabogatsch, hawkfish, dtenwolde). The
  label is new in this PR, so no stored key had it and `CACHE_FORMAT` stays.
- A SQL test for the database's own `home_directory` (hannes).

Declined or left open:

- `file:~/x` (hawkfish): `FileUrlOffset` gives 0 for it, so `IsLocalPath` refuses it already: unverified.
- The editor's "run this cell and below" keys a dependency that did not run with the files as they are now, so a reader
  run alone after a file changed outside the editor can be fresh over a table filled from the old file (taniabogatsch).
  It is older than this PR (main does the same for a plain `CREATE TABLE` cell). It belongs with the decision below:
  a dependency's entry records the versions it ran with, and keying with those would close it. Left open.
- `CacheReadFiles` and `CacheViewClosure` are not memoized across cells (tmonster), and `PathSettings` could become a list
  of settings (samansmink): left open. More SQL tests for each same-cell shape (taniabogatsch, pdet): the core tests
  cover them.

### Design decision: how the cache knows what a view cell reads, and when

The view finding came back in every iteration from 2 to 5, each time one step further on: a table that reads a view
(2), a view's earlier statements and `CREATE VIEW` cells (3), a view read at once by its own cell (4), other cells'
views read at once (5). The cause is the same each time. A reader of a cell's views must be keyed with the versions it
read, but whatever the cell **kept** (tables, variables) must be keyed with the versions of when the cell ran. Knowing
which is which needs, for each statement, what it reads (files and other cells' names) and whether it keeps state. The
key builder only has that per cell (`Cell::reads`, `depends_on`), and guesses the rest from the text (`IsCreateView`,
`IsQueryStatement`, `AddSources` per statement). Each fix taught the guess one more shape.

Options:

1. **Patch the next hop** (what the fourteen lenses suggested): when a view cell has a statement that keeps state, key
   each view it depends on both ways, as its files are. Small, errs towards stale, closes the iteration-5
   reproductions. But it is the fifth patch to the same guess, and leaves the editor case above.
2. **Mixed cells are not views.** A cell is lazy only when every statement is a `CREATE VIEW`, a plain query or its
   final view query. A cell with anything that keeps state is keyed with its own versions, its dependencies included,
   as every cell was before iteration 1. Readers of a view made in a mixed cell are needlessly stale after one of its
   files changes, the safe direction and a rare notebook. This removes the class, not the instance: nothing the cell
   kept can be keyed with a reader's versions. A small change, mostly deletions (`SplitViewReads` and the
   both-versions key go).
3. **Per-statement facts from DuckDB's parse tree.** `references.cpp` records, per statement, what it defines and reads
   and whether it creates a view or keeps state, and the key builder follows names instead of cells. This is exact,
   and replaces the lexer predicates. It is the biggest change: `FakeExtract` and the core tests must model statements,
   and the key builder's dependency walk changes.

**Recommendation: option 2 now, option 3 as its own pull request.** Option 2 is small, cannot be wrong in the unsafe
direction for any shape, and is easy to review. What it gives up (mixed cells' views going needlessly stale) is what
option 3 would win back exactly, with time to design it and its tests properly. Option 3 should also close the editor
case, by keying a dependency that did not run with the versions recorded in its stored entry. Until one of them lands,
**PR #21 has a known unsafe hole**: a view cell that also keeps state, reading another cell's view. It was reproduced and
it is narrow. It comes from this PR's view keying: on `main` a reader of a view is stale forever instead (R2), the
safe direction, though the editor case exists there. The user decides before merging: option 2 first, or this PR as
it is with the hole recorded.

### After iteration 5: the user's decision

The loop reached its cap here, with the view finding written up as the decision above. **The user chose option 2**:
a cell that keeps state is not a view; it is keyed with its own versions, the views it reads included. Option 3 (the
parse tree) stays open for its own pull request, and so does the editor's "run this cell and below" case. The user
asked for the loop to go on until the quorum is happy, for at most three more iterations, with the same rules; a
finding that only argues again for option 1 or 3 is declined with a pointer to this decision.

Option 2 as built (`IsLazyView`): a view cell's statements are `CREATE VIEW`s, plain queries and a final named query
that is not `materialize=table`, and at least one makes a view. Any other statement makes the cell keep state: it is
keyed with its own versions throughout, as every cell is that is not a view. For a reader, a view cell's files all take
the reader's versions. `SplitViewReads` and the both-versions key are gone. Readers of a view made in a cell that keeps
state are stale when one of its files changed, even if nothing kept read that file: the safe direction. Tests: the
iteration-4 same-cell shapes and the iteration-5 cross-cell shapes (a cell with a view and a table, a named query that
keeps a table, `materialize=table` with a `CREATE VIEW`), each failing on 81e7f8b, plus a SQL test of the cross-cell
case.

### Iteration 6 (e6201dc), on option 2

**Verdicts: 2 request changes (hawkfish, samansmink), 13 approve with nits.** No lens argued again for option 1 or 3.
Every lens walked `IsLazyView` through the cell shapes (`CREATE VIEW` cells, named queries, `materialize=table`, plain
queries, `SET VARIABLE`, `SET`/`USE`/`ATTACH`, `INSERT`, `COPY`, `CALL`, macros, charts, prose with `{{ }}`), and
carlopi confirmed the new tests fail on 81e7f8b (11 checks).

| Finding | Lenses | Fix |
|---|---|---|
| A cell that keeps state but makes a view (its final named query, or a `CREATE VIEW`) is not a view, so its readers were keyed with its run-time versions, yet DuckDB still reads that view as the reader runs. A version can come back (a glob's match list after a file is added and removed), and then the reader was fresh over rows from another version. Within option 2's rule, and made reachable by it for named queries | 1, blocking (hawkfish, reproduced with the release build) | Such a cell `DefinesLazily`; its readers, and theirs, are unverified (a key that differs still makes them stale). Core and SQL tests |
| A glob at the root got past the `net` rule (`/n?t/host/*.csv`, `/[Nn]etwork/…`, `/*/host/…`, `/**`): `FileVersion` globs before it checks the matches, so the automounter contacted the host on open | 1, blocking (samansmink), 1 as a nit (dentiny) | An absolute path's first segment, after `.` and `..`, is refused when it is `**` or a glob that could match `net` or `network` (a small matcher for `*`, `?` and `[...]`, without case). Further down a pattern only lists local directories. Core tests |

Nits fixed: a data-changing CTE (`WITH x AS (INSERT … RETURNING *) SELECT …`) counted as a plain query, so a cell with
one could be a view (dentiny; the unsafe direction): any `INSERT`, `UPDATE`, `DELETE` or `MERGE` word makes the cell
keep state. The `IsView` comment still gave the old rule (hannes, lnkuiper, carlopi, tishj, taniabogatsch, maxxen,
pdet, tmonster). Negative tests for `ATTACH`, `USE`, `SET`, `INSERT`, `CREATE MACRO` and data-changing CTEs, before a
view and before a named query (lnkuiper, maxxen, pdet). Comments that named review iterations describe the shape
(taniabogatsch). FORMAT §8 and the cache guide say a view made by a cell that keeps state leaves its readers
unverified (mytherin, hawkfish), and the guide's path bullet is rewrapped and says "segment" (carlopi, samansmink).

Declined or left open:

- A scalar macro that reads a view (lnkuiper): references do not record macro uses, so a cell calling a macro does not
  depend on the macro's cell at all. Older than this PR; it needs `references.cpp` to record them, with option 3.
- A view cell's plain-query files key its readers too (tmonster): needlessly stale at worst, and one stat.
- Sharing `Materialized` between the cache, the script and `Runner` (mytherin): the three read the option the same way;
  a refactor for later. Unknown `materialize=` values pass without an error (taniabogatsch, pdet): older than this PR,
  and cache and runner agree.
- `CREATE VIEW IF NOT EXISTS` over a different existing view (tishj) and a relative path read with the working
  directory under `/net` (hawkfish, tishj): older than this PR, or not a new contact.
- More SQL tests for each shape and a sleep shared between cases (carlopi, mytherin): the core tests cover the shapes.

### Iteration 7 (0a8239d)

**Verdicts: 14 request changes, 1 approve with nits (pdet).** All fourteen blocking findings are one: the iteration-6
root-glob guard could be got past, the second return of that finding. Its matcher lower-cased the pattern, which DuckDB's
case-sensitive `Glob` (`like.cpp`) does not, so a range or a negated set across the cases (`/[Z-n]et/…`, `/[!N]et/…`)
matched `net` in DuckDB but not here; it took `^` as negation where DuckDB takes only `!`; and it looked only at
absolute paths, so `n?t/host/…` read with the working directory at `/`, or `../../../n?t/…` and `~/../../n?t/…`,
climbed to `/` unchecked (mytherin, lnkuiper). `DefinesLazily`, `ReadLater` (unverified propagates through readers, view
cells and tables; a key that differs still makes a cell stale) and `ChangesData` were confirmed by every lens.

Fix: the matcher is gone, so nothing depends on reading DuckDB's glob syntax right. A glob is refused (unverified) when
its first segment is a pattern with more path after it, absolute or relative, or when it has a `..` segment; a `\\?\`
prefix is not a pattern. `data/*.csv`, `*.csv`, `/data/**/x.parquet` and `~/data/*/x.csv` stay local; `/d?ta/x.csv` and
`*/x.csv` become unverified. Core tests for every spelling the lenses used.

Nits fixed: tests that unverified carries through a view cell and into a `materialize=table` reader (pdet); FORMAT §8
says `DefinesLazily` is per cell, so a reader of a table such a cell makes is unverified too (mytherin, hannes, pdet).
Declined: a comment pointing at `like.cpp` (mytherin): there is no copy of it any more.

### Iteration 8 (caa69ea), the last

**Verdicts: 15 request changes.** Every lens confirmed that dropping the matcher was right, and found one of three gaps
in the structural rule that replaced it, the third return of the glob finding (iterations 6, 7 and 8):

| Finding | Lenses | Fix |
|---|---|---|
| A bare `**` (`/**`, `**`, `./**`, `file:///**`, `C:\**`) passed: one segment, so the first-segment rule never applied, yet DuckDB crawls everything below it | 10 (carlopi, lnkuiper, tishj, hawkfish, samansmink, pdet, tmonster, kryonix, szarnyasg; reproduced with strace by lnkuiper) | See below |
| `~/n?t/host/…` and `~/*/host/…`: the first segment is the literal `~`, but DuckDB expands it from `$HOME`, which is `/` for many containers and service accounts | 5 (mytherin, hannes, taniabogatsch, maxxen, dtenwolde; `glob('~/e?c/passwd')` with `HOME=/` gave `/etc/passwd`) | See below |
| `/proc/self/root/n?t/…`, `/proc/1/cwd/n?t/…`: a link to `/` that every Linux machine has | 1 (dentiny, reproduced) | See below |

Each is one more way for a pattern in a directory segment to be matched at `/`. Rather than refuse each, the rule now
allows a pattern only in a glob's **last segment**, and never `**`: the glob lists one directory the path names
literally and goes into nothing it matched, and each match is judged again before it is looked at, so a match under
`/net` is refused there. No pattern follows a link, the working directory or `$HOME` (a link in the listed directory itself is still followed when it is listed, as a linked literal path always was; corrected in iteration 9). The cost is the safe direction:
`data/*/x.csv`, Hive layouts (`year=*/…`) and `**` are unverified; `data/*.csv`, `*.csv`, `../data/*.csv` and
`~/data/*.csv` are checked as before (`..` is no longer refused on its own, since nothing below a pattern is entered).
Core tests for every spelling of iterations 6 to 8.

Nits declined: allowing relative directory patterns when the working directory is not `/` (mytherin, hannes, dentiny,
lnkuiper): with the rule above they would still follow links; left open with the Hive-layout cost. A test that counts
the directories an open lists (lnkuiper): left open.

### Loop ended

The loop ended after iteration 8, the cap of three iterations after option 2 landed. Final verdicts (iteration 8):
**15 request changes**, all for the glob rule, which iteration 8's fix replaces. No fix after iteration 7 has been
reviewed by the quorum; the user decides whether to run another iteration before merging. Since option 2 landed:

- the view finding has not come back (iterations 6 to 8 confirmed option 2 and its follow-up, `DefinesLazily`);
- the glob finding came back three times (iterations 6, 7, 8): each fix was an enumeration of what a pattern can reach,
  until iteration 8's, which removes patterns from directory segments altogether;
- no two lenses wanted opposite things.

### After iteration 8

The parent session asked for one more iteration on the iteration-8 fix, with a rule from the user: an unverified glob
is acceptable by design, so a finding that only asks for more globs to be verified is a nit. If the glob class still
broke it was to be written up, not patched; blocking findings of another kind were to be fixed, and the loop to stop.

### Iteration 9 (a623b49), the last

**Verdicts: 3 request changes (mytherin, hawkfish, dentiny), 12 approve with nits.** **The glob rule held**: every lens
checked it against DuckDB's `LocalGlobResult`, `GlobFilesInternal`, `IsCrawl`, `HasGlob` and `like.cpp` (only `*`, `?`
and `[` are patterns, both separators split on every platform, braces are literal, the last segment lists files of one
directory, a trailing separator or a bracketed separator puts the pattern in a directory part, no opener is passed) and
found no glob that goes into what it matched.

| Finding | Lenses | Fix |
|---|---|---|
| DuckDB expands `~` as `$HOME` followed by the rest, separator or not: with `HOME=/`, `~net/host/x.csv` and `~net/host/*.csv` are `/net/host/…`, but the `net` segment rule saw `~net`. Opening the notebook contacted the host; reproduced with strace under `HOME=/`. In the `net` rule that this PR adds, not the glob rule (hawkfish, dentiny), so fixed under the parent's rule | 3, blocking | A path starting with `~` not followed by a separator is unverified. Core tests |

Nits fixed:

- The `\\?\` prefix was skipped on every platform, but POSIX reads `\\?\C:\x` as `/?/C:/x`, a pattern in a directory
  (dentiny, dtenwolde, szarnyasg). Every `\\?\` path is now unverified: `src/core` cannot tell which platform reads it.
  This reverses iteration 3's "`\\?\` and a drive is local"; extended-length paths are rare in notebooks.
- "No link … can change that" (iteration 8) said too much: DuckDB stats every entry of the directory it lists,
  following links, so a link in the listed directory is still followed, as a linked literal path always was (hannes,
  lnkuiper, carlopi, tishj, taniabogatsch, samansmink, pdet, kryonix). The comment, FORMAT §8 and the iteration-8
  text say so. Links on the reader's own file system stay accepted as local, as mapped drives are.

Declined or left open: stopping the listing at 1000 entries (hannes, hawkfish): the cost is one directory, bounded;
left open. More globs verified (relative directory patterns, Hive layouts): unverified by the user's rule.

### Loop ended

The loop ended after iteration 9. **Final verdicts: 3 request changes, 12 approve with nits**; the three are one finding
(`~name`), fixed after the iteration under the parent's rule, and not reviewed again. The glob rule, the view rule
(option 2) and everything before them held in iteration 9. No two lenses wanted opposite things.

## Testing

Local, against the release build:

- `make core_test`;
- `make test`: all of `test/sql`, 19 files (18 before the PR review loop added `notebook_export_pdf.test`);
- `test/browser/test_browser.py` (Playwright, Chromium): all checks pass (re-run after iteration 4), including the new `test_session_keys`
  (database in the URL, one session per file, a save through a symlink, private cache entries, an old version not
  saved, a foreign key through the store). The html-wasm export was skipped (no runtime built);
- `make frontend_check`;
- `mkdocs build --strict` and the link check;
- `clang-format` (18) on the C++ files touched, except `server.cpp`, whose handler lambdas it re-indents
  (CLAUDE.md).

Not run locally: the wasm builds, Windows (UNC paths, `CanonicalizePath` there), macOS, and the CI jobs themselves.
