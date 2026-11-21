#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "miniz_wrapper.hpp"
#include "notebook/chart.hpp"
#include "notebook/document.hpp"
#include "notebook/functions.hpp"
#include "notebook/graph.hpp"
#include "notebook/script.hpp"
#include "notebook/session.hpp"
#include "notebook/sql_lexer.hpp"
#include "notebook/text.hpp"
#include "notebook/yaml.hpp"

#include <cstdlib>
#include <set>

// notebook_export (docs/DESIGN.md section 7). Every format starts from one run of the notebook in
// a session of its own; BuildPayload collects what that run shows, and each format presents it.
namespace duckdb {

using notebook::Json;

namespace {

string Gzip(const string &data) {
	MiniZStream stream;
	size_t size = MiniZStream::MaxCompressedLength(data.size());
	string out(size, '\0');
	stream.Compress(data.data(), data.size(), &out[0], &size);
	out.resize(size);
	return out;
}

//! Where scratch files go: TMPDIR (or TEMP/TMP on Windows), else the system default.
string TempDirectory() {
	for (auto name : {"TMPDIR", "TEMP", "TMP"}) {
		auto value = FileSystem::GetEnvVariable(name);
		if (!value.empty()) {
			return value;
		}
	}
#ifdef _WIN32
	return ".";
#else
	return "/tmp";
#endif
}

string Base64(const string &data) {
	return Blob::ToBase64(string_t(data.data(), NumericCast<uint32_t>(data.size())));
}

//! JavaScript or JSON that sits inside a <script> element.
string ScriptSafe(const string &code) {
	return notebook::ReplaceAll(code, "</script", "<\\/script");
}

string HtmlEscape(const string &text) {
	string out;
	for (char c : text) {
		switch (c) {
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '&':
			out += "&amp;";
			break;
		case '"':
			out += "&quot;";
			break;
		default:
			out += c;
		}
	}
	return out;
}

vector<Value> QueryValues(Connection &connection, const string &sql) {
	vector<Value> values;
	auto result = connection.Query(sql);
	if (result->HasError()) {
		return values;
	}
	while (auto chunk = result->Fetch()) {
		for (idx_t r = 0; r < chunk->size(); r++) {
			values.push_back(chunk->GetValue(0, r));
		}
	}
	return values;
}

Json OptionsJson(const notebook::Cell &cell) {
	auto options = Json::Object();
	for (auto &option : cell.options) {
		options.Set(option.first, Json::String(option.second));
	}
	return options;
}

Json StringArray(const std::vector<std::string> &values) {
	auto array = Json::Array();
	for (auto &value : values) {
		array.Push(Json::String(value));
	}
	return array;
}

} // namespace

//! Values an input offers: its options= list, or the distinct values of its from= column.
vector<Value> InputChoices(Connection &connection, const notebook::Cell &cell, idx_t limit) {
	vector<Value> values;
	auto options = cell.Option("options");
	if (!options.empty()) {
		size_t start = 0;
		while (start <= options.size()) {
			auto comma = options.find(',', start);
			if (comma == string::npos) {
				comma = options.size();
			}
			values.emplace_back(options.substr(start, comma - start));
			start = comma + 1;
		}
		return values;
	}
	string relation, name;
	if (!cell.InputFrom(relation, name)) {
		return values;
	}
	auto column = notebook::QuoteIdentifier(name);
	return QueryValues(connection, "SELECT DISTINCT " + column + " FROM " + notebook::RelationSql(relation) +
	                                   " WHERE " + column + " IS NOT NULL ORDER BY 1 LIMIT " + std::to_string(limit));
}

Json SqlWords(Connection &connection) {
	auto words = [&](const string &sql) {
		auto list = Json::Array();
		for (auto &value : QueryValues(connection, sql)) {
			list.Push(Json::String(value.ToString()));
		}
		return list;
	};
	return Json::Object()
	    .Set("keywords", words("SELECT keyword_name FROM duckdb_keywords() WHERE keyword_category <> 'unreserved' "
	                           "ORDER BY 1"))
	    .Set("types", words("SELECT DISTINCT lower(type_name) FROM duckdb_types() WHERE type_name NOT LIKE '%(%' "
	                        "AND type_name NOT LIKE '%\"%' ORDER BY 1"));
}

Json CellJson(const notebook::Cell &cell) {
	auto json = Json::Object();
	json.Set("idx", Json::Number(cell.index));
	json.Set("kind", Json::String(notebook::CellKindName(cell.kind)));
	json.Set("name", cell.name.empty() ? Json() : Json::String(cell.name));
	json.Set("head", Json::String(cell.head));
	json.Set("widget", cell.widget.empty() ? Json() : Json::String(cell.widget));
	json.Set("options", OptionsJson(cell));
	json.Set("body", Json::String(cell.body));
	json.Set("content", Json::String(cell.content));
	json.Set("sql", Json::String(cell.sql));
	json.Set("chart_specs", StringArray(cell.charts));
	json.Set("is_query", Json::Bool(cell.is_query));
	json.Set("variables", StringArray(cell.variables));
	json.Set("defines", StringArray(cell.defines));
	json.Set("reads", StringArray(cell.reads));
	auto depends = Json::Array();
	for (auto index : cell.depends_on) {
		depends.Push(Json::Number(index));
	}
	json.Set("depends_on", depends);
	json.Set("start_line", Json::Number(cell.start_line));
	json.Set("end_line", Json::Number(cell.end_line));
	json.Set("errors", StringArray(cell.errors));
	json.Set("warnings", StringArray(cell.warnings));
	return json;
}

Json BuildPayload(DatabaseInstance &db, const string &text, const string &source, const ExportOptions &options) {
	SessionState state;
	DetachOnExit detach {db, {}};
	Runner runner(db, state, options.caller);
	auto notebook = AnalyzeText(*runner.Conn().context, text);
	ThrowIfInvalid(notebook, source);
	PrepareOnce(runner, detach, notebook, source, options.params);
	RunnerOptions run;
	run.page_rows = options.page_rows;
	run.rows_at_least = true;
	run.mark_values = true;
	run.charts = true;
	run.params = options.params;

	vector<Json> cells(notebook.cells.size());
	for (auto index : notebook::RunOrder(notebook)) {
		auto &cell = notebook.cells[NumericCast<idx_t>(index)];
		Json data;
		auto outcome = runner.Run(notebook, cell, run, &data);
		auto json = CellJson(cell);
		json.Set("status", Json::String(outcome.status));
		json.Set("error", outcome.status == "error" ? Json::String(outcome.error) : Json());
		json.Set("row_count", outcome.row_count >= 0 ? Json::Number(static_cast<double>(outcome.row_count)) : Json());
		if (outcome.has_output) {
			json.Set("output", Json::String(outcome.output));
		}
		if (cell.kind == notebook::CellKind::CHART) {
			json.Set("chart", data);
		} else if (!data.IsNull()) {
			json.Set("table", data);
		}
		if (!outcome.charts.IsNull()) {
			json.Set("charts", outcome.charts);
		}
		cells[NumericCast<idx_t>(index)] = std::move(json);
	}
	for (auto &cell : notebook.cells) {
		if (cell.kind != notebook::CellKind::INPUT) {
			continue;
		}
		auto &json = cells[NumericCast<idx_t>(cell.index)];
		auto value = QueryValues(runner.Conn(), "SELECT getvariable(" + notebook::QuoteString(cell.name) + ")");
		json.Set("value", value.empty() ? Json() : ValueToJson(value[0]));
		if (cell.widget == "select" || cell.widget == "multiselect") {
			auto values = Json::Array();
			for (auto &option : InputChoices(runner.Conn(), cell, 1000)) {
				values.Push(ValueToJson(option));
			}
			json.Set("choices", values);
		}
	}
	auto array = Json::Array();
	for (auto &cell : cells) {
		array.Push(std::move(cell));
	}
	return Json::Object()
	    .Set("title", Json::String(notebook::NotebookTitle(notebook, source)))
	    .Set("source", Json::String(source))
	    .Set("sql_words", SqlWords(runner.Conn()))
	    .Set("cells", array)
	    .Set("hide", Json::String(options.hide));
}

namespace {

//! The html-wasm extras: the data the notebook reads as a .duckdb file in a storage version
//! DuckDB-WASM can read, and the SQL the page runs to rebuild the notebook's views.
Json WasmPayload(DatabaseInstance &db, const string &text, const string &source, const ExportOptions &options) {
	SessionState state;
	DetachOnExit detach {db, {}};
	Runner runner(db, state, options.caller);
	auto &connection = runner.Conn();
	auto notebook = AnalyzeText(*connection.context, text);
	PrepareOnce(runner, detach, notebook, source, options.params);
	RunnerOptions run;
	run.params = options.params;
	runner.RunAll(notebook, run);

	// Relations the cells read that no query cell defines: tables (or views) in the database.
	std::set<string> cell_views;
	for (auto &cell : notebook.cells) {
		if (cell.kind == notebook::CellKind::SQL && cell.is_query && !cell.name.empty()) {
			cell_views.insert(cell.name);
		}
	}
	std::set<string> relations;
	for (auto &cell : notebook.cells) {
		for (auto &name : cell.reads) {
			if (!name.empty() && name[0] != '$' && !cell_views.count(name)) {
				relations.insert(name);
			}
		}
	}
	auto id = StringUtil::Replace(UUID::ToString(UUID::GenerateRandomUUID()), "-", "");
	auto &fs = FileSystem::GetFileSystem(db);
	auto path = fs.JoinPath(TempDirectory(), "notebook_export_" + id + ".duckdb");
	// ATTACH is the whole database's: a name of this export's own, so that two exports never meet, and detached and
	// removed however the export ends.
	auto alias = "__notebook_export_" + id;
	auto target = notebook::QuoteIdentifier(alias);
	struct DropExport {
		DatabaseInstance &db;
		string alias;
		string path;
		bool attached = false;
		~DropExport() {
			try {
				if (attached) {
					DetachDatabases(db, {alias});
				}
				auto &fs = FileSystem::GetFileSystem(db);
				fs.TryRemoveFile(path);
				fs.TryRemoveFile(path + ".wal");
			} catch (...) { // NOLINT: a destructor must not throw
			}
		}
	} drop {db, alias, path};
	auto check = [&](const string &sql) {
		auto result = connection.Query(sql);
		if (result->HasError()) {
			throw InvalidInputException("html-wasm export: %s", result->GetError());
		}
	};
	check("ATTACH " + notebook::QuoteString(path) + " AS " + target + " (STORAGE_VERSION 'v1.5.0')");
	drop.attached = true;
	auto exported = Json::Array();
	for (auto &name : relations) {
		auto probe = connection.Query("SELECT * FROM " + notebook::RelationSql(name) + " LIMIT 0");
		if (probe->HasError()) {
			continue;
		}
		probe.reset();
		auto dot = name.find('.');
		if (dot != string::npos) {
			check("CREATE SCHEMA IF NOT EXISTS " + target + "." + notebook::QuoteIdentifier(name.substr(0, dot)));
		}
		check("CREATE TABLE " + target + "." + notebook::RelationSql(name) + " AS SELECT * FROM " +
		      notebook::RelationSql(name));
		exported.Push(Json::String(name));
	}
	check("DETACH " + target);
	drop.attached = false;
	auto bytes = ReadNotebookText(db, path);

	auto setup = Json::Array();
	for (auto &name : notebook::UndefinedVariables(notebook)) {
		// Engines before DuckDB 2.0 cannot read `.lo` from an untyped NULL, so selections start as
		// NULLs of their eventual type.
		string type;
		for (auto &cell : notebook.cells) {
			for (auto &spec : cell.charts) {
				if (notebook::ChartSelectionInfo(spec).name == name) {
					type = VariableSqlType(connection, notebook, cell, name);
				}
			}
		}
		setup.Push(Json::String("SET VARIABLE " + notebook::QuoteIdentifier(name) + " = NULL" +
		                        (type.empty() ? "" : "::" + type)));
	}
	for (auto index : notebook::RunOrder(notebook)) {
		auto &cell = notebook.cells[NumericCast<idx_t>(index)];
		// Inputs set their variables and query cells become temporary views again; effect cells already ran
		// into the data file.
		if (cell.kind == notebook::CellKind::INPUT || (cell.kind == notebook::CellKind::SQL && cell.is_query)) {
			for (auto &statement : notebook::SplitStatements(notebook::CellSql(cell))) {
				setup.Push(Json::String(notebook::RewriteVariables(statement.text)));
			}
		}
	}
	// The SQL type of every variable the page can set, as the editor gives it (VariableSqlType), so a value
	// set in the page is the same value.
	auto types = Json::Object();
	for (auto &cell : notebook.cells) {
		if (cell.kind == notebook::CellKind::INPUT && !cell.name.empty()) {
			types.Set(cell.name, Json::String(VariableSqlType(connection, notebook, cell, cell.name)));
		}
		for (auto &spec : cell.charts) {
			auto name = notebook::ChartSelectionInfo(spec).name;
			if (!name.empty()) {
				types.Set(name, Json::String(VariableSqlType(connection, notebook, cell, name)));
			}
		}
	}
	return Json::Object()
	    .Set("db", Json::String(Base64(Gzip(bytes))))
	    .Set("tables", exported)
	    .Set("setup", setup)
	    .Set("types", types);
}

//! The SQL a viewer running DuckDB-WASM needs per cell to refresh it.
void AddWasmQueries(Json &cell_json, const notebook::Cell &cell) {
	// Chart queries of an unnamed query cell contain its SQL, which may read $variables.
	for (auto key : {"chart", "charts"}) {
		auto charts = cell_json.Get(key);
		if (!charts || charts->IsNull()) {
			continue;
		}
		auto list = *charts;
		if (list.GetType() != Json::Type::ARRAY) {
			list = Json::Array();
			list.Push(*charts);
		}
		auto rewritten = Json::Array();
		for (auto chart : list.Items()) {
			if (auto queries = chart.Get("queries")) {
				auto plain = Json::Array();
				for (auto &query : queries->Items()) {
					plain.Push(Json::String(notebook::RewriteVariables(query.Str())));
				}
				chart.Set("queries", plain);
			}
			rewritten.Push(std::move(chart));
		}
		bool many = charts->GetType() == Json::Type::ARRAY;
		cell_json.Set(key, many ? rewritten : rewritten.Items()[0]);
	}
	if (cell.kind == notebook::CellKind::SQL && cell.is_query) {
		auto sql = notebook::DisplaySql(cell);
		cell_json.Set("display_sql", Json::String(notebook::RewriteVariables(sql)));
	} else if (cell.kind == notebook::CellKind::MARKDOWN) {
		auto expressions = Json::Array();
		for (auto &expression : cell.expressions) {
			expressions.Push(Json::Object()
			                     .Set("sql", Json::String(notebook::RewriteVariables(expression.sql)))
			                     .Set("begin", Json::Number(static_cast<double>(expression.begin)))
			                     .Set("end", Json::Number(static_cast<double>(expression.end))));
		}
		cell_json.Set("expressions", expressions);
	}
}

string ReadRuntimeFile(DatabaseInstance &db, const string &directory, const string &name) {
	auto path = directory + "/" + name;
	auto &fs = FileSystem::GetFileSystem(db);
	if (!fs.FileExists(path)) {
		throw InvalidInputException("html-wasm export: \"%s\" not found. Build the runtime with wasm/build.sh and "
		                            "pass its directory as wasm_runtime := '...' (or set NOTEBOOK_WASM_RUNTIME)",
		                            path);
	}
	return ReadNotebookText(db, path);
}

string Html(DatabaseInstance &db, const Json &payload, const string &title, const string &wasm_runtime) {
	auto html = GetAsset("viewer.html");
	auto data = Base64(Gzip(payload.Dump()));
	string wasm;
	if (!wasm_runtime.empty()) {
		wasm = "<script type=\"application/octet-stream\" id=\"nb-wasm-module\">" +
		       Base64(Gzip(ReadRuntimeFile(db, wasm_runtime, "duckdb-eh.wasm"))) + "</script>\n" +
		       "<script type=\"application/octet-stream\" id=\"nb-wasm-worker\">" +
		       Base64(ReadRuntimeFile(db, wasm_runtime, "duckdb-browser-eh.worker.js")) + "</script>\n" + "<script>" +
		       ScriptSafe(ReadRuntimeFile(db, wasm_runtime, "runtime.js")) + "</script>\n";
	}
	html = notebook::ReplaceAll(html, "{{TITLE}}", HtmlEscape(title));
	// The stylesheet loads fonts from the server; an export carries them inline.
	auto style = GetAsset("style.css");
	for (auto font : {"Excalifont.woff2", "CascadiaCode.woff2"}) {
		style = notebook::ReplaceAll(style, string("url('/assets/") + font + "')",
		                             "url(data:font/woff2;base64," + Base64(GetAsset(font)) + ")");
	}
	html = notebook::ReplaceAll(html, "{{STYLE}}", style);
	html = notebook::ReplaceAll(html, "{{DATA}}", data);
	html = notebook::ReplaceAll(html, "{{WASM}}", wasm);
	html = notebook::ReplaceAll(html, "{{RENDER}}",
	                            ScriptSafe(GetAsset("render-sketch.js") + GetAsset("ink-strokes.js") +
	                                       GetAsset("render-ink.js") + GetAsset("render-core.js") +
	                                       GetAsset("render-sql.js") + GetAsset("render-chart.js") +
	                                       GetAsset("render-typed.js") + GetAsset("render-present.js")));
	html = notebook::ReplaceAll(html, "{{VIEWER}}", ScriptSafe(GetAsset("viewer.js")));
	return html;
}

struct InputSpace {
	const notebook::Cell *cell;
	vector<Value> values;
};

} // namespace

string ExportDocument(DatabaseInstance &db, const string &text, const string &source, const ExportOptions &options,
                      string &note) {
	auto &format = options.format;
	if (format == "sql") {
		auto notebook = AnalyzeText(db, text);
		ThrowIfInvalid(notebook, source);
		return notebook::ExpandNotebook(notebook, source);
	}
	auto payload = BuildPayload(db, text, source, options);
	auto title = payload.Get("title")->Str();
	if (format == "md") {
		auto markdown = notebook::MarkdownDocument(payload);
		markdown = notebook::ReplaceAll(notebook::ReplaceAll(markdown, VALUE_OPEN, ""), VALUE_CLOSE, "");
		return "<!-- Exported from " + source + " by notebook_export. -->\n\n" + markdown;
	}
	if (format != "html" && format != "html-wasm") {
		throw InvalidInputException("Unknown export format '%s'; expected html, html-wasm, md, sql, data or pdf",
		                            format);
	}
	if (format == "html-wasm") {
		auto runtime = options.wasm_runtime;
		if (runtime.empty() && std::getenv("NOTEBOOK_WASM_RUNTIME")) {
			runtime = std::getenv("NOTEBOOK_WASM_RUNTIME");
		}
		if (runtime.empty()) {
			throw InvalidInputException("html-wasm export needs the DuckDB-WASM runtime: build it with wasm/build.sh "
			                            "and pass wasm_runtime := '<directory>' (or set NOTEBOOK_WASM_RUNTIME)");
		}
		auto notebook = AnalyzeText(db, text);
		auto cells = Json::Array();
		for (auto &cell_json : payload.Get("cells")->Items()) {
			auto copy = cell_json;
			AddWasmQueries(copy, notebook.cells[static_cast<idx_t>(cell_json.Get("idx")->Num())]);
			cells.Push(std::move(copy));
		}
		payload.Set("cells", cells);
		payload.Set("wasm", WasmPayload(db, text, source, options));
		note = "DuckDB-WASM runs the notebook's queries in the page";
		return Html(db, payload, title, runtime);
	}
	if (options.interactive == "precomputed") {
		// Run once per combination of input values, within the budget; the page swaps between them.
		SessionState state;
		DetachOnExit detach {db, {}};
		Runner runner(db, state, options.caller);
		auto notebook = AnalyzeText(*runner.Conn().context, text);
		// Choices may come from cells (select from=cell.column), which must run first.
		PrepareOnce(runner, detach, notebook, source, options.params);
		RunnerOptions run_all;
		run_all.params = options.params;
		runner.RunAll(notebook, run_all);
		vector<InputSpace> spaces;
		vector<string> frozen;
		idx_t combinations = 1;
		for (auto &cell : notebook.cells) {
			if (cell.kind != notebook::CellKind::INPUT || options.params.count(cell.name)) {
				continue;
			}
			vector<Value> values;
			if (cell.widget == "select") {
				values = InputChoices(runner.Conn(), cell, 50);
			} else if (cell.widget == "checkbox") {
				values = {Value::BOOLEAN(false), Value::BOOLEAN(true)};
			} else if ((cell.widget == "slider" || cell.widget == "number") && !cell.Option("step").empty()) {
				double lo = std::atof(cell.Option("min", "0").c_str()), hi = std::atof(cell.Option("max", "0").c_str());
				double step = std::atof(cell.Option("step").c_str());
				for (auto v : notebook::InputSteps(lo, hi, step, 51)) {
					values.push_back(Value::DOUBLE(v));
				}
			}
			// combinations * values.size() > budget, without overflowing on a huge budget.
			if (values.empty() || values.size() > options.budget / combinations) {
				frozen.push_back(cell.name);
				continue;
			}
			combinations *= values.size();
			spaces.push_back({&cell, std::move(values)});
		}
		auto inputs = Json::Array();
		for (auto &space : spaces) {
			auto values = Json::Array();
			for (auto &value : space.values) {
				values.Push(ValueToJson(value));
			}
			inputs.Push(Json::Object().Set("name", Json::String(space.cell->name)).Set("values", values));
		}
		auto states = Json::Object();
		vector<idx_t> position(spaces.size(), 0);
		for (idx_t n = 0; n < combinations && !spaces.empty(); n++) {
			ExportOptions run = options;
			string key;
			for (idx_t i = 0; i < spaces.size(); i++) {
				run.params[spaces[i].cell->name] = spaces[i].values[position[i]];
				key += (i ? "," : "") + std::to_string(position[i]);
			}
			auto state_payload = BuildPayload(db, text, source, run);
			states.Set(key, *state_payload.Get("cells"));
			for (idx_t i = spaces.size(); i-- > 0;) {
				if (++position[i] < spaces[i].values.size()) {
					break;
				}
				position[i] = 0;
			}
		}
		payload.Set("precomputed", Json::Object().Set("inputs", inputs).Set("states", states));
		note = std::to_string(combinations) + " precomputed state(s)";
		if (!frozen.empty()) {
			note += "; frozen at their current value:";
			for (auto &name : frozen) {
				note += " " + name;
			}
		}
		auto frozen_json = Json::Array();
		for (auto &name : frozen) {
			frozen_json.Push(Json::String(name));
		}
		payload.Set("frozen", frozen_json);
	}
	return Html(db, payload, title, string());
}

namespace {

struct ExportBindData : public TableFunctionData {
	string source, to;
	ExportOptions options;
};

//! True if `path` is a file, asked through the database's file system so its access rules apply.
bool IsFile(FileSystem &fs, const string &path) {
	try {
		return fs.FileExists(path);
	} catch (std::exception &) {
		return false;
	}
}

//! A Chrome, Chromium or Edge for printing PDFs: chrome :=, NOTEBOOK_CHROME, PATH, then the usual install places.
string FindBrowser(FileSystem &fs, const string &requested) {
	if (!requested.empty()) {
		return requested;
	}
	auto env = FileSystem::GetEnvVariable("NOTEBOOK_CHROME");
	if (!env.empty()) {
		return env;
	}
#ifdef _WIN32
	const char separator = ';';
	const char *names[] = {"chrome.exe", "msedge.exe", "chromium.exe", "chrome-headless-shell.exe"};
#else
	const char separator = ':';
	const char *names[] = {"chromium",       "chromium-browser", "google-chrome",        "google-chrome-stable",
	                       "microsoft-edge", "chrome",           "chrome-headless-shell"};
#endif
	auto dirs = FileSystem::GetEnvVariable("PATH");
	size_t start = 0;
	while (!dirs.empty() && start <= dirs.size()) {
		auto end = dirs.find(separator, start);
		auto dir = dirs.substr(start, end == string::npos ? string::npos : end - start);
		for (auto name : names) {
			auto candidate = dir + "/" + name;
			if (!dir.empty() && IsFile(fs, candidate)) {
				return candidate;
			}
		}
		if (end == string::npos) {
			break;
		}
		start = end + 1;
	}
	vector<string> installed;
#if defined(_WIN32)
	for (auto root : {"PROGRAMFILES", "PROGRAMFILES(X86)", "LOCALAPPDATA"}) {
		auto dir = FileSystem::GetEnvVariable(root);
		if (!dir.empty()) {
			installed.push_back(dir + "\\Google\\Chrome\\Application\\chrome.exe");
			installed.push_back(dir + "\\Microsoft\\Edge\\Application\\msedge.exe");
			installed.push_back(dir + "\\Chromium\\Application\\chrome.exe");
		}
	}
#elif defined(__APPLE__)
	for (auto app : {"Google Chrome", "Chromium", "Microsoft Edge"}) {
		installed.push_back(string("/Applications/") + app + ".app/Contents/MacOS/" + app);
	}
#endif
	for (auto &candidate : installed) {
		if (IsFile(fs, candidate)) {
			return candidate;
		}
	}
	return string();
}

//! Quotes one argument for the shell std::system runs: sh, or cmd.exe on Windows.
string ShellQuote(const string &text) {
	if (text.find('\n') != string::npos || text.find('\r') != string::npos || text.find('\0') != string::npos) {
		throw InvalidInputException("notebook_export: a path for the PDF printer cannot contain a line break");
	}
#ifdef _WIN32
	// Inside double quotes cmd.exe still ends the quote at '"' and expands %NAME%, and neither can be escaped
	// there. Windows file names cannot contain '"'; one with '%' (or '#' or '?', which the file URL encodes
	// with '%') cannot be printed this way.
	if (text.find('"') != string::npos || text.find('%') != string::npos) {
		throw InvalidInputException("notebook_export: a path for the PDF printer cannot contain '\"' or '%%' on "
		                            "Windows: %s",
		                            text);
	}
	// A backslash before the closing quote would escape it for the program reading its arguments.
	auto quoted = text;
	auto slashes = quoted.size() - MinValue<idx_t>(quoted.size(), quoted.find_last_not_of('\\') + 1);
	quoted.append(slashes, '\\');
	return "\"" + quoted + "\"";
#else
	return "'" + notebook::ReplaceAll(text, "'", "'\\''") + "'";
#endif
}

//! A file:// URL for a local path, relative paths resolved against the working directory.
string FileUrl(FileSystem &fs, const string &path) {
	auto absolute = fs.IsPathAbsolute(path) ? path : fs.JoinPath(FileSystem::GetWorkingDirectory(), path);
	absolute = notebook::ReplaceAll(absolute, "\\", "/");
	// What a URL would read as an escape, a query or a fragment is part of the file name.
	absolute = notebook::ReplaceAll(absolute, "%", "%25");
	absolute = notebook::ReplaceAll(absolute, "#", "%23");
	absolute = notebook::ReplaceAll(absolute, "?", "%3F");
	// Windows drive paths (C:/x) need a third slash: file:///C:/x.
	return string(absolute.compare(0, 1, "/") == 0 ? "file://" : "file:///") + absolute;
}

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<Identifier> &names) {
	auto result = make_uniq<ExportBindData>();
	result->source = SourceArgument(input, 0, "notebook_export");
	for (auto &entry : input.named_parameters) {
		auto key = entry.first.GetIdentifierName();
		auto &value = entry.second;
		if (value.IsNull()) {
			continue;
		}
		if (key == "file") {
			result->to = value.GetValue<string>();
		} else if (key == "format") {
			result->options.format = value.GetValue<string>();
		} else if (key == "interactive") {
			result->options.interactive = value.GetValue<string>();
		} else if (key == "budget") {
			result->options.budget = value.GetValue<idx_t>();
		} else if (key == "page_rows") {
			result->options.page_rows = value.GetValue<idx_t>();
		} else if (key == "wasm_runtime") {
			result->options.wasm_runtime = value.GetValue<string>();
		} else if (key == "chrome") {
			result->options.chrome = value.GetValue<string>();
		} else if (key == "hide") {
			result->options.hide = value.GetValue<string>();
		} else if (key == "data_format") {
			result->options.data_format = value.GetValue<string>();
		} else if (key == "params") {
			if (value.type().id() != LogicalTypeId::STRUCT) {
				throw InvalidInputException("notebook_export: params must be a struct, such as {region: 'US'}");
			}
			auto &children = StructValue::GetChildren(value);
			for (idx_t i = 0; i < children.size(); i++) {
				result->options.params[StructType::GetChildName(value.type(), i).GetIdentifierName()] = children[i];
			}
		}
	}
	auto &options = result->options;
	auto problem = notebook::ExportOptionError(options.format, options.interactive, options.data_format, options.hide);
	if (!problem.empty()) {
		throw InvalidInputException("notebook_export: %s", problem);
	}
	// A PDF is printed by running a browser (chrome :=, NOTEBOOK_CHROME or one found on the PATH): a program
	// outside DuckDB, which a database that may not reach outside itself must not start.
	if (options.format == "pdf" && !Settings::Get<EnableExternalAccessSetting>(context)) {
		throw PermissionException("notebook_export: format 'pdf' runs a browser to print the page, which is not "
		                          "allowed with external access disabled (enable_external_access is false)");
	}
	// The program is whatever chrome := names: with the configuration locked, SQL may not choose one to run, as it
	// may not start the server (notebook_start).
	if (options.format == "pdf" && Settings::Get<LockConfigurationSetting>(context)) {
		throw PermissionException("notebook_export: format 'pdf' runs a browser to print the page, which is not "
		                          "allowed while the configuration is locked (lock_configuration is set)");
	}
	if (options.hide == "none") {
		options.hide.clear();
	}
	// Parameters must name inputs or variables nothing sets, as notebook_run checks them.
	if (!options.params.empty()) {
		auto notebook = ReadNotebook(context, result->source);
		for (auto &param : options.params) {
			auto error = notebook::ParameterError(notebook, param.first);
			if (!error.empty()) {
				throw InvalidInputException("notebook_export: %s", error);
			}
		}
	}
	auto &format = options.format;
	if (result->to.empty()) {
		auto stem = notebook::IsStoredSource(result->source) ? result->source.substr(3) : result->source;
		auto dot = stem.rfind('.');
		auto slash = stem.find_last_of("/\\");
		if (dot != string::npos && (slash == string::npos || dot > slash)) {
			stem = stem.substr(0, dot);
		}
		auto extension = format == "md" ? ".md" : format == "sql" ? ".expanded.sql" : format == "pdf" ? ".pdf" : "";
		result->to = format == "data" ? stem + "_data" : stem + (extension[0] ? extension : ".html");
	}
	names.emplace_back("format");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("path");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("bytes");
	return_types.push_back(LogicalType::BIGINT);
	names.emplace_back("note");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

void Scan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<OnceState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &bind = data.bind_data->Cast<ExportBindData>();
	auto &db = DatabaseInstance::GetDatabase(context);
	// Every run of the export stops when this call is interrupted or runs out of max_execution_time.
	auto options = bind.options;
	options.caller = &context;
	auto text = ReadNotebookText(context, bind.source);
	auto &fs = FileSystem::GetFileSystem(context);
	string note;
	int64_t bytes = 0;
	auto format = options.format;

	if (format == "data") {
		// One file per named query cell, written from the run's own session.
		SessionState session;
		DetachOnExit detach {db, {}};
		Runner runner(db, session, &context);
		auto notebook = AnalyzeText(*runner.Conn().context, text);
		ThrowIfInvalid(notebook, bind.source);
		PrepareOnce(runner, detach, notebook, bind.source, options.params);
		RunnerOptions run;
		run.params = options.params;
		runner.RunAll(notebook, run);
		runner.CheckCaller();
		// Only once the run is done: a run past the call's deadline leaves no directory. The files are written one by
		// one, so a deadline that passes while they are written leaves those written so far.
		if (!fs.DirectoryExists(bind.to)) {
			fs.CreateDirectory(bind.to);
		}
		idx_t files = 0;
		auto extension = options.data_format == "csv" ? ".csv" : ".parquet";
		for (auto &cell : notebook.cells) {
			if (cell.kind != notebook::CellKind::SQL || !cell.is_query || cell.name.empty()) {
				continue;
			}
			auto path = fs.JoinPath(bind.to, cell.name + extension);
			auto result = runner.Conn().Query("COPY (SELECT * FROM " + notebook::QuoteIdentifier(cell.name) + ") TO " +
			                                  notebook::QuoteString(path) + " (FORMAT " +
			                                  (options.data_format == "csv" ? "csv" : "parquet") + ")");
			if (result->HasError()) {
				auto error = result->GetError();
				result.reset();
				// Out of the call's time: the same INTERRUPT error as every other export and notebook_run.
				runner.CheckCaller();
				throw InvalidInputException("notebook_export: cell '%s': %s", cell.name, error);
			}
			files++;
		}
		note = std::to_string(files) + " file(s)";
	} else if (format == "pdf") {
#ifdef NOTEBOOK_NO_HOST_FEATURES
		throw NotImplementedException("notebook_export: format 'pdf' runs a local Chrome, which this build of the "
		                              "sketchbook extension cannot do (it was built without host features, as for "
		                              "DuckDB-WASM). Export html and print it from the browser instead");
#endif
		auto browser = FindBrowser(fs, options.chrome);
		if (browser.empty()) {
			throw InvalidInputException("notebook_export: PDF needs Chrome, Chromium or Edge. Pass chrome := "
			                            "'/path/to/browser' or set NOTEBOOK_CHROME, or export html and print it");
		}
		ExportOptions html_options = options;
		html_options.format = "html";
		auto html = ExportDocument(db, text, bind.source, html_options, note);
		// A run that took past the call's deadline fails, whichever cell ran out of time, rather than prints its
		// errors.
		CheckCallerDeadline(context);
		// The page is printed as it opens: no handwriting, whatever headless Chrome's clock does, and
		// the normal width whatever the notebook or the reader chose (paper has its own width).
		html = notebook::ReplaceAll(html, "<html lang=\"en\">",
		                            "<html lang=\"en\" data-ink=\"off\" data-width-fixed=\"normal\">");
		// A name of its own, so that no file of the reader's (report.html beside report.pdf) is overwritten.
		// Beside the file a link points to, so that the printed PDF moves over it without crossing file systems.
		auto target = fs.CanonicalizePath(bind.to);
		auto html_path = target + "." + UUID::ToString(UUID::GenerateRandomUUID()) + ".html";
		WriteTextFile(context, html_path, html);
		// Printed to a file of its own, moved over the target once printed: an older PDF neither passes for this one
		// if the browser writes nothing, nor is lost if it fails.
		auto pdf_path = target + "." + UUID::ToString(UUID::GenerateRandomUUID()) + ".pdf";
		auto command = ShellQuote(browser) +
		               " --headless --disable-gpu --no-sandbox --no-pdf-header-footer --virtual-time-budget=10000 "
		               "--print-to-pdf=" +
		               ShellQuote(pdf_path) + " " + ShellQuote(FileUrl(fs, html_path));
#ifdef _WIN32
		// cmd.exe strips the first and last quote of a command line that starts with one.
		command = "\"" + command + " >NUL 2>&1\"";
#else
		command += " >/dev/null 2>&1";
#endif
		auto status = std::system(command.c_str());
		fs.TryRemoveFile(html_path);
		if (status != 0 || !fs.FileExists(pdf_path)) {
			fs.TryRemoveFile(pdf_path);
			throw InvalidInputException("notebook_export: %s failed to print the PDF (exit status %d)", browser,
			                            status);
		}
		try {
			// Over the file a link points to, as WriteFile does: the link stays a link.
			fs.MoveFile(pdf_path, target);
		} catch (...) {
			try {
				fs.TryRemoveFile(pdf_path);
			} catch (...) { // NOLINT: the move's error is the one to report
			}
			throw;
		}
		note = "printed with " + browser;
	} else {
		auto document = ExportDocument(db, text, bind.source, options, note);
		// A run that took past the call's deadline fails, whichever cell ran out of time, rather than writes its
		// errors.
		CheckCallerDeadline(context);
		WriteTextFile(context, bind.to, document);
		bytes = static_cast<int64_t>(document.size());
	}
	if (bytes == 0 && fs.FileExists(bind.to)) {
		auto handle = fs.OpenFile(bind.to, FileFlags::FILE_FLAGS_READ);
		bytes = handle->GetFileSize();
	}
	output.data[0].Append(Value(format));
	output.data[1].Append(Value(bind.to));
	output.data[2].Append(Value::BIGINT(bytes));
	output.data[3].Append(note.empty() ? Value(LogicalType::VARCHAR) : Value(note));
}

} // namespace

void RegisterNotebookExport(ExtensionLoader &loader) {
	TableFunction function("notebook_export", {LogicalType::VARCHAR}, Scan, Bind, OnceInit);
	function.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("file", LogicalType::VARCHAR)
		    .Add("format", LogicalType::VARCHAR)
		    .Add("params", LogicalType::ANY)
		    .Add("interactive", LogicalType::VARCHAR)
		    .Add("budget", LogicalType::UBIGINT)
		    .Add("page_rows", LogicalType::UBIGINT)
		    .Add("wasm_runtime", LogicalType::VARCHAR)
		    .Add("chrome", LogicalType::VARCHAR)
		    .Add("data_format", LogicalType::VARCHAR)
		    .Add("hide", LogicalType::VARCHAR);
	});
	loader.RegisterFunction(function);
}

} // namespace duckdb
