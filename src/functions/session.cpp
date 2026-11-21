#include "notebook/session.hpp"

#include "duckdb/common/chrono.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/main/settings.hpp"
#include "notebook/chart.hpp"
#include "notebook/functions.hpp"
#include "notebook/script.hpp"
#include "notebook/sql_lexer.hpp"
#include "notebook/yaml.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "duckdb/common/string_util.hpp"

namespace duckdb {

using notebook::Json;

const char *const VALUE_OPEN = "\xEE\x80\x80";
const char *const VALUE_CLOSE = "\xEE\x80\x81";

idx_t CellRows(const notebook::Cell &cell) {
	auto rows = cell.Option("rows");
	return rows.empty() ? 0 : NumericCast<idx_t>(std::atoi(rows.c_str()));
}

namespace {

//! Ends every query of a runner's connection by the caller's deadline at the latest (Runner): DuckDB sets each query's
//! deadline from max_execution_time as the query begins (ClientContext::BeginQueryInternal), then calls QueryBegin on
//! every registered state, so this has the last word whatever a cell SETs. Internal DuckDB API: recheck that order when
//! DuckDB is updated (notebook_run.test, the same-cell SET max_execution_time = 0 case, fails if it changes).
class CallerDeadline : public ClientContextState {
public:
	explicit CallerDeadline(idx_t deadline) : deadline(deadline) {
	}
	void QueryBegin(ClientContext &context) override {
		if (!context.query_deadline.IsValid() || context.query_deadline.GetIndex() > deadline) {
			context.query_deadline = deadline;
		}
	}

private:
	idx_t deadline;
};

//! A float as the double of its shortest decimal, so 0.1f travels as 0.1 and not 0.10000000149011612.
double FloatAsDouble(float value) {
	if (!std::isfinite(value)) {
		return static_cast<double>(value);
	}
	char buf[32];
	for (int precision = 6; precision <= 9; precision++) {
		snprintf(buf, sizeof(buf), "%.*g", precision, static_cast<double>(value));
		if (precision == 9 || std::strtof(buf, nullptr) == value) {
			break;
		}
	}
	return std::strtod(buf, nullptr);
}

} // namespace

Json ValueToJson(const Value &value, bool exact_decimal) {
	if (value.IsNull()) {
		return Json::Null();
	}
	auto &type = value.type();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return Json::Bool(value.GetValue<bool>());
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::DOUBLE:
		// Infinity and NaN are written as the strings "Infinity", "-Infinity" and "NaN" (Json::Dump).
		return Json::Number(value.GetValue<double>());
	case LogicalTypeId::FLOAT:
		return Json::Number(FloatAsDouble(value.GetValue<float>()));
	case LogicalTypeId::DECIMAL:
		// A double cannot hold every DECIMAL: tables get the exact digits as text. Chart data, which
		// only needs to be drawn, stays numeric.
		return exact_decimal ? Json::String(value.ToString()) : Json::Number(value.GetValue<double>());
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT: {
		// Beyond 2^53 a double loses digits, so very large integers travel as text.
		auto as_double = value.GetValue<double>();
		if (std::fabs(as_double) < 9007199254740992.0) {
			return Json::Number(as_double);
		}
		return Json::String(value.ToString());
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY: {
		auto result = Json::Array();
		auto &children =
		    type.id() == LogicalTypeId::LIST ? ListValue::GetChildren(value) : ArrayValue::GetChildren(value);
		for (auto &child : children) {
			result.Push(ValueToJson(child, exact_decimal));
		}
		return result;
	}
	case LogicalTypeId::STRUCT: {
		auto result = Json::Object();
		auto &children = StructValue::GetChildren(value);
		for (idx_t i = 0; i < children.size(); i++) {
			result.Set(StructType::GetChildName(type, i).GetIdentifierName(), ValueToJson(children[i], exact_decimal));
		}
		return result;
	}
	default:
		return Json::String(value.ToString());
	}
}

namespace {

template <class RESULT>
Json Columns(RESULT &result) {
	auto columns = Json::Array();
	for (idx_t i = 0; i < result.ColumnCount(); i++) {
		columns.Push(Json::Object()
		                 .Set("name", Json::String(result.ColumnName(i).GetIdentifierName()))
		                 .Set("type", Json::String(result.GetTypes()[i].ToString())));
	}
	return columns;
}

//! Adds the rows of `chunk` to `rows` while there are fewer than `limit`, and counts them all in `total`.
void AppendRows(DataChunk &chunk, idx_t limit, bool exact, Json &rows, int64_t &total) {
	for (idx_t r = 0; r < chunk.size(); r++, total++) {
		if (NumericCast<idx_t>(total) >= limit) {
			continue;
		}
		auto row = Json::Array();
		for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
			row.Push(ValueToJson(chunk.GetValue(c, r), exact));
		}
		rows.Push(std::move(row));
	}
}

template <class RESULT>
Json PageJson(RESULT &result, Json rows, idx_t limit, int64_t total) {
	return Json::Object()
	    .Set("columns", Columns(result))
	    .Set("rows", std::move(rows))
	    .Set("total", Json::Number(static_cast<double>(total)))
	    .Set("truncated", Json::Bool(NumericCast<idx_t>(total) > limit));
}

//! Up to `limit` rows of a result as JSON; reports in `total` how many rows there were.
Json ResultToJson(QueryResult &result, idx_t limit, int64_t &total, bool exact = true) {
	auto rows = Json::Array();
	total = 0;
	while (auto chunk = result.Fetch()) {
		AppendRows(*chunk, limit, exact, rows, total);
	}
	return PageJson(result, std::move(rows), limit, total);
}

//! Runs one statement and reads its result as a stream: the first `limit` rows as JSON, all of them
//! counted in `total`, never more than a chunk in memory. Sets `error` if it fails.
Json StreamToJson(Connection &connection, const string &sql, idx_t limit, int64_t &total, string &error) {
	total = 0;
	auto submitted = connection.Submit(sql);
	if (submitted->HasError()) {
		error = submitted->GetError();
		return Json();
	}
	unique_ptr<QueryResultStream<>> stream;
	try {
		stream = make_uniq<QueryResultStream<>>(std::move(submitted));
	} catch (std::exception &) {
		// Not a result that streams: read it whole instead.
		auto result = connection.Query(sql);
		if (result->HasError()) {
			error = result->GetError();
			return Json();
		}
		return ResultToJson(*result, limit, total);
	}
	auto rows = Json::Array();
	try {
		while (auto chunk = stream->Fetch()) {
			if (chunk->size() == 0) {
				break;
			}
			AppendRows(*chunk, limit, true, rows, total);
		}
	} catch (std::exception &ex) {
		error = ErrorData(ex).Message();
		return Json();
	}
	if (stream->HasError()) {
		error = stream->GetError();
		return Json();
	}
	return PageJson(*stream, std::move(rows), limit, total);
}

std::atomic<uint64_t> store_counter {0};

//! Whether `requested` (an attach: path) names the database attached at `existing` (duckdb_databases().path,
//! NULL for in-memory ones).
bool SamePath(FileSystem &fs, const string &requested, const Value &existing) {
	bool in_memory = requested.empty() || requested.compare(0, 8, ":memory:") == 0;
	if (existing.IsNull()) {
		return in_memory;
	}
	if (in_memory) {
		return false;
	}
	// As DuckDB makes a local path absolute (LocalFileSystem::MakePathAbsolute), separators included.
	auto normalize = [&](const string &path) {
		auto expanded = fs.ExpandPath(path);
		if (expanded.find("://") != string::npos) {
			return expanded;
		}
		auto parsed = Path::FromString(expanded);
		return (parsed.IsAbsolute() ? parsed : Path::FromString(FileSystem::GetWorkingDirectory()).Join(parsed))
		    .ToString();
	};
	return normalize(requested) == normalize(existing.ToString());
}

} // namespace

SessionStore::~SessionStore() {
	auto instance = db.lock();
	if (!instance) {
		return; // the database is closing, and the store goes with it
	}
	try {
		Connection connection(*instance);
		auto result = connection.Query("DETACH DATABASE IF EXISTS " + notebook::QuoteIdentifier(name));
		(void)result;
	} catch (...) { // NOLINT: a destructor must not throw
	}
}

Runner::Runner(DatabaseInstance &db, SessionState &state, optional_ptr<ClientContext> caller)
    : state(state), caller(caller) {
	connection = make_uniq<Connection>(db);
	auto &config = ClientConfig::GetConfig(*connection->context);
	for (auto &variable : state.variables) {
		config.SetUserVariable(variable.first, variable.second);
	}
	// Before anything runs on the connection, the replay and the [notebook] cell's ATTACH included.
	CheckCaller();
	if (caller && caller->query_deadline.IsValid()) {
		// Every query the runner's connection starts ends by the caller's deadline at the latest, whatever a cell
		// SETs: the deadline itself, not a duration that each query would get afresh.
		// Once per connection: each Runner has a connection of its own.
		connection->context->registered_state->Insert(
		    "sketchbook_caller_deadline", make_shared_ptr<CallerDeadline>(caller->query_deadline.GetIndex()));
	}
	// The rest of the session is replayed on first use (Restore), so that the caller can register the
	// connection for interrupts and time limits first.
}

void CheckCallerDeadline(ClientContext &context) {
	if (context.IsInterrupted()) {
		throw InterruptException();
	}
	if (!context.query_deadline.IsValid()) {
		return;
	}
	auto now = NumericCast<idx_t>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(steady_clock::now().time_since_epoch()).count());
	if (now >= context.query_deadline.GetIndex()) {
		throw InterruptException(ClientContext::QUERY_DEADLINE_ERROR);
	}
}

void Runner::CheckCaller() {
	if (caller) {
		CheckCallerDeadline(*caller);
	}
}

void Runner::Restore() {
	if (restored) {
		return;
	}
	restored = true;
	// Best effort throughout: a replayed view whose source has since been dropped fails here and the
	// cell reports the problem when it is next run.
	if (state.store) {
		// The table as it was created (keys, defaults, generated columns), then its rows. A table whose statement
		// names another (a foreign key) can be made only after it: what fails is tried again while any succeeds.
		auto create = [&](const string &table) {
			auto ddl = state.table_ddl.find(table);
			if (ddl == state.table_ddl.end() || ddl->second.first.empty()) {
				return false;
			}
			auto quoted = "temp.main." + notebook::QuoteIdentifier(table);
			auto created = connection->Query(ddl->second.first);
			if (!created->HasError()) {
				created.reset();
				auto filled =
				    connection->Query("INSERT INTO " + quoted + " (" + ddl->second.second + ") SELECT " +
				                      ddl->second.second + " FROM " + notebook::StoredTable(state.store->name, table));
				if (!filled->HasError()) {
					return true;
				}
			}
			created.reset();
			auto dropped = connection->Query("DROP TABLE IF EXISTS " + quoted);
			(void)dropped;
			return false;
		};
		vector<string> pending(state.tables.begin(), state.tables.end());
		for (bool progress = true; progress && !pending.empty();) {
			progress = false;
			vector<string> failed;
			for (auto &table : pending) {
				if (create(table)) {
					progress = true;
				} else {
					failed.push_back(table);
				}
			}
			pending = std::move(failed);
		}
		for (auto &table : pending) {
			// Rows only: the statement cannot run here (it uses a temp sequence, which is replayed later).
			auto result = connection->Query("CREATE OR REPLACE TEMP TABLE " + notebook::QuoteIdentifier(table) +
			                                " AS FROM " + notebook::StoredTable(state.store->name, table));
			(void)result;
		}
	}
	for (auto &replay : state.replay) {
		auto result = connection->Query(replay.second);
		(void)result;
	}
}

string Runner::Store() {
	if (state.store) {
		return state.store->name;
	}
	if (!state.keep_tables || state.store_failed) {
		return string();
	}
	for (int attempt = 0; attempt < 8; attempt++) {
		auto name = "_sketchbook_session_" + std::to_string(++store_counter);
		// Hidden: it is not listed by duckdb_databases() or duckdb_tables(), and owned by the database
		// instance rather than by this connection, so it outlives the request.
		auto result = connection->Query("ATTACH ':memory:' AS " + notebook::QuoteIdentifier(name) + " (HIDDEN true)");
		if (!result->HasError()) {
			state.store = make_shared_ptr<SessionStore>();
			state.store->db = connection->context->db;
			state.store->name = name;
			return name;
		}
	}
	// Without a store, the session replays temp tables by running their SQL again, as before.
	state.store_failed = true;
	return string();
}

bool Runner::KeepTempTables(const notebook::Cell &cell) {
	auto store = state.store ? state.store->name : string();
	if (store.empty()) {
		return true;
	}
	std::set<string> own;
	for (auto &name : cell.defines) {
		own.insert(StringUtil::Lower(name));
	}
	// The cell's own CREATE TEMP TABLE statements are replayed instead (Run). The tables they make must then not come
	// from the store as well, or replaying them fails on a table that already exists: none of them is kept.
	auto replay_own = [&]() {
		for (auto it = state.tables.begin(); it != state.tables.end();) {
			if (!own.count(StringUtil::Lower(*it))) {
				++it;
				continue;
			}
			auto dropped = connection->Query("DROP TABLE IF EXISTS " + notebook::StoredTable(store, *it));
			(void)dropped;
			state.table_ddl.erase(*it);
			it = state.tables.erase(it);
		}
		state.replayed.insert(own.begin(), own.end());
		return false;
	};
	auto listed = connection->Query("SELECT table_name, sql FROM duckdb_tables() WHERE database_name = 'temp'");
	if (listed->HasError()) {
		listed.reset();
		return replay_own();
	}
	std::set<string> present;
	std::map<string, string> ddl;
	while (auto chunk = listed->Fetch()) {
		for (idx_t r = 0; r < chunk->size(); r++) {
			auto name = chunk->GetValue(0, r).ToString();
			present.insert(name);
			auto sql = chunk->GetValue(1, r);
			ddl[name] = sql.IsNull() ? string() : sql.ToString();
		}
	}
	listed.reset();
	bool kept_all = true;
	std::set<string> mentioned;
	mentioned.insert(own.begin(), own.end());
	for (auto &name : cell.reads) {
		mentioned.insert(StringUtil::Lower(name));
	}
	// For tests only (test_browser.py): the copy of the table this names fails, as it would if the store ran out of
	// memory. Read once per process; DESIGN §11 names it. It changes nothing unless set.
	static const string fail_copy = StringUtil::Lower(FileSystem::GetEnvVariable("SKETCHBOOK_TEST_FAIL_STORE_COPY"));
	for (auto &table : present) {
		auto lower = StringUtil::Lower(table);
		// A table this cell may have changed, or one that is new: copy it, rows and all.
		if (state.tables.count(table) && !mentioned.count(lower)) {
			continue;
		}
		if (state.replayed.count(lower) && !own.count(lower)) {
			// Another cell's replay makes it again: a copy here would clash with that replay. It keeps only what that
			// cell's SQL makes, as every table did before the store.
			continue;
		}
		auto copied =
		    connection->Query("CREATE OR REPLACE TABLE " + notebook::StoredTable(store, table) + " AS FROM temp.main." +
		                      notebook::QuoteIdentifier(table) +
		                      (!fail_copy.empty() && lower == fail_copy ? " WHERE error('copy failed')" : ""));
		if (copied->HasError()) {
			copied.reset();
			if (own.count(StringUtil::Lower(table))) {
				// The cell made it: its SQL is replayed for it instead (Run), with the cell's other tables.
				kept_all = false;
			}
			// A table an earlier cell made and this one changed keeps its older copy: nothing else could make it
			// again, and an older table is better than none.
			continue;
		}
		copied.reset();
		state.tables.insert(table);
		auto columns = connection->Query("SELECT column_name FROM duckdb_columns() WHERE database_name = 'temp' AND "
		                                 "table_name = " +
		                                 notebook::QuoteString(table) + " AND NOT is_generated ORDER BY column_index");
		string list;
		if (!columns->HasError()) {
			while (auto chunk = columns->Fetch()) {
				for (idx_t r = 0; r < chunk->size(); r++) {
					list += (list.empty() ? "" : ", ") + notebook::QuoteIdentifier(chunk->GetValue(0, r).ToString());
				}
			}
		}
		state.table_ddl[table] = {list.empty() ? string() : ddl[table], list};
	}
	for (auto it = state.tables.begin(); it != state.tables.end();) {
		if (present.count(*it)) {
			++it;
			continue;
		}
		// Dropped by the cell.
		auto dropped = connection->Query("DROP TABLE IF EXISTS " + notebook::StoredTable(store, *it));
		(void)dropped;
		state.table_ddl.erase(*it);
		it = state.tables.erase(it);
	}
	if (!kept_all) {
		return replay_own();
	}
	for (auto &name : own) {
		// Copied this time: the store has them again, and the cell's replay leaves them out.
		state.replayed.erase(name);
	}
	return true;
}

Runner::~Runner() {
	try {
		Save();
	} catch (...) { // NOLINT: a destructor must not throw
	}
}

void Runner::Save() {
	state.variables.clear();
	auto &config = ClientConfig::GetConfig(*connection->context);
	for (auto &variable : config.user_variables) {
		state.variables[variable.first.GetIdentifierName()] = variable.second;
	}
}

void Runner::ApplyMetadata(const notebook::Notebook &notebook, const string &source) {
	for (auto &cell : notebook.cells) {
		if (cell.kind != notebook::CellKind::NOTEBOOK) {
			continue;
		}
		string error;
		auto meta = notebook::ParseYaml(cell.content, error);
		auto run = [&](const string &sql) {
			auto result = connection->Query(sql);
			if (result->HasError()) {
				throw InvalidInputException("notebook metadata: %s", result->GetError());
			}
		};
		if (auto extensions = meta.Get("extensions")) {
			for (auto &name : extensions->Strings()) {
				auto result = connection->Query("LOAD " + notebook::QuoteIdentifier(name));
				if (result->HasError()) {
					// Usually the notebook was opened in a DuckDB without that extension installed.
					throw InvalidInputException(
					    "the [notebook] cell loads extension '%s', which this DuckDB cannot load. Open the notebook in "
					    "a DuckDB where LOAD %s works (INSTALL it, or start that DuckDB with -unsigned for a "
					    "community or local build). DuckDB said: %s",
					    name, name, result->GetError());
				}
			}
		}
		auto attach = meta.Get("attach");
		if (!attach || !attach->IsList()) {
			continue;
		}
		// Both separators: a Windows notebook path (C:\nb\x.sql) has its folder too.
		auto slash = source.find_last_of("/\\");
		auto base = notebook::IsStoredSource(source) || slash == string::npos ? string() : source.substr(0, slash + 1);
		for (auto &entry : attach->items) {
			auto path = entry.IsMap() ? entry.Str("path") : entry.scalar;
			if (path.empty()) {
				continue;
			}
			auto &fs = FileSystem::GetFileSystem(*connection->context);
			// `:memory:`, `md:x` and URLs are not relative paths either.
			bool relative =
			    !fs.IsPathAbsolute(path) && path[0] != '/' && path[0] != '\\' && path.find(':') == string::npos;
			auto full = relative ? base + path : path;
			auto alias = entry.IsMap() ? entry.Str("as") : string();
			if (alias.empty()) {
				// The name ATTACH would give it.
				alias = AttachedDatabase::ExtractDatabaseName(full, fs).GetIdentifierName();
			}
			bool read_only = entry.IsMap() && entry.Str("read_only") == "true";
			// Without READ_ONLY, an attach takes the database's own access mode.
			bool expect_read_only =
			    read_only || DBConfig::GetConfig(*connection->context).options.access_mode == AccessMode::READ_ONLY;
			auto existing = connection->Query("SELECT path, readonly FROM duckdb_databases() WHERE database_name = " +
			                                  notebook::QuoteString(alias));
			if (existing->HasError()) {
				throw InvalidInputException("notebook metadata: %s", existing->GetError());
			}
			auto found = existing->Fetch();
			if (found && found->size() > 0) {
				// Already attached, by an earlier request of this session or by someone else: use it only if
				// it is the same database in the same mode, and leave it attached.
				auto existing_path = found->GetValue(0, 0);
				bool existing_read_only = found->GetValue(1, 0).GetValue<bool>();
				existing.reset();
				if (!SamePath(fs, full, existing_path)) {
					throw InvalidInputException(
					    "the [notebook] cell attaches '%s' as %s, but %s is already attached to %s. DETACH %s first, "
					    "or give this one another name with as:",
					    full, alias, alias,
					    existing_path.IsNull() ? string("an in-memory database") : "'" + existing_path.ToString() + "'",
					    alias);
				}
				if (existing_read_only != expect_read_only) {
					throw InvalidInputException(
					    "the [notebook] cell attaches '%s' as %s%s, but %s is already attached %s. DETACH %s first, "
					    "or give this one another name with as:",
					    full, alias, read_only ? " read-only" : "", alias,
					    existing_read_only ? "read-only" : "for writing", alias);
				}
				continue;
			}
			existing.reset();
			auto sql = "ATTACH " + notebook::QuoteString(full) + " AS " + notebook::QuoteIdentifier(alias);
			if (read_only) {
				sql += " (READ_ONLY)";
			}
			run(sql);
			attached.push_back(alias);
		}
	}
}

void Runner::Prepare(const notebook::Notebook &notebook, const string &source, const std::map<string, Value> &params) {
	CheckCaller();
	ApplyMetadata(notebook, source);
	InitializeVariables(notebook, params);
}

void Runner::RunAll(const notebook::Notebook &notebook, const RunnerOptions &options) {
	for (auto index : notebook::RunOrder(notebook)) {
		Run(notebook, notebook.cells[NumericCast<idx_t>(index)], options);
	}
}

void DetachDatabases(DatabaseInstance &db, const vector<string> &names) {
	if (names.empty()) {
		return;
	}
	// A connection of its own: the runner's may have USEd one of them, and the default database
	// cannot be detached.
	Connection connection(db);
	for (auto &name : names) {
		auto result = connection.Query("DETACH DATABASE IF EXISTS " + notebook::QuoteIdentifier(name));
		(void)result;
	}
}

DetachOnExit::~DetachOnExit() {
	try {
		DetachDatabases(db, names);
	} catch (...) { // NOLINT: a destructor must not throw
	}
}

void PrepareOnce(Runner &runner, DetachOnExit &detach, const notebook::Notebook &notebook, const string &source,
                 const std::map<string, Value> &params) {
	try {
		runner.Prepare(notebook, source, params);
	} catch (...) {
		detach.names = runner.Attached();
		throw;
	}
	detach.names = runner.Attached();
}

void Runner::InitializeVariables(const notebook::Notebook &notebook, const std::map<string, Value> &params) {
	Restore();
	auto &config = ClientConfig::GetConfig(*connection->context);
	for (auto &name : notebook::UndefinedVariables(notebook)) {
		auto param = params.find(name);
		if (param != params.end()) {
			config.SetUserVariable(name, param->second);
		} else if (state.variables.find(name) == state.variables.end()) {
			config.SetUserVariable(name, Value());
		}
	}
}

Json Runner::Page(const string &sql, idx_t limit, string &error, bool exact) {
	Restore();
	auto result = connection->Query(sql);
	if (result->HasError()) {
		error = result->GetError();
		return Json();
	}
	int64_t total;
	return ResultToJson(*result, limit, total, exact);
}

Json Runner::Peek(const string &sql, idx_t cursor, idx_t rows, idx_t timeout_ms) {
	auto nothing = [](const string &reason) {
		return Json::Object().Set("reason", Json::String(reason));
	};
	auto &context = *connection->context;
	auto run = [&](const string &statement) {
		auto result = connection->Query(statement);
		return !result->HasError();
	};
	// The time limit comes first: it also bounds replaying the session.
	if (!run("SET SESSION max_execution_time = " + std::to_string(MaxValue<idx_t>(timeout_ms, 1)))) {
		return nothing("a time limit could not be set");
	}
	Restore();
	PeekTarget target;
	// Finding the part reads the catalog (are the functions it calls safe?), which needs a transaction.
	context.RunFunctionInTransaction([&]() { target = FindPeekTarget(context, sql, cursor); });
	if (target.sql.empty()) {
		return nothing(target.reason);
	}
	// Three guards on top of FindPeekTarget's: a time limit (set above), a transaction that cannot
	// write, and a rollback whatever happens. The limit is this connection's own setting and goes with it.
	if (!run("BEGIN TRANSACTION READ ONLY")) {
		return nothing("a read-only transaction could not be started");
	}
	string error;
	auto page = Page(target.sql + " LIMIT " + std::to_string(rows + 1), rows, error);
	Json total;
	if (error.empty()) {
		auto fetched = static_cast<idx_t>(page.Get("total")->Num());
		if (fetched <= rows) {
			total = Json::Number(static_cast<double>(fetched));
		} else {
			// More rows than shown: count them if that is quick, and say nothing if it is not.
			run("SET SESSION max_execution_time = " + std::to_string(MaxValue<idx_t>(timeout_ms / 3, 1)));
			auto counted = connection->Query("SELECT count(*) FROM (" + target.sql + ")");
			if (!counted->HasError()) {
				if (auto chunk = counted->Fetch()) {
					total = Json::Number(static_cast<double>(chunk->GetValue(0, 0).GetValue<int64_t>()));
				}
			}
		}
	}
	run("ROLLBACK");
	if (!error.empty()) {
		return nothing(error);
	}
	return Json::Object()
	    .Set("kind", Json::String(target.kind))
	    .Set("name", Json::String(target.name))
	    .Set("start", Json::Number(static_cast<double>(target.start)))
	    .Set("end", Json::Number(static_cast<double>(target.end)))
	    .Set("columns", *page.Get("columns"))
	    .Set("rows", *page.Get("rows"))
	    .Set("more", *page.Get("truncated"))
	    .Set("total", total);
}

bool Runner::ChartSource(const notebook::Cell &cell, int64_t row_count, ChartInput &input) {
	Restore();
	auto relation = notebook::RelationSql(notebook::ChartFrom(cell));
	input.columns.clear();
	if (!DescribeColumns(*connection, relation, input.columns, input.error)) {
		return false;
	}
	input.row_count = row_count;
	if (input.row_count < 0) {
		auto counted = connection->Query("SELECT count(*) FROM " + relation);
		if (!counted->HasError()) {
			if (auto chunk = counted->Fetch()) {
				input.row_count = chunk->GetValue(0, 0).GetValue<int64_t>();
			}
		}
	}
	return true;
}

Json Runner::Chart(const notebook::Notebook &notebook, const notebook::Cell &cell, const string &spec) {
	ChartInput input;
	if (!ChartSource(cell, -1, input)) {
		return Json::Object().Set("error", Json::String(input.error));
	}
	return Chart(notebook, cell, spec, input);
}

Json Runner::Chart(const notebook::Notebook &notebook, const notebook::Cell &cell, const string &spec,
                   const ChartInput &input) {
	auto from = notebook::ChartFrom(cell);
	notebook::ChartContext context;
	context.width = std::atoi(cell.Option("width", "640").c_str());
	context.height = std::atoi(cell.Option("height", "320").c_str());
	context.columns = input.columns;
	if (input.row_count >= 0) {
		context.row_count = input.row_count;
	}
	context.selections = notebook::NotebookSelections(notebook);
	auto plan = notebook::CompileChart(spec, from, context);
	if (!plan.error.empty()) {
		return Json::Object().Set("error", Json::String(plan.error));
	}
	auto datasets = Json::Array();
	for (auto &sql : plan.queries) {
		string error;
		// Chart data is drawn, not read digit by digit: DECIMAL stays numeric.
		auto page = Page(sql, 100000, error, false);
		if (!error.empty()) {
			return Json::Object().Set("error", Json::String(error)).Set("sql", Json::String(sql));
		}
		datasets.Push(std::move(page));
	}
	auto queries = Json::Array();
	for (auto &sql : plan.queries) {
		queries.Push(Json::String(sql));
	}
	return Json::Object().Set("render", plan.render).Set("datasets", datasets).Set("queries", queries);
}

Json Runner::Charts(const notebook::Notebook &notebook, const notebook::Cell &cell, string &error) {
	return Charts(notebook, cell, -1, error);
}

Json Runner::Charts(const notebook::Notebook &notebook, const notebook::Cell &cell, int64_t row_count, string &error) {
	auto charts = Json::Array();
	// Every chart of a cell reads the same relation: describe and count it once.
	ChartInput input;
	bool described = ChartSource(cell, row_count, input);
	for (idx_t i = 0; i < cell.charts.size(); i++) {
		auto chart = described ? Chart(notebook, cell, cell.charts[i], input)
		                       : Json::Object().Set("error", Json::String(input.error));
		if (auto message = chart.Get("error")) {
			if (error.empty()) {
				error =
				    cell.charts.size() > 1 ? "chart " + std::to_string(i + 1) + ": " + message->Str() : message->Str();
			}
		}
		charts.Push(std::move(chart));
	}
	return charts;
}

CellOutcome Runner::Run(const notebook::Notebook &notebook, const notebook::Cell &cell, const RunnerOptions &options,
                        Json *data) {
	CheckCaller();
	Restore();
	CellOutcome outcome;
	auto start = steady_clock::now();
	auto finish = [&]() {
		outcome.elapsed_ms = std::chrono::duration<double, std::milli>(steady_clock::now() - start).count();
	};
	auto &config = ClientConfig::GetConfig(*connection->context);

	if (cell.kind == notebook::CellKind::INPUT) {
		auto param = options.params.find(cell.name);
		if (param != options.params.end()) {
			// A parameter replaces the input's default.
			config.SetUserVariable(cell.name, param->second);
			outcome.status = "ok";
			finish();
			return outcome;
		}
	}
	if (cell.kind == notebook::CellKind::MARKDOWN) {
		string rendered;
		size_t copied = 0;
		for (auto &expression : cell.expressions) {
			auto result =
			    connection->Query("SELECT (" + expression.sql + "\n)"); // a trailing -- comment ends at the newline
			if (result->HasError()) {
				outcome.status = "error";
				outcome.error = "{{ " + expression.sql + " }}: " + result->GetError();
				finish();
				return outcome;
			}
			auto chunk = result->Fetch();
			auto value = chunk && chunk->size() > 0 ? chunk->GetValue(0, 0) : Value();
			rendered += cell.content.substr(copied, expression.begin - copied);
			auto text = value.IsNull() ? string("NULL") : value.ToString();
			rendered += options.mark_values ? VALUE_OPEN + text + VALUE_CLOSE : text;
			copied = expression.end;
		}
		outcome.output = rendered + cell.content.substr(copied);
		outcome.has_output = true;
		if (!cell.expressions.empty()) {
			outcome.status = "ok";
			finish();
		}
		return outcome;
	}
	if (cell.kind == notebook::CellKind::CHART) {
		if (options.charts) {
			auto chart = Chart(notebook, cell, cell.content);
			if (auto error = chart.Get("error")) {
				outcome.status = "error";
				outcome.error = error->Str();
			} else {
				outcome.status = "ok";
				auto datasets = chart.Get("datasets");
				if (datasets && datasets->Size() > 0) {
					outcome.row_count = static_cast<int64_t>(datasets->Items()[0].Get("rows")->Size());
				}
			}
			if (data) {
				*data = std::move(chart);
			}
			finish();
		}
		return outcome;
	}

	auto cell_rows = CellRows(cell);
	idx_t page_rows = options.page_rows;
	if (cell_rows > 0) {
		page_rows = options.rows_at_least ? MaxValue(page_rows, cell_rows) : cell_rows;
	}
	bool is_query = cell.kind == notebook::CellKind::SQL && cell.is_query;
	bool materialized = is_query && !cell.name.empty() && cell.Option("materialize", "view") == "table";
	bool may_write = notebook::MayWrite(cell);
	// A long-lived session keeps what such a cell makes in its store (SessionState::keep_tables).
	auto store = materialized || may_write ? Store() : (state.store ? state.store->name : string());
	auto sql = notebook::CellSql(cell, store);
	if (sql.empty()) {
		return outcome;
	}
	bool failed = false;
	string message;
	if (is_query && cell.name.empty()) {
		// Statements before the query, then the query itself as a stream: its rows are counted and the
		// first page kept without holding the whole result.
		auto statements = notebook::SplitStatements(cell.sql);
		string leading;
		for (idx_t i = 0; i + 1 < statements.size(); i++) {
			leading += statements[i].text + ";\n";
		}
		if (!leading.empty()) {
			auto result = connection->Query(leading);
			failed = result->HasError();
			message = failed ? result->GetError() : string();
		}
		if (!failed) {
			int64_t total;
			auto page = StreamToJson(*connection, statements.back().text, data ? page_rows : 0, total, message);
			failed = !message.empty();
			if (!failed) {
				outcome.row_count = total;
				if (data && page_rows > 0) {
					*data = std::move(page);
				}
			}
		}
	} else {
		auto result = connection->Query(sql);
		failed = result->HasError();
		message = failed ? result->GetError() : string();
		// A result keeps its connection busy until it is released.
		result.reset();
	}
	if (!failed && is_query && !cell.name.empty()) {
		// Creating the view only binds the query; reading it is what proves it runs. The count is the
		// one execution every named query costs; the page reads only its first rows.
		auto count = connection->Query("SELECT count(*) FROM " + notebook::QuoteIdentifier(cell.name));
		failed = count->HasError();
		if (failed) {
			message = count->GetError();
		} else if (auto chunk = count->Fetch()) {
			outcome.row_count = chunk->GetValue(0, 0).GetValue<int64_t>();
		}
		count.reset();
		if (!failed && data && page_rows > 0) {
			string error;
			*data = Page(notebook::DisplaySql(cell) + " LIMIT " + std::to_string(page_rows), page_rows, error);
			if (error.empty() && outcome.row_count >= 0) {
				data->Set("total", Json::Number(static_cast<double>(outcome.row_count)));
				data->Set("truncated", Json::Bool(NumericCast<idx_t>(outcome.row_count) > page_rows));
			}
		}
	}
	bool kept = true;
	if (may_write) {
		// Even after a failure: the store must hold what the connection has.
		kept = KeepTempTables(cell);
	}
	if (!failed && options.charts && !cell.charts.empty()) {
		// The query ran; a chart that fails is reported without hiding the result.
		outcome.charts = Charts(notebook, cell, outcome.row_count, message);
		failed = !message.empty();
	}
	outcome.status = failed ? "error" : "ok";
	outcome.error = message;
	if (!failed || !outcome.charts.IsNull()) {
		// A temp table that could not be copied into the store is made again by running its SQL.
		auto replay = notebook::ReplaySql(cell, kept ? store : string());
		if (replay.empty()) {
			state.replay.erase(cell.index);
		} else {
			state.replay[cell.index] = replay;
		}
		// A table this cell kept in the store under another name (it was renamed, or no longer
		// materialized) is no longer read by anything.
		auto kept = state.stored.find(cell.index);
		if (kept != state.stored.end() && (!materialized || kept->second != cell.name) && state.store) {
			auto dropped = connection->Query("DROP TABLE IF EXISTS " + notebook::StoredTable(store, kept->second));
			(void)dropped;
			state.stored.erase(kept);
		}
		if (materialized && !store.empty()) {
			state.stored[cell.index] = cell.name;
		}
	}
	finish();
	return outcome;
}

bool DescribeColumns(Connection &connection, const string &relation, std::vector<notebook::ColumnInfo> &columns,
                     string &error) {
	auto described = connection.Query("DESCRIBE SELECT * FROM " + relation);
	if (described->HasError()) {
		error = described->GetError();
		return false;
	}
	while (auto chunk = described->Fetch()) {
		for (idx_t r = 0; r < chunk->size(); r++) {
			columns.push_back({chunk->GetValue(0, r).ToString(), chunk->GetValue(1, r).ToString()});
		}
	}
	return true;
}

static string ColumnType(Connection &connection, const string &relation, const string &column) {
	std::vector<notebook::ColumnInfo> columns;
	string error;
	DescribeColumns(connection, relation, columns, error);
	for (auto &info : columns) {
		if (StringUtil::CIEquals(info.name, column)) {
			return info.type;
		}
	}
	return string();
}

string VariableSqlType(Connection &connection, const notebook::Notebook &notebook, const notebook::Cell &cell,
                       const string &name) {
	if (cell.kind == notebook::CellKind::INPUT) {
		auto &w = cell.widget;
		if (w == "text") {
			return "VARCHAR";
		}
		if (w == "number" || w == "slider") {
			return "DOUBLE";
		}
		if (w == "checkbox") {
			return "BOOLEAN";
		}
		if (w == "date" || w == "daterange") {
			return "DATE";
		}
		string relation, column;
		if (cell.InputFrom(relation, column)) {
			auto type = ColumnType(connection, notebook::RelationSql(relation), column);
			return type.empty() ? type : (w == "multiselect" ? type + "[]" : type);
		}
		return string();
	}
	for (auto &spec : cell.charts) {
		auto selection = notebook::ChartSelectionInfo(spec);
		if (selection.name != name) {
			continue;
		}
		auto relation = notebook::RelationSql(notebook::ChartFrom(cell));
		vector<string> types;
		for (auto &field : selection.fields) {
			auto type = ColumnType(connection, relation, field);
			// A brush on a date axis lands between days; compare as timestamps.
			types.push_back(type == "DATE" ? "TIMESTAMP" : type);
		}
		if (selection.type == "point") {
			return types.empty() || types[0].empty() ? string() : types[0] + "[]";
		}
		if (types.size() >= 2) {
			string type = "STRUCT(";
			for (idx_t i = 0; i < types.size(); i++) {
				auto inner = types[i].empty() ? "VARCHAR" : types[i];
				type += (i ? ", " : "") + selection.channels[i] + " STRUCT(lo " + inner + ", hi " + inner + ")";
			}
			return type + ")";
		}
		if (types.empty() || types[0].empty()) {
			return string();
		}
		return "STRUCT(lo " + types[0] + ", hi " + types[0] + ")";
	}
	return string();
}

CellOutcome RunForEditor(Runner &runner, const notebook::Notebook &notebook, const notebook::Cell &cell,
                         const RunnerOptions &options, Json &result) {
	Json data;
	auto outcome = runner.Run(notebook, cell, options, &data);
	result.Set("status", Json::String(outcome.status));
	result.Set("error", outcome.status == "error" ? Json::String(outcome.error) : Json());
	result.Set("row_count", outcome.row_count >= 0 ? Json::Number(static_cast<double>(outcome.row_count)) : Json());
	result.Set("elapsed_ms", outcome.elapsed_ms >= 0 ? Json::Number(outcome.elapsed_ms) : Json());
	if (outcome.has_output) {
		result.Set("output", Json::String(outcome.output));
	}
	if (!data.IsNull()) {
		result.Set("data", data);
	}
	if (!outcome.charts.IsNull()) {
		result.Set("charts", outcome.charts);
	}
	if (cell.kind == notebook::CellKind::INPUT) {
		auto value = runner.Conn().Query("SELECT getvariable(" + notebook::QuoteString(cell.name) + ")");
		if (!value->HasError()) {
			if (auto chunk = value->Fetch()) {
				result.Set("value", ValueToJson(chunk->GetValue(0, 0)));
			}
		}
	}
	if (cell.kind == notebook::CellKind::INPUT && (cell.widget == "select" || cell.widget == "multiselect")) {
		auto choices = Json::Array();
		for (auto &choice : InputChoices(runner.Conn(), cell, 1000)) {
			choices.Push(ValueToJson(choice));
		}
		result.Set("choices", choices);
	}
	return outcome;
}

} // namespace duckdb
