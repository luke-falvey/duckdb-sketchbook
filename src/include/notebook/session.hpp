#pragma once

#include "duckdb.hpp"
#include "notebook/chart.hpp"
#include "notebook/format.hpp"
#include "notebook/json.hpp"

#include <map>
#include <set>

// Running cells, shared by notebook_run, notebook_export and the server.
//
// A Session holds no Connection. Its state is the variables it has set and the statements that
// rebuild its per-connection objects (temp views of query cells, temp tables, settings). A Runner
// opens a connection, replays that state, runs cells, and records the new state when it goes
// away. The extension therefore never keeps the database alive between requests (DESIGN §11).
//
// A long-lived session (the server's, `keep_tables`) also has a store: an in-memory database attached
// to the DatabaseInstance, hidden from listings. Its materialize=table cells create their tables
// there, and the temp tables its cells make are copied there after each cell that may change them,
// so a new connection reads them instead of running their SQL again (DESIGN §12).
namespace duckdb {

//! A session's store: `ATTACH ':memory:' AS <name> (HIDDEN true)`, owned by the database instance.
//! Detached when the last SessionState that shares it goes away, if the database is still open.
struct SessionStore {
	weak_ptr<DatabaseInstance> db;
	string name;
	~SessionStore();
};

struct SessionState {
	//! User variables, as they were when the last runner finished.
	std::map<string, Value> variables;
	//! Per cell index: the SQL that recreates what that cell made which a new connection lacks.
	std::map<int, string> replay;
	//! Set for a session that outlives its requests (the server's): keep tables in a store.
	bool keep_tables = false;
	//! The store, created by the first cell that needs it. Copies of the state (a peek's) share it.
	shared_ptr<SessionStore> store;
	//! The store could not be attached; temp tables are replayed by running their SQL again.
	bool store_failed = false;
	//! Temp tables with a copy in the store, restored into every new connection.
	std::set<string> tables;
	//! For each of `tables`: the statement that created it as DuckDB writes it (duckdb_tables().sql), and the
	//! columns that hold data (not generated), quoted: restored as the same table, constraints and defaults too.
	std::map<string, std::pair<string, string>> table_ddl;
	//! Temp tables (lower case) that their cell's replay makes again, because a copy into the store failed. No other
	//! cell copies them into the store, which would clash with that replay.
	std::set<string> replayed;
	//! Per cell index: the materialize=table cell's table in the store.
	std::map<int, string> stored;
};

//! What every chart of a cell needs to know about the relation it reads (Runner::Charts).
struct ChartInput {
	std::vector<notebook::ColumnInfo> columns;
	int64_t row_count = -1;
	string error;
};

struct CellOutcome {
	//! `ok`, `error` or `skipped`.
	string status = "skipped";
	string error;
	//! Rows a query cell returns, or -1.
	int64_t row_count = -1;
	double elapsed_ms = -1;
	//! Markdown cells: the rendered text. `has_output` tells an empty render from none.
	string output;
	bool has_output = false;
	//! Query cells with charts, when charts are requested: one Runner::Chart result per chart.
	notebook::Json charts;
};

struct RunnerOptions {
	//! Rows of each query cell's result to keep, 0 for none. A cell's rows= option replaces it, or
	//! with rows_at_least (exports, which page on the client) raises it.
	idx_t page_rows = 0;
	bool rows_at_least = false;
	//! Wrap each markdown {{ }} value in U+E000 ... U+E001 so a renderer can highlight it.
	bool mark_values = false;
	//! Compile and fetch chart data.
	bool charts = false;
	//! Parameters by variable name (notebook_run / export params).
	std::map<string, Value> params;
};

class Runner {
public:
	//! `caller`: the context of the SQL call a one-off run (notebook_run, an export) belongs to. Its interrupt and its
	//! query's deadline (max_execution_time) then bound the run: checked before each cell, and every query of the
	//! runner's connection ends by that deadline at the latest, whatever a cell SETs.
	Runner(DatabaseInstance &db, SessionState &state, optional_ptr<ClientContext> caller = nullptr);
	~Runner();

	//! The connection, with the session restored (Restore).
	Connection &Conn() {
		Restore();
		return *connection;
	}

	//! The connection's context before anything has run on it: register it for interrupts first.
	shared_ptr<ClientContext> Context() {
		return connection->context;
	}

	//! Replays the session into the connection: temp tables from the store, then every cell's replay.
	//! Runs once, on first use of the connection; never before Context() can be registered.
	void Restore();

	//! The databases ApplyMetadata attached (not those it found attached already).
	const vector<string> &Attached() const {
		return attached;
	}

	//! Applies the [notebook] cell: loads its `extensions` and attaches its `attach` databases
	//! (paths relative to the notebook file). Throws if one fails, or if its name is already attached
	//! to another database or in another mode; one that matches is used as it is.
	void ApplyMetadata(const notebook::Notebook &notebook, const string &source);

	//! Sets variables that cells read but nothing sets to NULL, or to their param.
	void InitializeVariables(const notebook::Notebook &notebook, const std::map<string, Value> &params);

	//! Before the first cell of a run: ApplyMetadata, then InitializeVariables.
	void Prepare(const notebook::Notebook &notebook, const string &source, const std::map<string, Value> &params);

	//! Runs every cell in run order (notebook::RunOrder), keeping no outcomes.
	void RunAll(const notebook::Notebook &notebook, const RunnerOptions &options);

	//! Runs one cell. Fills `data` with the result page (query cells) or chart data (chart cells)
	//! as requested in the options.
	CellOutcome Run(const notebook::Notebook &notebook, const notebook::Cell &cell, const RunnerOptions &options,
	                notebook::Json *data = nullptr);

	//! `{columns: [{name, type}], rows: [[...]], truncated}` for a query, at most `limit` rows.
	//! DECIMAL values are exact strings unless `exact` is false.
	notebook::Json Page(const string &sql, idx_t limit, string &error, bool exact = true);

	//! `{render, datasets: [...], queries: [...]}` for one of a cell's charts, or `{error}`.
	notebook::Json Chart(const notebook::Notebook &notebook, const notebook::Cell &cell, const string &spec);

	//! Every chart of a cell, in order. Sets `error` to the first chart's error, if any.
	notebook::Json Charts(const notebook::Notebook &notebook, const notebook::Cell &cell, string &error);
	//! As above, with the relation's row count when it is known (-1 counts it).
	notebook::Json Charts(const notebook::Notebook &notebook, const notebook::Cell &cell, int64_t row_count,
	                      string &error);

	//! Peek under the pen: the first `rows` rows of the CTE or sub-query around byte offset `cursor`
	//! of `sql`, as `{kind, name, start, end, columns, rows, more, total}` (`total` only when counting
	//! was cheap), or `{reason}` when there is nothing to show. Read-only: it runs one SELECT in a
	//! read-only transaction with a time limit, and creates, replaces and records nothing.
	notebook::Json Peek(const string &sql, idx_t cursor, idx_t rows, idx_t timeout_ms);

	//! Records the connection's variables into the state (also done by the destructor).
	void Save();

	//! Throws if the caller was interrupted or its deadline has passed. Nothing without a caller. Runs before the
	//! replay, Prepare and each cell; a one-off run calls it once more after its last cell, so a run that took past
	//! the deadline fails whichever cell ran out of time.
	void CheckCaller();

private:
	//! The store's name, attaching it if the session keeps tables and has none yet; empty without one.
	string Store();
	//! After a cell that may change tables: copies the temp tables it made or names into the store,
	//! and forgets those that are gone.
	//! False if a table the cell made could not be copied: the cell's CREATE TEMP TABLE statements are then replayed
	//! instead, and none of its tables comes from the store. A table an earlier cell made that cannot be copied
	//! again keeps its older copy.
	bool KeepTempTables(const notebook::Cell &cell);
	//! Describes (and, unless `row_count` is known, counts) the relation a cell's charts read.
	bool ChartSource(const notebook::Cell &cell, int64_t row_count, ChartInput &input);
	notebook::Json Chart(const notebook::Notebook &notebook, const notebook::Cell &cell, const string &spec,
	                     const ChartInput &input);

	SessionState &state;
	optional_ptr<ClientContext> caller;
	unique_ptr<Connection> connection;
	bool restored = false;
	vector<string> attached;
};

//! Throws (InterruptException) if `context` was interrupted or its query's deadline (max_execution_time) has passed.
void CheckCallerDeadline(ClientContext &context);

//! Detaches `names` (Runner::Attached) from a connection of its own; best effort.
void DetachDatabases(DatabaseInstance &db, const vector<string> &names);

//! Detaches what a one-off run (notebook_run, an export) attached, once its runner, whose connection may have USEd
//! one of them, is gone: declare it before the Runner. Never throws.
struct DetachOnExit {
	DatabaseInstance &db;
	vector<string> names;
	~DetachOnExit();
};

//! Runner::Prepare for a one-off run: what it attaches is left to `detach`, even when it throws.
void PrepareOnce(Runner &runner, DetachOnExit &detach, const notebook::Notebook &notebook, const string &source,
                 const std::map<string, Value> &params);

struct ExportOptions {
	//! html, html-wasm, md, sql, data or pdf.
	string format = "html";
	std::map<string, Value> params;
	//! `precomputed`: also run once per input value so the static page can switch between them.
	string interactive;
	//! Most notebook runs `interactive := 'precomputed'` may make.
	idx_t budget = 16;
	//! Result rows kept per query cell.
	idx_t page_rows = 100;
	//! html-wasm: directory holding the DuckDB-WASM runtime built by wasm/build.sh.
	string wasm_runtime;
	//! pdf: browser executable.
	string chrome;
	//! data: parquet or csv.
	string data_format = "parquet";
	//! The SQL call the export runs for: its interrupt and max_execution_time bound every run (Runner).
	optional_ptr<ClientContext> caller;
	//! What to hide in every cell, on top of each cell's own hide= option: code, output, all, or
	//! empty for nothing.
	string hide;
};

//! Runs a notebook and returns everything a reader needs to see it: cells, rendered markdown,
//! result pages, chart render descriptions and data, input values.
notebook::Json BuildPayload(DatabaseInstance &db, const string &text, const string &source,
                            const ExportOptions &options);

//! The export as a document (html, html-wasm, md, sql). `note` reports frozen inputs and the like.
string ExportDocument(DatabaseInstance &db, const string &text, const string &source, const ExportOptions &options,
                      string &note);

//! The values a select input offers: its options= list or the distinct values of its from= column.
vector<Value> InputChoices(Connection &connection, const notebook::Cell &cell, idx_t limit);

//! The words SQL highlighting colours, from this DuckDB: `{keywords, types}`. Keywords are the
//! reserved, column-name and type/function ones; unreserved keywords are also common column names.
notebook::Json SqlWords(Connection &connection);

//! A cell's parse and graph results as JSON, as the browser sees them.
notebook::Json CellJson(const notebook::Cell &cell);

//! An embedded frontend file (frontend/ in the repository), compiled into the extension.
const string &GetAsset(const string &name);

//! Runs one cell as the editor does and builds the result the editor is sent: status, error, row
//! count, timing, rendered markdown, result page, charts, and an input's value and choices.
CellOutcome RunForEditor(Runner &runner, const notebook::Notebook &notebook, const notebook::Cell &cell,
                         const RunnerOptions &options, notebook::Json &result);

//! The columns of `relation` (SQL text, as RelationSql gives) with their DuckDB types. On failure returns
//! false and sets `error`; `columns` is left empty.
bool DescribeColumns(Connection &connection, const string &relation, std::vector<notebook::ColumnInfo> &columns,
                     string &error);

//! The SQL type a variable set from the browser should have, so it compares like the column it
//! came from: from an input's widget or from= column, or a chart selection's fields. Empty when
//! unknown (the literal's own type is used).
string VariableSqlType(Connection &connection, const notebook::Notebook &notebook, const notebook::Cell &cell,
                       const string &name);

//! Markers around rendered {{ }} values when RunnerOptions::mark_values is set.
extern const char *const VALUE_OPEN;
extern const char *const VALUE_CLOSE;

//! The page size a cell asks for with rows=, or 0.
idx_t CellRows(const notebook::Cell &cell);

//! A Value as JSON: numbers stay numbers (unless too large for a double), temporal values are
//! ISO-like strings, lists and structs nest.
//! DECIMAL is an exact string unless `exact_decimal` is false (chart data); ±inf and NaN are numbers
//! that Json::Dump writes as "Infinity", "-Infinity" and "NaN".
notebook::Json ValueToJson(const Value &value, bool exact_decimal = true);

} // namespace duckdb
