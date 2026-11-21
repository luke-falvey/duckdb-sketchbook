#include "notebook/functions.hpp"

#include "duckdb/common/chrono.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "notebook/result_cache.hpp"
#include "notebook/script.hpp"
#include "notebook/session.hpp"

#include <map>

// notebook_run(path, params := {...}) runs a notebook headlessly, in a connection of its
// own. Cells run in file order, except that a markdown cell waits for the cells its
// `{{ }}` expressions read (notebook::RunOrder). Cells run through Runner (session.cpp), as in
// the server and the exports.
//
// A run never reads the result cache: it exists to compute from the data as it is now. With
// cache := true it writes it, so the next person to open the notebook sees these results at once.
//
// The whole notebook runs before the first row is returned, so a LIMIT or a filter above the call
// cannot decide how many cells run, and cache := true is always written. Databases the [notebook]
// cell attached are detached when the run is over.
//
// The Runner's connection is created here and destroyed when the run finishes. Never
// store a Connection (or anything else that owns the DatabaseInstance) in state that
// outlives a query: it would keep the database open after the user closes it, so the
// file stays locked and is not checkpointed.
namespace duckdb {

namespace {

struct RunBindData : public TableFunctionData {
	string path;
	notebook::Notebook notebook;
	vector<int> order;
	//! Values from params := {...}, by variable name.
	std::map<string, Value> params;
	//! cache := true: keep each cell's result as the editor would (FORMAT §8).
	bool cache = false;
};

struct RunState : public GlobalTableFunctionState {
	//! Per cell, in run order: the cell index and what running it gave.
	vector<std::pair<int, CellOutcome>> outcomes;
	//! Position in outcomes.
	idx_t next = 0;
};

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<Identifier> &names) {
	auto result = make_uniq<RunBindData>();
	result->path = SourceArgument(input, 0, "notebook_run");
	result->notebook = ReadNotebook(context, result->path);
	ThrowIfInvalid(result->notebook, result->path);
	result->order = notebook::RunOrder(result->notebook);

	auto params = input.named_parameters.find("params");
	if (params != input.named_parameters.end() && !params->second.IsNull()) {
		auto &value = params->second;
		if (value.type().id() != LogicalTypeId::STRUCT) {
			throw InvalidInputException("notebook_run: params must be a struct, such as {region: 'US'}, not %s",
			                            value.type().ToString());
		}
		auto &children = StructValue::GetChildren(value);
		for (idx_t i = 0; i < children.size(); i++) {
			auto name = StructType::GetChildName(value.type(), i).GetIdentifierName();
			auto problem = notebook::ParameterError(result->notebook, name);
			if (!problem.empty()) {
				throw InvalidInputException("notebook_run: %s", problem);
			}
			result->params[name] = children[i];
		}
	}

	result->cache = NamedBool(input, "cache");

	auto column = [&](const char *name, LogicalType type) {
		names.emplace_back(name);
		return_types.push_back(std::move(type));
	};
	column("idx", LogicalType::INTEGER);
	column("kind", LogicalType::VARCHAR);
	column("name", LogicalType::VARCHAR);
	// 'ok', 'error', or 'skipped' for cells with nothing to execute (metadata, markdown without {{ }}).
	column("status", LogicalType::VARCHAR);
	column("error", LogicalType::VARCHAR);
	// Rows a query cell returns, or rows of a chart's (first) data query. NULL for other cells.
	column("row_count", LogicalType::BIGINT);
	column("elapsed_ms", LogicalType::DOUBLE);
	// Markdown cells: the content with each {{ expr }} replaced by its value.
	column("output", LogicalType::VARCHAR);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	auto state = make_uniq<RunState>();
	auto &bind_data = input.bind_data->Cast<RunBindData>();
	auto &db = DatabaseInstance::GetDatabase(context);
	auto &cells = bind_data.notebook.cells;
	SessionState session;
	ResultCache cache;
	cache.database = CurrentDatabase(context);
	vector<RanCell> ran;
	DetachOnExit detach {db, {}};
	{
		// A separate connection gives the notebook its own temp views and variables, while sharing
		// the caller's attached databases. The caller's interrupt and max_execution_time bound the whole run: the
		// runner stops before the next cell, and no query of its connection outlives the call's deadline.
		Runner runner(db, session, &context);
		PrepareOnce(runner, detach, bind_data.notebook, bind_data.path, bind_data.params);
		RunFileVersions versions;
		if (bind_data.cache) {
			// Every cell runs, and is keyed with the files as it read them (BeforeCell): nothing to take beforehand.
			cache.Load(db, bind_data.path, bind_data.notebook);
		}
		for (auto index : bind_data.order) {
			auto &cell = cells[NumericCast<idx_t>(index)];
			RunnerOptions options;
			options.params = bind_data.params;
			// Chart cells compile and query their data, so a broken spec fails the run.
			options.charts = true;
			CellOutcome outcome;
			if (bind_data.cache) {
				// As the cell reads them: a file an earlier cell wrote counts as written.
				versions.BeforeCell(db, bind_data.notebook, cell);
				// The result the editor would be sent: a page of rows, chart data, marked {{ }} values.
				options.page_rows = 100;
				options.mark_values = true;
				auto result = notebook::Json::Object();
				outcome = RunForEditor(runner, bind_data.notebook, cell, options, result);
				outcome.output =
				    StringUtil::Replace(StringUtil::Replace(outcome.output, VALUE_OPEN, ""), VALUE_CLOSE, "");
				ran.push_back(RanFromResult(cell.index, outcome.status, result));
			} else {
				outcome = runner.Run(bind_data.notebook, cell, options);
			}
			state->outcomes.emplace_back(cell.index, std::move(outcome));
		}
		// The last cell may have run out of the call's time: the call fails then, as it does when an earlier one did.
		runner.CheckCaller();
		if (bind_data.cache) {
			runner.Save();
			cache.Record(db, bind_data.path, bind_data.notebook, session, ran, &versions);
		}
		// The connection is released here, then the databases, before the first row is returned.
	}
	return std::move(state);
}

void Scan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<RunBindData>();
	auto &cells = bind_data.notebook.cells;
	auto &state = data.global_state->Cast<RunState>();
	idx_t count = 0;
	while (state.next < state.outcomes.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = state.outcomes[state.next++];
		auto &cell = cells[NumericCast<idx_t>(entry.first)];
		auto &outcome = entry.second;
		output.data[0].Append(Value::INTEGER(cell.index));
		output.data[1].Append(Value(notebook::CellKindName(cell.kind)));
		output.data[2].Append(cell.name.empty() ? Value(LogicalType::VARCHAR) : Value(cell.name));
		output.data[3].Append(Value(outcome.status));
		output.data[4].Append(outcome.status == "error" ? Value(outcome.error) : Value(LogicalType::VARCHAR));
		output.data[5].Append(outcome.row_count >= 0 ? Value::BIGINT(outcome.row_count) : Value(LogicalType::BIGINT));
		output.data[6].Append(outcome.elapsed_ms >= 0 ? Value::DOUBLE(outcome.elapsed_ms) : Value(LogicalType::DOUBLE));
		output.data[7].Append(outcome.has_output ? Value(outcome.output) : Value(LogicalType::VARCHAR));
		count++;
	}
}

} // namespace

void RegisterNotebookRun(ExtensionLoader &loader) {
	TableFunction function("notebook_run", {LogicalType::VARCHAR}, Scan, Bind, Init);
	// params := {name: value, ...} overrides input defaults and sets selection variables.
	// cache := true keeps the results for the editor to show when the notebook is next opened.
	function.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("params", LogicalType::ANY).Add("cache", LogicalType::BOOLEAN);
	});
	loader.RegisterFunction(function);
}

} // namespace duckdb
