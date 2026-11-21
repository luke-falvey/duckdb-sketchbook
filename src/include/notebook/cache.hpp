#pragma once

#include "notebook/format.hpp"
#include "notebook/json.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// The result cache (docs/FORMAT.md section 8): the last result of each cell, kept so that a notebook
// reopens showing its output without running anything. This file is the part that needs no database:
// what a cell's key covers, how an entry is written and read, and whether an entry is fresh or stale.
// Where entries live (a directory beside the file, or a table in the database) is
// src/functions/result_cache.cpp.
namespace notebook {

//! Written into every entry. An entry with another number is ignored, so changing what a key covers
//! or how a result is laid out only needs this number raised.
extern const int CACHE_FORMAT;

//! The largest result kept for one cell, in bytes of JSON. A larger result is not cached: the cell
//! reopens empty, as every cell did before there was a cache.
extern const size_t CACHE_MAX_RESULT_BYTES;

//! The id of the entry that holds the notebook's variables rather than a cell's result.
extern const char *const CACHE_STATE_ID;

//! 16 hex digits (64-bit FNV-1a). Not cryptographic: it only has to tell texts apart.
std::string CacheHash(const std::string &text);

struct CacheKey {
	//! Covers what the cell computes: its kind, name, SQL (or text) and the options that change its
	//! data, the data keys of the cells it reads, the values of the variables it reads, and the
	//! version of every file it names (CacheSources). The cells that read this one build their keys
	//! from it.
	std::string data;
	//! Covers what the cell shows: the data key plus its chart specs, the options that change the
	//! picture (rows=, width=), and the selections its charts filter on. An entry is fresh when it
	//! was stored under this key.
	std::string result;
	//! False when the key cannot cover all the data the cell reads, here or in a cell it reads: a
	//! file whose version is unknown, a relation no cell defines (a table already in the database,
	//! one in an attached database), or a scan the cache cannot check (CacheSources::unchecked).
	//! A matching entry is then `unverified` rather than fresh.
	bool verified = true;
};

//! What one cell reads from outside the notebook by literal path, found with the lexer (no database):
//! the first argument of the file readers (`read_csv`, `read_parquet`, `read_json`, `read_text`,
//! `glob`, ...: a string or a list of strings) and a string after `FROM` or `JOIN`.
struct CacheSources {
	//! Paths and globs exactly as written, in order of first appearance.
	std::vector<std::string> files;
	//! The cell reads through something whose input the cache cannot check: a file reader given
	//! anything but literal paths (`read_csv($path)`), or another scan of external data (`delta_scan`,
	//! `postgres_query`, `query(...)`, any other `read_*`, `*_scan` or `*_query` function), or any table function
	//! (Cell::table_functions) but a file reader and the few whose rows follow from their arguments (`range`,
	//! `generate_series`, `unnest`, `json_each`, `json_tree`, `repeat_row`).
	bool unchecked = false;
};
CacheSources CellSources(const Cell &cell);

//! True for a path on this machine's own file system: no `scheme://` other than `file://`, no UNC or device path but
//! `\\?\` and a drive, nothing under the automounter's `/net/`. The cache looks only at these; a remote file's version
//! would cost a request on every open and run, so its cells are `unverified`. A `~` path is judged as written: what
//! it expands to is the user's own home, not something a notebook chooses.
bool IsLocalPath(const std::string &path);

//! True if a file modified at `modified` (microseconds since the epoch) may still change without its modified time
//! changing, as seen at `now`: file systems that keep whole seconds report the same time for a same-size rewrite later
//! in that second (git's "racily clean" files). Such a file has no known version yet.
bool ModifiedTooRecently(int64_t modified, int64_t now);

//! The files of every cell's CacheSources, once each: what CacheKeys needs versions of.
std::vector<std::string> CacheSourceFiles(const Notebook &notebook);

//! One key per cell, in file order. `variables` maps a variable name (no `$`) to a text that
//! identifies its value and type (CacheVariableKey); a variable that is missing counts as unset.
//! `files` maps each path of CacheSourceFiles to a text that identifies its version (size, modified
//! time, ETag); a path that is missing or maps to "" has no known version, and one that starts with `?`
//! is a version that may still change (ModifiedTooRecently): the key covers it but is not verified.
//! `cell_files`, by cell index, replaces `files` for that cell and the views it reads (CacheViewClosure): the
//! versions as they were just before it ran (of CacheReadFiles), so a file an earlier cell of the same run wrote
//! counts as that cell read it, directly or through a view. Needs AnalyzeNotebook.
std::vector<CacheKey> CacheKeys(const Notebook &notebook, const std::map<std::string, std::string> &variables,
                                const std::map<std::string, std::string> &files = {},
                                const std::map<size_t, std::map<std::string, std::string>> &cell_files = {});

//! A cell another cell reads lazily, when it runs: one that makes views and keeps nothing. Its statements are CREATE
//! VIEWs, plain queries and a final named query that is not materialize=table, and at least one makes a view. A cell
//! with any other statement (a table, a variable, an insert) keeps what it read when it ran, and is keyed with its own
//! versions throughout.
bool IsLazyView(const Cell &cell);

//! True for a cell that defines a view, which is read when it is used: a named query that is not materialize=table,
//! or a CREATE VIEW. When such a cell is not a view (IsLazyView), its readers are unverified: the cell's
//! key has the versions of when it ran, but what it defines is read as the reader runs.
bool DefinesLazily(const Cell &cell);

//! The cells a cell reads lazily (IsLazyView) that it depends on, and those they depend on through views, in file
//! order. Their lazily read files are read when the cell runs.
std::vector<size_t> CacheViewClosure(const Notebook &notebook, size_t index);

//! The files a cell reads when it runs: its own CellSources and the lazily read files of CacheViewClosure.
std::vector<std::string> CacheReadFiles(const Notebook &notebook, size_t index);

//! A file version or entry key after the `?` mark: known, but not vouched for (ModifiedTooRecently, CacheEntryKey).
//! No version or key text starts with `?` otherwise (versions start with a digit, `glob` or `missing`; keys are
//! hashes), so the mark changes no layout and needs no CACHE_FORMAT: an older reader sees a marked key as stale.
bool IsUnsure(const std::string &text);
std::string StripUnsure(const std::string &text);
std::string MarkUnsure(const std::string &text);

//! What CacheEntry::key holds for a result made under `key`: the result key, after a `?` when the key was not
//! verified. Such an entry is at best `unverified`, even once its files can be vouched for.
std::string CacheEntryKey(const CacheKey &key);

//! Identifies a variable's value in a key: its SQL type and its text.
std::string CacheVariableKey(const std::string &type, const std::string &text, bool is_null);

//! The name each cell's result is stored under, in file order. A named cell uses its name. An
//! unnamed cell uses `@` and a hash of its own text, so it keeps its result when cells around it
//! move and loses it when it is edited.
std::vector<std::string> CacheCellIds(const Notebook &notebook);

//! An id as a file name: letters, digits and `_ . @ -` as they are, anything else as %XX.
std::string CacheFileName(const std::string &id);

//! False if the [notebook] cell says `cache: false`.
bool CacheEnabled(const Notebook &notebook);

//! Variables the browser can set: the names of input cells and of chart selections.
std::set<std::string> SettableVariables(const Notebook &notebook);

struct CacheEntry {
	//! CacheKey::result of the cell when the result was made, after a `?` if that key was not verified
	//! (CacheEntryKey).
	std::string key;
	//! CacheKey::data then: a redraw (charts only) may refresh the entry only while the data is unchanged.
	std::string data;
	//! When it was made: `YYYY-MM-DD hh:mm:ss`, UTC.
	std::string saved_at;
	//! The cell's result as the editor receives it from a run, as JSON text.
	std::string result;
};

//! True if `json` is a result the editor can show: an object with a status of `ok`, `error` or `skipped` whose
//! parts have the shapes the renderers expect, no larger than CACHE_MAX_RESULT_BYTES.
bool ValidCacheResult(const std::string &json);

//! The text of an entry file.
std::string DumpCacheEntry(const std::string &id, const CacheEntry &entry);
//! Reads an entry file. False (and `entry` untouched) for anything that is not a complete entry
//! of this format for cell `id`: truncated or corrupt text, another format number, another cell.
bool ParseCacheEntry(const std::string &text, const std::string &id, CacheEntry &entry);

struct CacheVariable {
	std::string type;
	std::string text;
	bool is_null = false;
	//! Input and selection variables: the value as the browser holds it. Null JSON when unknown.
	Json shown;
};

//! The notebook's variables when a result was last stored.
struct CacheState {
	std::map<std::string, CacheVariable> variables;
};

std::string DumpCacheState(const CacheState &state);
//! False (and `state` untouched) for corrupt or old text.
bool ParseCacheState(const std::string &text, CacheState &state);

//! The variables of a state as CacheKeys wants them.
std::map<std::string, std::string> CacheVariableKeys(const CacheState &state);

//! A cell that has a cached result.
struct CachedCell {
	int index = 0;
	//! The result was made from other inputs than the cell has now: its own text, a cell it reads,
	//! or a variable it reads has changed since. A file it reads having changed counts too.
	bool stale = false;
	//! CacheKey::verified: false when the cell reads data whose changes the key cannot see.
	bool verified = true;
	const CacheEntry *entry = nullptr;

	//! `stale`, `unverified` (the key matches, but the cell reads data it cannot check) or `fresh`.
	const char *Status() const {
		return stale ? "stale" : verified ? "fresh" : "unverified";
	}
};

//! The decision: which cells have a cached result, and whether each is still fresh. Cells with
//! errors have none (they cannot run, so nothing they show could be current). `files` as for CacheKeys.
std::vector<CachedCell> MatchCache(const Notebook &notebook, const std::map<std::string, std::string> &variables,
                                   const std::map<std::string, CacheEntry> &entries,
                                   const std::map<std::string, std::string> &files = {});

} // namespace notebook
