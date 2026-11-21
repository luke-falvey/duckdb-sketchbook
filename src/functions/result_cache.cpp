#include "notebook/result_cache.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/main/settings.hpp"
#include "notebook/functions.hpp"
#include "notebook/script.hpp"

namespace duckdb {

using notebook::CacheEntry;
using notebook::CacheState;
using notebook::Json;

namespace {

// Where entries live, and where they lived before the extension was renamed: read when the new place has
// nothing for a notebook, cleared with it, never written.
const char *const CACHE_SCHEMA = "_sketchbook";
const char *const LEGACY_CACHE_SCHEMA = "_notebook";
const char *const CACHE_DIRECTORY = ".duckdb-sketchbook";
const char *const LEGACY_CACHE_DIRECTORY = ".duckdb-notebook";

string Now() {
	// `YYYY-MM-DD hh:mm:ss`, UTC.
	return Timestamp::ToString(Timestamp::GetCurrentTimestamp()).substr(0, 19);
}

//! Runs a statement with parameters. Throws on any error; callers catch.
unique_ptr<QueryResult> Execute(Connection &connection, const string &sql, vector<Value> values = {}) {
	auto prepared = connection.Prepare(sql);
	if (prepared->HasError()) {
		throw InvalidInputException("%s", prepared->GetError());
	}
	auto result = prepared->Execute(values);
	if (result->HasError()) {
		result->ThrowError();
	}
	return result;
}

//! True if the default database stores a notebook named `name` (in `_sketchbook` or the old `_notebook` schema), so the
//! cache there under that name is that notebook's own. True when it cannot tell.
bool StoredInDefault(DatabaseInstance &db, const string &name) {
	try {
		Connection connection(db);
		for (auto schema : {CACHE_SCHEMA, LEGACY_CACHE_SCHEMA}) {
			auto listed = connection.Query("SELECT 1 FROM duckdb_tables() WHERE lower(database_name) = "
			                               "lower(current_database()) AND schema_name = " +
			                               notebook::QuoteString(schema) + " AND table_name = 'versions'");
			if (listed->HasError()) {
				return true;
			}
			auto chunk = listed->Fetch();
			bool exists = chunk && chunk->size() > 0;
			listed.reset();
			if (!exists) {
				continue;
			}
			auto found = Execute(connection, string("SELECT 1 FROM ") + schema + ".versions WHERE name = ? LIMIT 1",
			                     {Value(name)});
			auto rows = found->Fetch();
			if (rows && rows->size() > 0) {
				return true;
			}
		}
		return false;
	} catch (...) { // NOLINT: when in doubt, the default database's cache is not ours
		return true;
	}
}

//! True if `schema.cache` exists in `database` (the default one when empty).
bool CacheTableExists(Connection &connection, const string &database, const string &schema) {
	auto result =
	    connection.Query("SELECT 1 FROM duckdb_tables() WHERE lower(database_name) = lower(" +
	                     (database.empty() ? string("current_database()") : notebook::QuoteString(database)) +
	                     ") AND schema_name = " + notebook::QuoteString(schema) + " AND table_name = 'cache'");
	if (result->HasError()) {
		return false;
	}
	auto chunk = result->Fetch();
	return chunk && chunk->size() > 0;
}

//! A file's text, or false if it is missing, unreadable or larger than an entry can be.
bool ReadSmallFile(FileSystem &fs, const string &path, string &text) {
	try {
		if (!fs.FileExists(path)) {
			return false;
		}
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		auto size = NumericCast<idx_t>(handle->GetFileSize());
		if (size > notebook::CACHE_MAX_RESULT_BYTES + 4096) {
			return false;
		}
		text.assign(size, '\0');
		idx_t total = 0;
		while (total < size) {
			auto n = handle->Read(&text[total], size - total);
			if (n <= 0) {
				return false;
			}
			total += NumericCast<idx_t>(n);
		}
		return true;
	} catch (...) { // NOLINT: an unreadable cache is no cache
		return false;
	}
}

//! True if DuckDB's file system has `path` on this machine's own file system. A notebook anywhere else (a URL, an
//! object store, HDFS or any other file system an extension adds) has no cache: it would be written over the
//! network on every run. False too for a file that does not exist.
bool IsLocalFile(FileSystem &fs, const string &path) {
	// The same rule as the files the cells read (notebook::IsLocalPath): any other "scheme://" path is served by an
	// extension's file system (hdfs://, webhdfs://, gs://, ...), and a UNC path is on another machine. Decided from the
	// path, not from an opened handle: wrapping file systems (the test runner's, caching wrappers) do not report
	// themselves as local even when the file is.
	if (!notebook::IsLocalPath(path) || FileSystem::IsRemoteFile(path) || Path::FromString(path).IsRemote()) {
		return false;
	}
	return fs.FileExists(path);
}

//! Writes a cache file: to a temporary file of this writer's own beside it, then moved over it, so a reader never
//! sees half an entry and two writers never share a temporary file. Not synced: the cache is disposable, and an
//! entry lost in a crash only means a cell reopens empty.
void WriteCacheFile(FileSystem &fs, const string &path, const string &text) {
	auto temporary = path + "." + UUID::ToString(UUID::GenerateRandomUUID()) + ".tmp";
	try {
		// Readable by its owner only: an entry holds query results, which the notebook file itself does not.
		auto handle = fs.OpenFile(temporary, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
		                                         FileFlags::FILE_FLAGS_PRIVATE);
		handle->Write(const_cast<char *>(text.data()), text.size());
		handle->Close();
		fs.MoveFile(temporary, path);
	} catch (...) {
		fs.TryRemoveFile(temporary);
		throw;
	}
}

//! A temporary file a writer left behind (WriteCacheFile).
bool IsCacheTemporary(const string &file) {
	return StringUtil::EndsWith(file, ".tmp") && StringUtil::Contains(file, ".json.");
}

//! True for a temporary file no writer is still writing: one older than any write takes (ten minutes), left behind
//! by a writer that stopped.
bool LeftBehind(FileSystem &fs, const string &path) {
	try {
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
		if (!handle) {
			return false;
		}
		auto modified = handle->Stats().last_modification_time.value;
		const int64_t TEN_MINUTES = 600LL * 1000000LL;
		return modified > 0 && modified < Timestamp::GetCurrentTimestamp().value - TEN_MINUTES;
	} catch (...) { // NOLINT: when in doubt, leave it
		return false;
	}
}

//! A file the store wrote: an entry, or a temporary file.
bool IsCacheFile(const string &file) {
	return StringUtil::EndsWith(file, ".json") || IsCacheTemporary(file);
}

//! A text that changes when one file does: its size, modified time, file id and version tag (an ETag, for
//! the file systems that have one). "" when the file system reports neither a modified time nor a tag: its
//! version is unknown. `missing` for a file that does not exist.
string OneFileVersion(FileSystem &fs, const string &path) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
	if (!handle) {
		return "missing";
	}
	auto stats = handle->Stats();
	bool modified = stats.last_modification_time.value > 0 && stats.last_modification_time != timestamp_t::infinity();
	if (!modified && stats.version_tag.empty()) {
		return string();
	}
	auto version = std::to_string(stats.file_size) + ":" +
	               (modified ? std::to_string(stats.last_modification_time.value) : string()) + ":" +
	               (stats.file_id.IsValid() ? std::to_string(stats.file_id.GetIndex()) : string()) + ":" +
	               stats.version_tag;
	if (modified &&
	    notebook::ModifiedTooRecently(stats.last_modification_time.value, Timestamp::GetCurrentTimestamp().value)) {
		// Modified times are whole seconds: a same-size rewrite later in this second would not change them. The
		// version is kept, so a result made from it is not stale while the file stays as it is, but not vouched for.
		return notebook::MarkUnsure(version);
	}
	return version;
}

//! The most files a glob may match and still be checked; beyond it, its cells are `unverified`.
const idx_t MAX_GLOB_FILES = 1000;

//! What a notebook's settings may change about where a path is read from: file_search_path, home_directory.
struct PathSettings {
	bool search_path = false;
	bool home = false;
};

//! A file's or a glob's version for the cache keys (notebook::CacheKeys), "" when it cannot be told.
string FileVersion(FileSystem &fs, const string &path, const PathSettings &settings) {
	if (!notebook::IsLocalPath(path)) {
		// Not looked at: a remote file would cost a request (a glob, a listing and one per match) on every open
		// and run, before the reader has trusted the notebook.
		return string();
	}
	if (settings.home && !path.empty() && path[0] == '~') {
		// DuckDB expands `~` with home_directory when reading it, which looking it up here (with $HOME) would not.
		return string();
	}
	if (settings.search_path && !fs.IsPathAbsolute(path)) {
		// DuckDB looks for a relative path in each directory of file_search_path when reading it, which opening it
		// here would not: what the cell reads may be another file, or a remote one.
		return string();
	}
	try {
		if (!FileSystem::HasGlob(path)) {
			return OneFileVersion(fs, path);
		}
		// What the glob matches is part of its version: a file added or removed changes it.
		auto matches = fs.Glob(path);
		if (matches.size() > MAX_GLOB_FILES) {
			return string();
		}
		string version = "glob";
		bool unsure = false;
		for (auto &match : matches) {
			if (!notebook::IsLocalPath(match.path)) {
				return string();
			}
			auto one = OneFileVersion(fs, match.path);
			if (one.empty()) {
				return string();
			}
			unsure = unsure || notebook::IsUnsure(one);
			version += "|" + match.path + "=" + notebook::StripUnsure(one);
		}
		return unsure ? notebook::MarkUnsure(version) : version;
	} catch (...) { // NOLINT: a file that cannot be looked at has no known version
		return string();
	}
}

//! file_search_path or home_directory is set, for the database or by a cell of the notebook (on the connection the
//! cells run on, which opening the notebook never sees): a relative path, or one starting with `~`, may be read from
//! somewhere else than where it is looked at.
PathSettings NotebookPathSettings(DatabaseInstance &db, const notebook::Notebook &notebook) {
	PathSettings settings;
	settings.search_path = notebook::SetsSetting(notebook, "file_search_path");
	settings.home = notebook::SetsSetting(notebook, "home_directory");
	try {
		settings.search_path = settings.search_path || !Settings::Get<FileSearchPathSetting>(db).empty();
	} catch (...) { // NOLINT: unknown counts as set, which only makes cells unverified
		settings.search_path = true;
	}
	try {
		settings.home = settings.home || !Settings::Get<HomeDirectorySetting>(db).empty();
	} catch (...) { // NOLINT: as above
		settings.home = true;
	}
	return settings;
}

} // namespace

std::map<string, string> CacheFileVersions(DatabaseInstance &db, const notebook::Notebook &notebook) {
	std::map<string, string> versions;
	auto files = notebook::CacheSourceFiles(notebook);
	if (files.empty()) {
		return versions;
	}
	auto &fs = FileSystem::GetFileSystem(db);
	auto path_settings = NotebookPathSettings(db, notebook);
	for (auto &path : files) {
		versions[path] = FileVersion(fs, path, path_settings);
	}
	return versions;
}

void RunFileVersions::Start(DatabaseInstance &db, const notebook::Notebook &notebook) {
	if (notebook::CacheEnabled(notebook)) {
		before = CacheFileVersions(db, notebook);
	}
}

void RunFileVersions::BeforeCell(DatabaseInstance &db, const notebook::Notebook &notebook, const notebook::Cell &cell) {
	if (!notebook::CacheEnabled(notebook) || cell.index < 0) {
		return;
	}
	auto &versions = cells[NumericCast<size_t>(cell.index)];
	versions.clear();
	// Its own files and those of the views it reads, which it reads as it runs.
	auto files = notebook::CacheReadFiles(notebook, NumericCast<size_t>(cell.index));
	if (files.empty()) {
		return;
	}
	auto &fs = FileSystem::GetFileSystem(db);
	auto path_settings = NotebookPathSettings(db, notebook);
	for (auto &path : files) {
		versions[path] = FileVersion(fs, path, path_settings);
	}
}

// ---- The store -------------------------------------------------------------------------------------

ResultCacheStore::ResultCacheStore(DatabaseInstance &db, const string &source, bool legacy, const string &database)
    : db(db), source(source), legacy(legacy), database(database) {
	schema = legacy ? LEGACY_CACHE_SCHEMA : CACHE_SCHEMA;
	table = (database.empty() ? string() : notebook::QuoteIdentifier(database) + ".") + schema + ".cache";
	if (notebook::IsStoredSource(source)) {
		in_database = true;
		auto stored = notebook::ParseStoredSource(source);
		name = stored.name;
		available = stored.error.empty();
		return;
	}
	try {
		auto &fs = FileSystem::GetFileSystem(db);
		// A cache beside a remote notebook would be written over the network on every run.
		if (!IsLocalFile(fs, source)) {
			return;
		}
		auto slash = source.find_last_of("/\\");
		auto file = slash == string::npos ? source : source.substr(slash + 1);
		auto dot = file.rfind('.');
		auto stem = dot == string::npos || dot == 0 ? file : file.substr(0, dot);
		root = (slash == string::npos ? string() : source.substr(0, slash + 1)) +
		       (legacy ? LEGACY_CACHE_DIRECTORY : CACHE_DIRECTORY);
		directory = root + fs.PathSeparator(source) + notebook::CacheFileName(stem);
		available = true;
	} catch (...) { // NOLINT
	}
}

string ResultCacheStore::FilePath(const string &id) const {
	return directory + "/" + notebook::CacheFileName(id) + ".json";
}

//! Makes the place entries are written to: the directories (with a .gitignore, so the cache never
//! needs one written by hand), or the table.
bool ResultCacheStore::Prepare() {
	if (prepared) {
		return true;
	}
	if (!available) {
		return false;
	}
	try {
		if (in_database) {
			Connection connection(db);
			Execute(connection, "CREATE SCHEMA IF NOT EXISTS " +
			                        (database.empty() ? string() : notebook::QuoteIdentifier(database) + ".") +
			                        CACHE_SCHEMA);
			Execute(connection,
			        string("CREATE TABLE IF NOT EXISTS ") + table +
			            " (name VARCHAR NOT NULL, cell VARCHAR NOT NULL, format INTEGER NOT NULL, key "
			            "VARCHAR NOT NULL, data VARCHAR, saved_at TIMESTAMP NOT NULL, result VARCHAR NOT NULL, "
			            "PRIMARY KEY (name, cell))");
			// A table made before entries kept their data key (cache format 1).
			Execute(connection, string("ALTER TABLE ") + table + " ADD COLUMN IF NOT EXISTS data VARCHAR");
		} else {
			auto &fs = FileSystem::GetFileSystem(db);
			if (!fs.DirectoryExists(root)) {
				fs.CreateDirectory(root);
			}
			if (!fs.DirectoryExists(directory)) {
				fs.CreateDirectory(directory);
			}
			// A file system without directories (DuckDB-WASM's in-memory one: creating a directory does
			// nothing and none exists) could take the entries but never list them back: no cache there.
			if (!fs.DirectoryExists(directory)) {
				available = false;
				return false;
			}
			auto ignore = root + "/.gitignore";
			if (!fs.FileExists(ignore)) {
				WriteCacheFile(fs, ignore, "# Results cached by the DuckDB sketchbook extension. Safe to delete.\n*\n");
			}
		}
		prepared = true;
	} catch (...) { // NOLINT: nowhere to write means no cache
		available = false;
	}
	return prepared;
}

void ResultCacheStore::Load(const std::vector<string> &ids, std::map<string, CacheEntry> &entries, CacheState &state) {
	if (Read(ids, entries, state) || legacy || !available) {
		return;
	}
	// Nothing here yet: a notebook cached before the extension was renamed.
	if (ResultCacheStore(db, source, true, database).Read(ids, entries, state) || !in_database || database.empty()) {
		return;
	}
	// Or a stored notebook cached before its cache followed USE: every one was cached in the default database. Read
	// there (and cleared there), never written; unless the default database stores a notebook of that name, whose
	// cache it then is.
	if (StoredInDefault(db, name)) {
		return;
	}
	if (!ResultCacheStore(db, source, false, string()).Read(ids, entries, state)) {
		ResultCacheStore(db, source, true, string()).Read(ids, entries, state);
	}
}

bool ResultCacheStore::Read(const std::vector<string> &ids, std::map<string, CacheEntry> &entries, CacheState &state) {
	bool found = false;
	if (!available) {
		return found;
	}
	try {
		if (in_database) {
			Connection connection(db);
			if (!CacheTableExists(connection, database, schema)) {
				return found;
			}
			auto result = Execute(
			    connection,
			    string("SELECT cell, key, strftime(saved_at, '%Y-%m-%d %H:%M:%S'), result, coalesce(data, '') FROM ") +
			        table + " WHERE name = ? AND format = ? AND length(result) <= ?",
			    {Value(name), Value::INTEGER(notebook::CACHE_FORMAT),
			     Value::BIGINT(NumericCast<int64_t>(notebook::CACHE_MAX_RESULT_BYTES))});
			while (auto chunk = result->Fetch()) {
				for (idx_t r = 0; r < chunk->size(); r++) {
					found = true;
					auto id = chunk->GetValue(0, r).ToString();
					auto text = chunk->GetValue(3, r).ToString();
					if (id == notebook::CACHE_STATE_ID) {
						notebook::ParseCacheState(text, state);
					} else if (std::find(ids.begin(), ids.end(), id) != ids.end() && notebook::ValidCacheResult(text)) {
						entries[id] = CacheEntry {chunk->GetValue(1, r).ToString(), chunk->GetValue(4, r).ToString(),
						                          chunk->GetValue(2, r).ToString(), std::move(text)};
					}
				}
			}
			return found;
		}
		auto &fs = FileSystem::GetFileSystem(db);
		if (!fs.DirectoryExists(directory)) {
			return found;
		}
		found = true;
		string text;
		if (ReadSmallFile(fs, FilePath(notebook::CACHE_STATE_ID), text)) {
			notebook::ParseCacheState(text, state);
		}
		for (auto &id : ids) {
			CacheEntry entry;
			if (ReadSmallFile(fs, FilePath(id), text) && notebook::ParseCacheEntry(text, id, entry)) {
				entries[id] = std::move(entry);
			}
		}
	} catch (...) { // NOLINT: an unreadable cache is no cache
	}
	return found;
}

bool ResultCacheStore::Write(const string &id, const string &key, const string &data, const string &saved_at,
                             const string &text) {
	if (!Prepare()) {
		return false;
	}
	try {
		if (in_database) {
			Connection connection(db);
			Execute(connection,
			        string("INSERT OR REPLACE INTO ") + table +
			            " (name, cell, format, key, data, saved_at, result) VALUES (?, ?, ?, ?, ?, ?::TIMESTAMP, ?)",
			        {Value(name), Value(id), Value::INTEGER(notebook::CACHE_FORMAT), Value(key), Value(data),
			         Value(saved_at), Value(text)});
			return true;
		}
		WriteCacheFile(FileSystem::GetFileSystem(db), FilePath(id), text);
		return true;
	} catch (...) { // NOLINT: writing the cache must not fail a run
		return false;
	}
}

bool ResultCacheStore::Put(const string &id, const CacheEntry &entry) {
	return Write(id, entry.key, entry.data, entry.saved_at,
	             in_database ? entry.result : notebook::DumpCacheEntry(id, entry));
}

bool ResultCacheStore::PutState(const CacheState &state) {
	return Write(notebook::CACHE_STATE_ID, string("state"), string(), Now(), notebook::DumpCacheState(state));
}

void ResultCacheStore::Remove(const string &id) {
	if (!available) {
		return;
	}
	try {
		if (in_database) {
			Connection connection(db);
			if (CacheTableExists(connection, database, schema)) {
				Execute(connection, string("DELETE FROM ") + table + " WHERE name = ? AND cell = ?",
				        {Value(name), Value(id)});
			}
			return;
		}
		FileSystem::GetFileSystem(db).TryRemoveFile(FilePath(id));
	} catch (...) { // NOLINT
	}
}

//! The ids of everything stored for this notebook (files: the names of its .json files).
vector<string> ResultCacheStore::StoredIds() {
	vector<string> stored;
	if (in_database) {
		Connection connection(db);
		if (CacheTableExists(connection, database, schema)) {
			auto result = Execute(connection, string("SELECT cell FROM ") + table + " WHERE name = ?", {Value(name)});
			while (auto chunk = result->Fetch()) {
				for (idx_t r = 0; r < chunk->size(); r++) {
					stored.push_back(chunk->GetValue(0, r).ToString());
				}
			}
		}
		return stored;
	}
	auto &fs = FileSystem::GetFileSystem(db);
	if (fs.DirectoryExists(directory)) {
		fs.ListFiles(directory, [&](const string &file, bool is_directory) {
			// Only what this extension writes there is ever touched.
			if (!is_directory && IsCacheFile(file)) {
				stored.push_back(file);
			}
		});
	}
	return stored;
}

void ResultCacheStore::Prune(const std::vector<string> &ids) {
	if (!available) {
		return;
	}
	try {
		std::set<string> keep;
		keep.insert(in_database ? string(notebook::CACHE_STATE_ID)
		                        : notebook::CacheFileName(notebook::CACHE_STATE_ID) + ".json");
		for (auto &id : ids) {
			keep.insert(in_database ? id : notebook::CacheFileName(id) + ".json");
		}
		for (auto &stored : StoredIds()) {
			if (keep.count(stored)) {
				continue;
			}
			if (in_database) {
				Remove(stored);
				continue;
			}
			auto &fs = FileSystem::GetFileSystem(db);
			auto path = directory + "/" + stored;
			if (IsCacheTemporary(stored) && !LeftBehind(fs, path)) {
				// Another writer's (a notebook_run beside the editor), about to be moved into place.
				continue;
			}
			fs.TryRemoveFile(path);
		}
	} catch (...) { // NOLINT
	}
}

idx_t ResultCacheStore::Clear() {
	idx_t removed = ClearHere();
	if (!legacy && available) {
		removed = MaxValue<idx_t>(removed, ResultCacheStore(db, source, true, database).ClearHere());
		if (in_database && !database.empty() && !StoredInDefault(db, name)) {
			// What Load reads from the default database.
			removed = MaxValue<idx_t>(removed, ResultCacheStore(db, source, false, string()).ClearHere());
			removed = MaxValue<idx_t>(removed, ResultCacheStore(db, source, true, string()).ClearHere());
		}
	}
	return removed;
}

idx_t ResultCacheStore::ClearHere() {
	idx_t removed = 0;
	if (!available) {
		return removed;
	}
	try {
		auto state_name = in_database ? string(notebook::CACHE_STATE_ID)
		                              : notebook::CacheFileName(notebook::CACHE_STATE_ID) + ".json";
		auto stored = StoredIds();
		for (auto &entry : stored) {
			removed += entry != state_name && !IsCacheTemporary(entry);
		}
		if (in_database) {
			if (!stored.empty()) {
				Connection connection(db);
				Execute(connection, string("DELETE FROM ") + table + " WHERE name = ?", {Value(name)});
			}
			return removed;
		}
		auto &fs = FileSystem::GetFileSystem(db);
		for (auto &file : stored) {
			fs.TryRemoveFile(directory + "/" + file);
		}
		// The notebook's directory goes if nothing else is in it; the cache directory itself stays.
		bool empty = true;
		if (fs.DirectoryExists(directory)) {
			fs.ListFiles(directory, [&](const string &, bool) { empty = false; });
			if (empty) {
				fs.RemoveDirectory(directory);
			}
		}
	} catch (...) { // NOLINT
	}
	return removed;
}

// ---- A session's cache ------------------------------------------------------------------------------

void ResultCache::Load(DatabaseInstance &db, const string &source, const notebook::Notebook &notebook) {
	entries.clear();
	state = CacheState();
	if (!notebook::CacheEnabled(notebook)) {
		return;
	}
	ResultCacheStore store(db, source, false, database);
	store.Load(notebook::CacheCellIds(notebook), entries, state);
}

std::set<string> ResultCache::RestoreVariables(DatabaseInstance &db, const notebook::Notebook &notebook,
                                               SessionState &session) const {
	std::set<string> restored;
	if (state.variables.empty()) {
		return restored;
	}
	unique_ptr<Connection> connection;
	for (auto &name : notebook::SettableVariables(notebook)) {
		auto found = state.variables.find(name);
		if (found == state.variables.end() || session.variables.count(name)) {
			continue;
		}
		try {
			// Only the type's name is parsed and the text cast to it: no statement runs.
			if (!connection) {
				connection = make_uniq<Connection>(db);
			}
			// ParseLogicalType binds the type in a transaction of its own (DuckDB 2.0 needs one to resolve a
			// type name); TransformStringToLogicalType without one always threw here.
			auto type = connection->context->ParseLogicalType(found->second.type);
			session.variables[name] =
			    found->second.is_null ? Value(type) : Value(found->second.text).DefaultCastAs(type);
			restored.insert(name);
		} catch (...) { // NOLINT: a value that cannot be restored leaves the variable unset
		}
	}
	return restored;
}

void ResultCache::UpdateVariables(const SessionState &session) {
	for (auto &variable : session.variables) {
		auto &recorded = state.variables[variable.first];
		recorded.type = variable.second.type().ToString();
		recorded.is_null = variable.second.IsNull();
		recorded.text = recorded.is_null ? string() : variable.second.ToString();
	}
}

std::map<string, string> ResultCache::Variables(const SessionState &session) const {
	auto keys = notebook::CacheVariableKeys(state);
	for (auto &variable : session.variables) {
		bool is_null = variable.second.IsNull();
		keys[variable.first] = notebook::CacheVariableKey(variable.second.type().ToString(),
		                                                  is_null ? string() : variable.second.ToString(), is_null);
	}
	return keys;
}

void ResultCache::SetShown(const string &name, const Json &value) {
	state.variables[name].shown = value;
}

void ResultCache::Record(DatabaseInstance &db, const string &source, const notebook::Notebook &notebook,
                         const SessionState &session, const vector<RanCell> &ran, const RunFileVersions *versions) {
	if (!notebook::CacheEnabled(notebook)) {
		if (!entries.empty() || !emptied) {
			Clear(db, source);
			emptied = true;
		}
		return;
	}
	emptied = false;
	for (auto &cell : ran) {
		if (!cell.value.IsNull() && cell.index >= 0 && NumericCast<idx_t>(cell.index) < notebook.cells.size()) {
			SetShown(notebook.cells[NumericCast<idx_t>(cell.index)].name, cell.value);
		}
	}
	UpdateVariables(session);
	if (ran.empty()) {
		// Nothing new to store (an input changed): keep the variables, look at no files.
		if (!entries.empty()) {
			ResultCacheStore store(db, source, false, database);
			store.PutState(state);
		}
		return;
	}
	std::map<string, string> before;
	if (versions) {
		// The cells that did not run are keyed with the versions from before the run where there are some (the editor
		// takes them), else as they are now: notebook_run runs every cell, and looks at no file twice.
		before = versions->before;
		auto &fs = FileSystem::GetFileSystem(db);
		auto path_settings = NotebookPathSettings(db, notebook);
		for (auto &cell : notebook.cells) {
			if (cell.index < 0 || versions->cells.count(NumericCast<size_t>(cell.index))) {
				continue;
			}
			for (auto &path : notebook::CacheReadFiles(notebook, NumericCast<size_t>(cell.index))) {
				if (!before.count(path)) {
					before[path] = FileVersion(fs, path, path_settings);
				}
			}
		}
	}
	auto keys =
	    versions ? notebook::CacheKeys(notebook, notebook::CacheVariableKeys(state), before, versions->cells)
	             : notebook::CacheKeys(notebook, notebook::CacheVariableKeys(state), CacheFileVersions(db, notebook));
	auto ids = notebook::CacheCellIds(notebook);
	ResultCacheStore store(db, source, false, database);
	auto now = Now();
	for (auto &cell : ran) {
		if (cell.index < 0 || NumericCast<idx_t>(cell.index) >= ids.size()) {
			continue;
		}
		auto &id = ids[NumericCast<idx_t>(cell.index)];
		string text;
		if (!cell.redraw.IsNull()) {
			// Only the charts were drawn again: the rest of the stored result still stands.
			auto found = entries.find(id);
			// Only the charts are new: the rows in the entry are those of its data key. If the data has changed
			// since, the entry stays as it is, stale.
			if (found == entries.end() || found->second.data != keys[NumericCast<idx_t>(cell.index)].data) {
				continue;
			}
			string error;
			auto merged = notebook::ParseJson(found->second.result, error);
			for (auto &part : cell.redraw.Entries()) {
				merged.Set(part.first, part.second);
			}
			text = merged.Dump();
		} else {
			text = cell.result;
		}
		if (!notebook::ValidCacheResult(text)) {
			// Too large to keep (or not a result at all): better no result than an older one.
			if (entries.erase(id)) {
				store.Remove(id);
			}
			continue;
		}
		auto &entry = entries[id];
		entry.key = notebook::CacheEntryKey(keys[NumericCast<idx_t>(cell.index)]);
		entry.data = keys[NumericCast<idx_t>(cell.index)].data;
		entry.saved_at = now;
		entry.result = std::move(text);
		store.Put(id, entry);
	}
	for (auto entry = entries.begin(); entry != entries.end();) {
		entry = std::find(ids.begin(), ids.end(), entry->first) == ids.end() ? entries.erase(entry) : std::next(entry);
	}
	if (!entries.empty() || !ran.empty()) {
		store.Prune(ids);
		store.PutState(state);
	}
}

idx_t ResultCache::Clear(DatabaseInstance &db, const string &source) {
	auto held = entries.size();
	entries.clear();
	ResultCacheStore store(db, source, false, database);
	return MaxValue<idx_t>(held, store.Clear());
}

Json ResultCache::CachedJson(DatabaseInstance &db, const notebook::Notebook &notebook,
                             const SessionState &session) const {
	auto cached = Json::Array();
	if (entries.empty()) {
		return cached;
	}
	for (auto &match : notebook::MatchCache(notebook, Variables(session), entries, CacheFileVersions(db, notebook))) {
		cached.Push(Json::Object()
		                .Set("idx", Json::Number(match.index))
		                .Set("stale", Json::Bool(match.stale))
		                .Set("unverified", Json::Bool(!match.stale && !match.verified))
		                .Set("saved_at", Json::String(match.entry->saved_at))
		                .Set("result", Json::Raw(match.entry->result)));
	}
	return cached;
}

Json ResultCache::ShownJson(const notebook::Notebook &notebook) const {
	auto shown = Json::Object();
	for (auto &name : notebook::SettableVariables(notebook)) {
		auto found = state.variables.find(name);
		if (found != state.variables.end() && !found->second.shown.IsNull()) {
			shown.Set(name, found->second.shown);
		}
	}
	return shown;
}

RanCell RanFromResult(int index, const string &status, const Json &result) {
	RanCell done;
	done.index = index;
	done.status = status;
	if (auto value = result.Get("value")) {
		done.value = *value;
	}
	done.result = result.Dump();
	return done;
}

// ---- notebook_cache(path) and notebook_cache_clear(path) --------------------------------------------

namespace {

struct CacheRow {
	string cell;
	int idx;
	string kind, status, saved_at;
	int64_t bytes;
};

struct CacheBindData : public TableFunctionData {
	string path;
	vector<CacheRow> rows;
};

struct CacheScanState : public GlobalTableFunctionState {
	idx_t next = 0;
};

unique_ptr<FunctionData> CacheBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<CacheBindData>();
	result->path = SourceArgument(input, 0, "notebook_cache");
	auto notebook = ReadNotebook(context, result->path);
	auto &db = DatabaseInstance::GetDatabase(context);
	ResultCache cache;
	cache.database = CurrentDatabase(context);
	cache.Load(db, result->path, notebook);
	auto ids = notebook::CacheCellIds(notebook);
	SessionState none;
	auto versions = cache.Entries().empty() ? std::map<string, string>() : CacheFileVersions(db, notebook);
	for (auto &match : notebook::MatchCache(notebook, cache.Variables(none), cache.Entries(), versions)) {
		auto &cell = notebook.cells[NumericCast<idx_t>(match.index)];
		result->rows.push_back({ids[NumericCast<idx_t>(match.index)], match.index, notebook::CellKindName(cell.kind),
		                        match.Status(), match.entry->saved_at,
		                        NumericCast<int64_t>(match.entry->result.size())});
	}
	auto column = [&](const char *name, LogicalType type) {
		names.emplace_back(name);
		return_types.push_back(std::move(type));
	};
	// The name the result is stored under: the cell's name, or `@<hash of its text>`.
	column("cell", LogicalType::VARCHAR);
	column("idx", LogicalType::INTEGER);
	column("kind", LogicalType::VARCHAR);
	// `fresh`; `stale` when the cell, something it reads, a variable it reads or a file it names has changed
	// since; `unverified` when nothing has, but the cell reads data whose changes cannot be told (a table no cell
	// defines, a file with no known version, a scan of external data).
	column("status", LogicalType::VARCHAR);
	column("saved_at", LogicalType::TIMESTAMP);
	column("bytes", LogicalType::BIGINT);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> CacheInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<CacheScanState>();
}

void CacheScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<CacheBindData>();
	auto &state = data.global_state->Cast<CacheScanState>();
	for (idx_t count = 0; state.next < bind.rows.size() && count < STANDARD_VECTOR_SIZE; count++) {
		auto &row = bind.rows[state.next++];
		output.data[0].Append(Value(row.cell));
		output.data[1].Append(Value::INTEGER(row.idx));
		output.data[2].Append(Value(row.kind));
		output.data[3].Append(Value(row.status));
		output.data[4].Append(Value(row.saved_at).DefaultCastAs(LogicalType::TIMESTAMP));
		output.data[5].Append(Value::BIGINT(row.bytes));
	}
}

struct ClearBindData : public TableFunctionData {
	string path;
};

unique_ptr<FunctionData> ClearBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<ClearBindData>();
	result->path = SourceArgument(input, 0, "notebook_cache_clear");
	names.emplace_back("removed");
	return_types.push_back(LogicalType::BIGINT);
	return std::move(result);
}

void ClearScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<CacheScanState>();
	if (state.next++ > 0) {
		return;
	}
	ResultCacheStore store(DatabaseInstance::GetDatabase(context), data.bind_data->Cast<ClearBindData>().path, false,
	                       CurrentDatabase(context));
	output.data[0].Append(Value::BIGINT(NumericCast<int64_t>(store.Clear())));
}

} // namespace

void RegisterNotebookCache(ExtensionLoader &loader) {
	loader.RegisterFunction(TableFunction("notebook_cache", {LogicalType::VARCHAR}, CacheScan, CacheBind, CacheInit));
	loader.RegisterFunction(
	    TableFunction("notebook_cache_clear", {LogicalType::VARCHAR}, ClearScan, ClearBind, CacheInit));
}

} // namespace duckdb
