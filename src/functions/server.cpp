#include "httplib.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "notebook/chart.hpp"
#include "notebook/document.hpp"
#include "notebook/functions.hpp"
#include "notebook/graph.hpp"
#include "notebook/cache.hpp"
#include "notebook/result_cache.hpp"
#include "notebook/script.hpp"
#include "notebook/session.hpp"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <random>
#include <thread>

// The notebook server (docs/DESIGN.md section 4): an HTTP server on 127.0.0.1 inside the user's
// DuckDB process, serving the embedded frontend and a JSON API.
//
// Lifetime: the server never owns the database. It holds a weak_ptr to the DatabaseInstance and
// opens a connection per request (a Runner replays the notebook session's variables and views),
// so closing the database is never blocked by the server. The server object lives in the
// database's object cache, which is destroyed with the database, and stops the server then.
//
// Security: requests must come to 127.0.0.1 or localhost on the server's port (Host header), from
// the server's own origin if an Origin header is sent, and carry the random token created at start
// (X-Notebook-Token header or token= parameter). This blocks other sites and DNS rebinding.
namespace duckdb {

using notebook::Json;
namespace http = duckdb_httplib;

namespace {

thread_local bool in_server_thread = false;

struct Session {
	string source;
	//! The database a stored notebook (`db:`) is read from and saved to, and whose `_sketchbook.cache` holds its
	//! cache: the current database (the one USE chose) of the notebook_start call before the session was opened.
	string database;
	string text;
	SessionState state;
	//! Serialises work on the session: one request runs its cells at a time.
	std::mutex lock;
	std::mutex running_lock;
	shared_ptr<ClientContext> running;
	//! The last result of each cell (FORMAT §8): shown when the notebook is opened, kept as cells run.
	ResultCache cache;
	//! True once the session has run a cell. Until then the page shows the last results only.
	bool ran = false;
	//! The variables whose values came from the store, not from the reader: reading the store again
	//! refreshes these and keeps the rest.
	std::set<string> restored;
	//! Trust (DESIGN section 3): true once the reader has run something. Until then, and again
	//! after the text is replaced by the file on disk, nothing runs by itself: no peeks.
	std::atomic<bool> trusted {false};
	//! Counts peeks as they arrive; one that is no longer the latest gives up. The peek that is
	//! running (guarded by running_lock) is interrupted by a newer one and by a run.
	std::atomic<uint64_t> peeks {0};
	shared_ptr<ClientContext> peeking;

	void InterruptPeek() {
		std::lock_guard<std::mutex> guard(running_lock);
		if (peeking) {
			peeking->Interrupt();
		}
	}
};

struct ServerState {
	weak_ptr<DatabaseInstance> db;
	string token;
	string host = "127.0.0.1";
	int port = 0;
	http::Server http;
	std::atomic<bool> stopping {false};
	std::mutex sessions_lock;
	std::map<string, shared_ptr<Session>> sessions;
	//! The current database of the latest notebook_start: where a request without `db` keeps stored notebooks
	//! (RequestDatabase). Guarded by sessions_lock.
	string database;
};

string RandomToken() {
	std::random_device random;
	string token;
	const char *digits = "0123456789abcdef";
	for (int i = 0; i < 32; i++) {
		token += digits[random() % 16];
	}
	return token;
}

shared_ptr<DatabaseInstance> Database(ServerState &state) {
	auto db = state.db.lock();
	if (!db) {
		throw InvalidInputException("The database this notebook server belonged to has been closed");
	}
	return db;
}

void SendJson(http::Response &res, const Json &json, int status = 200) {
	res.status = status;
	res.set_header("Cache-Control", "no-store");
	res.set_content(json.Dump(), "application/json");
}

void SendError(http::Response &res, const string &message, int status = 400) {
	SendJson(res, Json::Object().Set("error", Json::String(message)), status);
}

Json Body(const http::Request &req) {
	string error;
	auto body = notebook::ParseJson(req.body.empty() ? "{}" : req.body, error);
	if (!error.empty()) {
		throw InvalidInputException("Request body is not JSON: %s", error);
	}
	return body;
}

string Param(const http::Request &req, const string &key) {
	return req.has_param(key) ? req.get_param_value(key) : string();
}

// Numbers in requests are checked before they are cast: a negative, fractional, huge or non-numeric
// value is refused (400) or clamped, never cast as it is (which is undefined for NaN or out of range).

//! A number in a request body: `fallback` when absent or null, refused when it is not a number.
double BodyNumber(const Json *value, const string &name, double fallback) {
	if (!value || value->IsNull()) {
		return fallback;
	}
	if (value->GetType() != Json::Type::NUMBER || !std::isfinite(value->Num())) {
		throw InvalidInputException("%s must be a number", name);
	}
	return value->Num();
}

//! A count in a request body, clamped to [low, high].
idx_t BodyCount(const Json *value, const string &name, idx_t fallback, idx_t low, idx_t high) {
	auto number = BodyNumber(value, name, static_cast<double>(fallback));
	number = MaxValue<double>(static_cast<double>(low), MinValue<double>(static_cast<double>(high), number));
	return static_cast<idx_t>(number);
}

//! A cell index in a request body: a whole number that is not negative.
int BodyIndex(const Json &value, const string &name) {
	if (value.GetType() != Json::Type::NUMBER || !(value.Num() >= 0 && value.Num() <= INT_MAX) ||
	    value.Num() != std::floor(value.Num())) {
		throw InvalidInputException("%s must be cell indexes (whole numbers from 0)", name);
	}
	return static_cast<int>(value.Num());
}

//! A whole number in a query string parameter; false when it is not one or is out of range.
bool ParseWhole(const string &text, int64_t &out) {
	if (text.empty()) {
		return false;
	}
	errno = 0;
	char *end = nullptr;
	auto value = std::strtoll(text.c_str(), &end, 10);
	if (errno != 0 || end != text.c_str() + text.size()) {
		return false;
	}
	out = static_cast<int64_t>(value);
	return true;
}

//! A count in the query string: `fallback` when absent, refused when not a whole number, then clamped.
idx_t ParamCount(const http::Request &req, const string &key, idx_t fallback, idx_t low, idx_t high) {
	auto text = Param(req, key);
	int64_t value = static_cast<int64_t>(fallback);
	if (!text.empty() && !ParseWhole(text, value)) {
		throw InvalidInputException("%s must be a whole number", key);
	}
	if (value < static_cast<int64_t>(low)) {
		return low;
	}
	return MinValue<idx_t>(high, static_cast<idx_t>(value));
}

Json CellsJson(const notebook::Notebook &notebook) {
	auto cells = Json::Array();
	for (auto &cell : notebook.cells) {
		cells.Push(CellJson(cell));
	}
	return cells;
}

//! The notebook as the editor shows it. With `results`, also the cached result of every cell that has
//! one and the input values that go with them: sent when a notebook is opened, not on every edit.
Json NotebookJson(DatabaseInstance &db, Session &session, bool results = false) {
	auto notebook = AnalyzeText(db, session.text);
	auto json = Json::Object()
	                .Set("source", Json::String(session.source))
	                .Set("text", Json::String(session.text))
	                .Set("preamble", Json::String(notebook.preamble))
	                .Set("title", Json::String(notebook::NotebookTitle(notebook, session.source)))
	                .Set("trusted", Json::Bool(session.trusted))
	                .Set("cells", CellsJson(notebook))
	                .Set("sql_words", [&]() {
		                Connection connection(db);
		                return SqlWords(connection);
	                }());
	if (!notebook::CacheEnabled(notebook)) {
		// Switched off in the [notebook] cell: whatever was kept goes at once.
		session.cache.Record(db, session.source, notebook, session.state, {});
	} else if (results) {
		json.Set("cached", session.cache.CachedJson(db, notebook, session.state))
		    .Set("variables", session.cache.ShownJson(notebook))
		    .Set("ran", Json::Bool(session.ran));
	}
	return json;
}

//! The notebook a request is about: `nb` in its JSON body, or else in its query string.
string Source(const http::Request &req, const Json &body) {
	return body.Get("nb") ? body.Get("nb")->Str() : Param(req, "nb");
}

//! The last results and input values kept for the session's notebook, to show without running anything.
void LoadKept(DatabaseInstance &db, Session &session) {
	auto notebook = notebook::ParseNotebook(session.text);
	// What the reader set in this session (an input moved before any run) outlives the store's copy.
	auto shown = session.cache.ShownJson(notebook);
	for (auto &name : session.restored) {
		session.state.variables.erase(name);
	}
	session.cache = ResultCache();
	session.cache.database = session.database;
	session.cache.Load(db, session.source, notebook);
	session.restored = session.cache.RestoreVariables(db, notebook, session.state);
	for (auto &entry : shown.Entries()) {
		if (session.state.variables.count(entry.first) && !session.restored.count(entry.first)) {
			session.cache.SetShown(entry.first, entry.second);
		}
	}
}

//! The database a request's stored notebooks belong to: the `db` the page was opened with (notebook_start puts the
//! current database in its URL), else that of the latest notebook_start.
string RequestDatabase(ServerState &state, const http::Request &req, const Json &body) {
	if (auto db = body.Get("db")) {
		if (db->GetType() == Json::Type::STRING) {
			return db->Str();
		}
	}
	if (req.has_param("db")) {
		return Param(req, "db");
	}
	std::lock_guard<std::mutex> guard(state.sessions_lock);
	return state.database;
}

//! What identifies a session: a stored notebook with the database it is in, a file by its canonical path, so that
//! `a.sql`, `./a.sql` and a link to it are one session.
string SessionKey(DatabaseInstance &db, const string &source, const string &database) {
	if (notebook::IsStoredSource(source)) {
		// Database names are matched without case, as DuckDB does: `USE MYDB` and `USE mydb` are one database.
		return "db\n" + StringUtil::Lower(database) + "\n" + source;
	}
	try {
		return "file\n" + FileSystem::GetFileSystem(db).CanonicalizePath(source);
	} catch (...) { // NOLINT: a path that cannot be resolved is its own key
		return "file\n" + source;
	}
}

shared_ptr<Session> GetSession(ServerState &state, const string &source, const string &database) {
	if (source.empty()) {
		throw InvalidInputException("No notebook given: add nb=<path> or nb=db:<name> to the URL");
	}
	auto db = Database(state);
	auto key = SessionKey(*db, source, database);
	{
		std::lock_guard<std::mutex> guard(state.sessions_lock);
		auto found = state.sessions.find(key);
		if (found != state.sessions.end()) {
			return found->second;
		}
	}
	// Read outside the server's lock: a slow read (a remote notebook, a large cache) must not hold up the requests of
	// every other notebook.
	auto session = make_shared_ptr<Session>();
	session->source = source;
	session->database = database;
	// The session outlives its requests: materialize=table results and temp tables are kept, not recomputed.
	session->state.keep_tables = true;
	bool exists = notebook::IsStoredSource(source) || FileSystem::GetFileSystem(*db).FileExists(source);
	if (exists) {
		session->text = ReadNotebookText(*db, session->database, source);
		LoadKept(*db, *session);
	} else {
		// A new notebook: it becomes a file when first saved.
		session->text = "-- %% [markdown]\n/*\n# Untitled\n*/\n\n-- %%\nSELECT 42 AS answer;\n";
	}
	std::lock_guard<std::mutex> guard(state.sessions_lock);
	// Another request for the same notebook may have got there first: it wins, and this copy is dropped.
	auto inserted = state.sessions.emplace(key, session);
	return inserted.first->second;
}

shared_ptr<Session> GetSession(ServerState &state, const http::Request &req, const Json &body) {
	return GetSession(state, Source(req, body), RequestDatabase(state, req, body));
}

shared_ptr<Session> GetSession(ServerState &state, const http::Request &req) {
	return GetSession(state, req, Json());
}

const notebook::Cell &CellAt(const notebook::Notebook &notebook, const string &index) {
	int64_t i = -1;
	if (!ParseWhole(index, i) || i < 0 || i >= static_cast<int64_t>(notebook.cells.size())) {
		throw InvalidInputException("No cell %s", index);
	}
	return notebook.cells[NumericCast<idx_t>(i)];
}

//! Said instead of running anything until the reader has run something (DESIGN section 3).
const char *const NOT_TRUSTED = "nothing has been run yet";

//! Remembers which context is running so /api/interrupt can stop it.
struct RunningGuard {
	Session &session;
	RunningGuard(Session &session, Runner &runner) : session(session) {
		std::lock_guard<std::mutex> guard(session.running_lock);
		session.running = runner.Context();
	}
	~RunningGuard() {
		std::lock_guard<std::mutex> guard(session.running_lock);
		session.running.reset();
	}
};

string ContentType(const string &name) {
	if (StringUtil::EndsWith(name, ".js")) {
		return "text/javascript; charset=utf-8";
	}
	if (StringUtil::EndsWith(name, ".css")) {
		return "text/css; charset=utf-8";
	}
	if (StringUtil::EndsWith(name, ".html")) {
		return "text/html; charset=utf-8";
	}
	if (StringUtil::EndsWith(name, ".woff2")) {
		return "font/woff2";
	}
	return "application/octet-stream";
}

//! Wraps a handler: resolves the server state, turns exceptions into JSON errors.
template <class F>
http::Server::Handler Handler(weak_ptr<ServerState> weak, F f) {
	return [weak, f](const http::Request &req, http::Response &res) {
		auto state = weak.lock();
		if (!state) {
			SendError(res, "server stopped", 503);
			return;
		}
		try {
			f(*state, req, res);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			SendError(res, error.RawMessage());
		}
	};
}

void Routes(shared_ptr<ServerState> &shared) {
	weak_ptr<ServerState> weak = shared;
	auto &server = shared->http;

	server.set_pre_routing_handler([weak](const http::Request &req, http::Response &res) {
		in_server_thread = true;
		auto state = weak.lock();
		if (!state || state->stopping) {
			res.status = 503;
			return http::Server::HandlerResponse::Handled;
		}
		auto port = std::to_string(state->port);
		auto host = req.get_header_value("Host");
		if (host != "127.0.0.1:" + port && host != "localhost:" + port) {
			res.status = 403;
			res.set_content("Forbidden host", "text/plain");
			return http::Server::HandlerResponse::Handled;
		}
		auto origin = req.get_header_value("Origin");
		if (!origin.empty() && origin != "http://127.0.0.1:" + port && origin != "http://localhost:" + port) {
			res.status = 403;
			res.set_content("Forbidden origin", "text/plain");
			return http::Server::HandlerResponse::Handled;
		}
		if (StringUtil::StartsWith(req.path, "/assets/")) {
			return http::Server::HandlerResponse::Unhandled;
		}
		auto token = req.get_header_value("X-Notebook-Token");
		if (token.empty()) {
			token = Param(req, "token");
		}
		// Compare in constant time.
		unsigned diff = token.size() != state->token.size();
		for (idx_t i = 0; i < token.size() && i < state->token.size(); i++) {
			diff |= static_cast<unsigned>(token[i] ^ state->token[i]);
		}
		if (diff) {
			res.status = 403;
			res.set_content("Missing or wrong token. Open the URL that notebook_start() returned.", "text/plain");
			return http::Server::HandlerResponse::Handled;
		}
		return http::Server::HandlerResponse::Unhandled;
	});

	server.Get("/", [](const http::Request &, http::Response &res) {
		res.set_header("Cache-Control", "no-store");
		res.set_content(GetAsset("index.html"), "text/html; charset=utf-8");
	});

	server.Get(R"(/assets/([\w.-]+))", [](const http::Request &req, http::Response &res) {
		auto name = req.path.substr(8);
		auto &asset = GetAsset(name);
		if (asset.empty() || name == "viewer.html") {
			res.status = 404;
			return;
		}
		res.set_content(asset, ContentType(name));
	});

	server.Get("/api/notebook", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		           auto session = GetSession(state, req);
		           std::lock_guard<std::mutex> guard(session->lock);
		           auto db = Database(state);
		           if (!session->ran) {
			           // Until this session runs something, the store is the truth: a scheduled notebook_run(cache :=
			           // true) may have written results since, or notebook_cache_clear removed them.
			           LoadKept(*db, *session);
		           }
		           SendJson(res, NotebookJson(*db, *session, true));
	           }));

	// Update the session's text (and optionally save it). Returns the analysis of the new text.
	server.Post("/api/notebook", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		            auto body = Body(req);
		            auto session = GetSession(state, req, body);
		            std::lock_guard<std::mutex> guard(session->lock);
		            auto db = Database(state);
		            if (auto text = body.Get("text")) {
			            session->text = text->Str();
		            }
		            if (body.Get("reload") && body.Get("reload")->Truthy()) {
			            // The text now comes from the file, not from the reader: it is inert again.
			            session->trusted = false;
		            }
		            string note;
		            if (body.Get("save") && body.Get("save")->Truthy()) {
			            if (notebook::IsStoredSource(session->source)) {
				            auto stored = notebook::ParseStoredSource(session->source);
				            if (stored.version != 0) {
					            // Saving would quietly make an old version the newest, over the edits made since.
					            throw InvalidInputException(
					                "%s is version %d of '%s', not the newest, and cannot be saved: open db:%s to save "
					                "changes, or write this version to a file with notebook_checkout('%s', version := %d, "
					                "file := ...)",
					                session->source, stored.version, stored.name, stored.name, stored.name,
					                stored.version);
				            }
				            Connection connection(*db);
				            auto name = stored.name;
				            bool saved;
				            auto version =
				                SaveNotebookVersion(connection, session->database, name, session->text,
				                                    "saved from the notebook UI", true, saved, note, session->source);
				            note = saved ? "Saved as version " + std::to_string(version) : "No changes to save";
			            } else {
				            WriteTextFile(*db, session->source, session->text);
				            note = "Saved " + session->source;
			            }
		            }
		            auto json = NotebookJson(*db, *session, body.Get("reload") && body.Get("reload")->Truthy());
		            json.Set("note", Json::String(note));
		            SendJson(res, json);
	            }));

	server.Post("/api/run", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		            auto body = Body(req);
		            auto session = GetSession(state, req, body);
		            // A run does not wait for a peek.
		            session->peeks++;
		            session->InterruptPeek();
		            std::lock_guard<std::mutex> guard(session->lock);
		            session->trusted = true;
		            if (auto text = body.Get("text")) {
			            if (text->GetType() == Json::Type::STRING) {
				            session->text = text->Str();
			            }
		            }
		            auto db = Database(state);
		            Runner runner(*db, session->state);
		            RunningGuard running(*session, runner);
		            auto notebook = AnalyzeText(*runner.Conn().context, session->text);
		            runner.Prepare(notebook, session->source, {});
		            // Before anything runs, and before each cell: a file that changes after a cell read it leaves it
		            // stale, one an earlier cell wrote does not.
		            RunFileVersions versions;
		            versions.Start(*db, notebook);
		            RunnerOptions options;
		            options.charts = true;
		            options.mark_values = true;
		            options.page_rows = BodyCount(body.Get("page_rows"), "page_rows", 100, 1, 10000);
		            std::set<int> wanted, redraw;
		            vector<RanCell> ran;
		            auto cells = body.Get("cells");
		            bool all = !cells || cells->IsNull();
		            if (!all) {
			            for (auto &item : cells->Items()) {
				            wanted.insert(BodyIndex(item, "cells"));
			            }
		            }
		            // Cells whose charts are drawn again without running their SQL: a chart spec changed, or a
		            // selection their charts filter on.
		            if (auto list = body.Get("redraw")) {
			            for (auto &item : list->Items()) {
				            redraw.insert(BodyIndex(item, "redraw"));
			            }
		            }
		            auto results = Json::Array();
		            for (auto index : notebook::RunOrder(notebook)) {
			            bool redrawing = redraw.count(index) && !all && !wanted.count(index);
			            if (!all && !wanted.count(index) && !redrawing) {
				            continue;
			            }
			            auto &cell = notebook.cells[NumericCast<idx_t>(index)];
			            auto result = Json::Object().Set("idx", Json::Number(index));
			            if (!cell.errors.empty()) {
				            result.Set("status", Json::String("error")).Set("error", Json::String(cell.errors[0]));
				            results.Push(result);
				            continue;
			            }
			            if (redrawing && cell.kind == notebook::CellKind::SQL) {
				            string error;
				            auto charts = runner.Charts(notebook, cell, error);
				            result.Set("redraw", Json::Bool(true)).Set("charts", charts);
				            result.Set("status", Json::String(error.empty() ? "ok" : "error"));
				            result.Set("error", error.empty() ? Json() : Json::String(error));
				            RanCell drawn;
				            drawn.index = index;
				            drawn.redraw = Json::Object()
				                               .Set("charts", charts)
				                               .Set("status", *result.Get("status"))
				                               .Set("error", *result.Get("error"));
				            ran.push_back(std::move(drawn));
				            results.Push(result);
				            continue;
			            }
			            versions.BeforeCell(*db, notebook, cell);
			            auto status = RunForEditor(runner, notebook, cell, options, result).status;
			            // Serialised once, for the response and for the cache.
			            auto done = RanFromResult(index, status, result);
			            results.Push(Json::Raw(done.result));
			            ran.push_back(std::move(done));
		            }
		            session->ran = true;
		            runner.Save();
		            session->cache.Record(*db, session->source, notebook, session->state, ran, &versions);
		            SendJson(res, Json::Object().Set("results", results).Set("cells", CellsJson(notebook)));
	            }));

	server.Post("/api/variable", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		            auto body = Body(req);
		            auto session = GetSession(state, req, body);
		            std::lock_guard<std::mutex> guard(session->lock);
		            auto name = body.Get("name") ? body.Get("name")->Str() : string();
		            if (name.empty()) {
			            throw InvalidInputException("variable needs a name");
		            }
		            auto db = Database(state);
		            Runner runner(*db, session->state);
		            auto notebook = AnalyzeText(*runner.Conn().context, session->text);
		            // Until the reader runs something, the [notebook] cell's LOAD and ATTACH wait, and so does
		            // any query that would give the value its type (an input's from=, a chart's relation).
		            bool trusted = session->trusted;
		            if (trusted) {
			            try { // setting an input never fails because of the [notebook] cell: a run reports that
				            runner.ApplyMetadata(notebook, session->source);
			            } catch (std::exception &) { // NOLINT
			            }
		            }
		            string type;
		            if (auto cell = body.Get("cell")) {
			            if (!cell->IsNull()) {
				            auto index = std::to_string(BodyIndex(*cell, "cell"));
				            auto &target = CellAt(notebook, index);
				            string relation, column;
				            bool without_sql =
				                target.kind == notebook::CellKind::INPUT && !target.InputFrom(relation, column);
				            if (trusted || without_sql) {
					            type = VariableSqlType(runner.Conn(), notebook, target, name);
				            }
			            }
		            }
		            auto value = body.Get("value") ? *body.Get("value") : Json();
		            auto sql = "SET VARIABLE " + notebook::QuoteIdentifier(name) + " = " +
		                       notebook::JsonToSqlLiteral(value, type);
		            auto result = runner.Conn().Query(sql);
		            if (result->HasError()) {
			            throw InvalidInputException("%s", result->GetError());
		            }
		            result.reset();
		            // The value goes with the cached results, so a reopened notebook shows the same input.
		            runner.Save();
		            session->cache.SetShown(name, value);
		            session->restored.erase(name);
		            session->cache.Record(*db, session->source, notebook, session->state, {});
		            SendJson(res, Json::Object().Set("ok", Json::Bool(true)).Set("sql", Json::String(sql)));
	            }));

	// Peek under the pen: the first rows of the CTE or sub-query under the caret (DESIGN section 19).
	// `sql` is the text being edited, which may be newer than the session's; `cursor` is a byte offset
	// into it. Answers `{reason}` instead of an error when there is nothing to show.
	server.Post("/api/peek", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		            auto body = Body(req);
		            auto session = GetSession(state, req, body);
		            auto nothing = [&](const string &reason) {
			            SendJson(res, Json::Object().Set("reason", Json::String(reason)));
		            };
		            if (!session->trusted) {
			            nothing(NOT_TRUSTED);
			            return;
		            }
		            auto sql = body.Get("sql") ? body.Get("sql")->Str() : string();
		            auto cursor = BodyNumber(body.Get("cursor"), "cursor", -1);
		            if (sql.empty() || cursor < 0 || cursor > static_cast<double>(sql.size())) {
			            nothing("no SQL or cursor position");
			            return;
		            }
		            auto rows = BodyCount(body.Get("rows"), "rows", 5, 1, 20);
		            // The newest peek wins: it stops the one that is running and takes its place.
		            auto ticket = ++session->peeks;
		            session->InterruptPeek();
		            std::lock_guard<std::mutex> guard(session->lock);
		            if (session->peeks != ticket || !session->trusted) {
			            nothing("superseded");
			            return;
		            }
		            auto db = Database(state);
		            Json result;
		            {
			            // A copy of the session's state: a peek leaves the session exactly as it was.
			            SessionState scratch = session->state;
			            Runner runner(*db, scratch);
			            {
				            std::lock_guard<std::mutex> running(session->running_lock);
				            session->peeking = runner.Context();
			            }
			            try {
				            result = runner.Peek(sql, static_cast<idx_t>(cursor), rows, 1500);
			            } catch (std::exception &ex) {
				            result = Json::Object().Set("reason", Json::String(ErrorData(ex).RawMessage()));
			            }
			            std::lock_guard<std::mutex> running(session->running_lock);
			            session->peeking.reset();
		            }
		            SendJson(res, result);
	            }));

	server.Get("/api/page", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		           auto session = GetSession(state, req);
		           std::lock_guard<std::mutex> guard(session->lock);
		           // A page runs the cell's SQL: only once the reader has run something (DESIGN section 3).
		           if (!session->trusted) {
			           throw InvalidInputException("Paging runs the cell's query, and %s in this session: run the "
			                                       "notebook first",
			                                       NOT_TRUSTED);
		           }
		           auto limit = ParamCount(req, "limit", 100, 1, 10000);
		           auto offset = ParamCount(req, "offset", 0, 0, static_cast<idx_t>(NumericLimits<int64_t>::Maximum()));
		           auto db = Database(state);
		           Runner runner(*db, session->state);
		           auto notebook = AnalyzeText(*runner.Conn().context, session->text);
		           runner.ApplyMetadata(notebook, session->source);
		           auto sql = notebook::DisplaySql(CellAt(notebook, Param(req, "cell")));
		           if (sql.empty()) {
			           throw InvalidInputException("That cell has no result to page through");
		           }
		           string error;
		           auto page = runner.Page("SELECT * FROM (" + sql + ") LIMIT " + std::to_string(limit) + " OFFSET " +
		                                       std::to_string(offset),
		                                   limit, error);
		           if (!error.empty()) {
			           throw InvalidInputException("%s", error);
		           }
		           SendJson(res, page);
	           }));

	server.Get("/api/options", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		           auto session = GetSession(state, req);
		           std::lock_guard<std::mutex> guard(session->lock);
		           auto db = Database(state);
		           auto values = Json::Array();
		           if (!session->trusted) {
			           // An options= list needs no SQL. Choices from= a query wait for the reader's first run, which
			           // sends them; until then the [notebook] cell's LOAD and ATTACH do not run either.
			           auto notebook = AnalyzeText(*db, session->text);
			           auto index = Param(req, "cell");
			           auto &cell = CellAt(notebook, index);
			           auto json = Json::Object();
			           if (!cell.Option("options").empty()) {
				           Connection connection(*db);
				           for (auto &value : InputChoices(connection, cell, 1000)) {
					           values.Push(ValueToJson(value));
				           }
			           } else {
				           json.Set("reason", Json::String(NOT_TRUSTED));
			           }
			           SendJson(res, json.Set("values", values));
			           return;
		           }
		           Runner runner(*db, session->state);
		           auto notebook = AnalyzeText(*runner.Conn().context, session->text);
		           runner.ApplyMetadata(notebook, session->source);
		           for (auto &value : InputChoices(runner.Conn(), CellAt(notebook, Param(req, "cell")), 1000)) {
			           values.Push(ValueToJson(value));
		           }
		           SendJson(res, Json::Object().Set("values", values));
	           }));

	server.Get(
	    "/api/catalog", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		    auto session = GetSession(state, req);
		    std::lock_guard<std::mutex> guard(session->lock);
		    auto db = Database(state);
		    // Until the reader runs something the catalog is the database's own: the session's views are not
		    // replayed and the [notebook] cell's LOAD and ATTACH wait for the first run (DESIGN section 3).
		    bool trusted = session->trusted;
		    unique_ptr<Runner> runner;
		    unique_ptr<Connection> plain;
		    if (trusted) {
			    runner = make_uniq<Runner>(*db, session->state);
			    try { // the catalog lists what is there: a metadata failure is reported by a run, not here
				    runner->ApplyMetadata(AnalyzeText(*runner->Conn().context, session->text), session->source);
			    } catch (std::exception &) { // NOLINT
			    }
		    } else {
			    plain = make_uniq<Connection>(*db);
		    }
		    auto &connection = trusted ? runner->Conn() : *plain;
		    auto tables = Json::Array();
		    auto result = connection.Query(
		        "SELECT t.database_name, t.schema_name, t.table_name, t.kind, list({'name': c.column_name, 'type': "
		        "c.data_type} ORDER BY c.column_index) FROM (SELECT database_name, schema_name, table_name, 'table' AS "
		        "kind "
		        "FROM duckdb_tables() WHERE NOT internal UNION ALL SELECT database_name, schema_name, view_name, "
		        "'view' FROM "
		        "duckdb_views() WHERE NOT internal) t LEFT JOIN duckdb_columns() c USING (database_name, schema_name, "
		        "table_name) WHERE t.schema_name NOT IN ('_sketchbook', '_notebook') GROUP BY ALL ORDER BY ALL");
		    if (result->HasError()) {
			    throw InvalidInputException("%s", result->GetError());
		    }
		    auto current = connection.Query("SELECT current_database()");
		    string current_db = current->HasError() ? "" : current->Fetch()->GetValue(0, 0).ToString();
		    while (auto chunk = result->Fetch()) {
			    for (idx_t r = 0; r < chunk->size(); r++) {
				    auto database = chunk->GetValue(0, r).ToString(), schema = chunk->GetValue(1, r).ToString();
				    auto name = chunk->GetValue(2, r).ToString();
				    auto qualified = notebook::QuoteIdentifier(name);
				    if (!(database == current_db || database == "temp") || schema != "main") {
					    qualified = notebook::QuoteIdentifier(database) + "." + notebook::QuoteIdentifier(schema) +
					                "." + qualified;
				    }
				    tables.Push(Json::Object()
				                    .Set("database", Json::String(database))
				                    .Set("schema", Json::String(schema))
				                    .Set("name", Json::String(name))
				                    .Set("kind", Json::String(chunk->GetValue(3, r).ToString()))
				                    .Set("qualified", Json::String(qualified))
				                    .Set("columns", ValueToJson(chunk->GetValue(4, r))));
			    }
		    }
		    SendJson(res, Json::Object().Set("tables", tables).Set("trusted", Json::Bool(trusted)));
	    }));

	server.Get("/api/notebooks", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		           auto db = Database(state);
		           auto files = Json::Array(), stored = Json::Array(), open = Json::Array();
		           for (auto &file : FileSystem::GetFileSystem(*db).Glob("*.sql")) {
			           files.Push(Json::String(file.path));
		           }
		           // The stored notebooks of the database this page belongs to.
		           auto database = RequestDatabase(state, req, Json());
		           Connection connection(*db);
		           auto result =
		               connection.Query("SELECT DISTINCT name FROM " +
		                                (database.empty() ? string() : notebook::QuoteIdentifier(database) + ".") +
		                                StorageSchema(connection, database) + ".versions ORDER BY name");
		           if (!result->HasError()) {
			           while (auto chunk = result->Fetch()) {
				           for (idx_t r = 0; r < chunk->size(); r++) {
					           stored.Push(Json::String(chunk->GetValue(0, r).ToString()));
				           }
			           }
		           }
		           {
			           std::lock_guard<std::mutex> guard(state.sessions_lock);
			           for (auto &session : state.sessions) {
				           open.Push(Json::String(session.second->source));
			           }
		           }
		           SendJson(res, Json::Object().Set("files", files).Set("stored", stored).Set("open", open));
	           }));

	// Forgets the notebook's cached results. What the page shows from this session is untouched.
	server.Post("/api/cache/clear",
	            Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		            auto body = Body(req);
		            auto session = GetSession(state, req, body);
		            std::lock_guard<std::mutex> guard(session->lock);
		            auto db = Database(state);
		            auto removed = session->cache.Clear(*db, session->source);
		            SendJson(res, Json::Object().Set("ok", Json::Bool(true)).Set("removed", Json::Number(removed)));
	            }));

	server.Post("/api/interrupt", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		            auto body = Body(req);
		            auto session = GetSession(state, req, body);
		            std::lock_guard<std::mutex> guard(session->running_lock);
		            if (session->running) {
			            session->running->Interrupt();
		            }
		            SendJson(res, Json::Object().Set("ok", Json::Bool(true)));
	            }));

	server.Get(
	    "/api/export", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		    auto session = GetSession(state, req);
		    string text;
		    {
			    std::lock_guard<std::mutex> guard(session->lock);
			    text = session->text;
		    }
		    auto db = Database(state);
		    ExportOptions options;
		    options.format = Param(req, "format").empty() ? "html" : Param(req, "format");
		    options.interactive = Param(req, "interactive");
		    auto problem =
		        notebook::ExportOptionError(options.format, Param(req, "interactive"), string(), Param(req, "hide"));
		    if (!problem.empty()) {
			    throw InvalidInputException("%s", problem);
		    }
		    options.hide = Param(req, "hide") == "none" ? string() : Param(req, "hide");
		    string note;
		    auto document = ExportDocument(*db, text, session->source, options, note);
		    auto stem = session->source.substr(session->source.find_last_of("/:") + 1);
		    stem = stem.substr(0, stem.rfind('.'));
		    auto extension = options.format == "md" ? ".md" : options.format == "sql" ? ".expanded.sql" : ".html";
		    auto type = options.format == "md"    ? "text/markdown; charset=utf-8"
		                : options.format == "sql" ? "application/sql"
		                                          : "text/html; charset=utf-8";
		    res.set_header("Content-Disposition", "inline; filename=\"" + stem + extension + "\"");
		    res.set_content(document, type);
	    }));

	// Server-sent events: reports edits to the file made by another program. Each stream lasts
	// about 30 seconds; EventSource reconnects by itself.
	server.Get("/api/events", Handler(weak, [weak](ServerState &state, const http::Request &req, http::Response &res) {
		           auto session = GetSession(state, req);
		           auto source = session->source;
		           res.set_header("Cache-Control", "no-store");
		           auto started = std::chrono::steady_clock::now();
		           auto last = std::make_shared<string>();
		           res.set_chunked_content_provider("text/event-stream", [weak, session, source, started,
		                                                                  last](size_t offset, http::DataSink &sink) {
			           auto state = weak.lock();
			           if (!state || state->stopping ||
			               std::chrono::steady_clock::now() - started > std::chrono::seconds(30)) {
				           sink.done();
				           return true;
			           }
			           if (offset == 0) {
				           sink.write("retry: 1000\n\n", 13);
			           }
			           std::this_thread::sleep_for(std::chrono::milliseconds(500));
			           // A stored notebook changes only through this server; a remote one would be fetched whole
			           // twice a second for as long as the page is open.
			           if (notebook::IsStoredSource(source) || !notebook::IsLocalPath(source)) {
				           sink.write(": ping\n\n", 8);
				           return true;
			           }
			           auto db = state->db.lock();
			           if (!db) {
				           sink.done();
				           return true;
			           }
			           string text;
			           try {
				           if (!FileSystem::GetFileSystem(*db).FileExists(source)) {
					           sink.write(": ping\n\n", 8);
					           return true;
				           }
				           text = ReadNotebookText(*db, source);
			           } catch (...) {
				           sink.write(": ping\n\n", 8);
				           return true;
			           }
			           if (last->empty()) {
				           *last = text;
			           }
			           if (text != *last) {
				           *last = text;
				           auto event =
				               "event: changed\ndata: " + Json::Object().Set("text", Json::String(text)).Dump() +
				               "\n\n";
				           sink.write(event.data(), event.size());
			           } else {
				           sink.write(": ping\n\n", 8);
			           }
			           return true;
		           });
	           }));
}

class NotebookServer : public ObjectCacheEntry {
public:
	explicit NotebookServer(shared_ptr<DatabaseInstance> db) : db(db) {
	}
	~NotebookServer() override {
		Stop();
	}

	static string ObjectType() {
		return "notebook_server";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		// Not evictable: the server lives until it is stopped or the database closes.
		return optional_idx();
	}

	bool Running() const {
		return state && !state->stopping;
	}

	//! Starts the server unless it runs. `database`: the caller's current database, where a request whose page has no
	//! `db` (Url) reads and saves stored notebooks; set before the server answers anything.
	void Start(int port, const string &database) {
		std::lock_guard<std::mutex> guard(lock);
		if (Running()) {
			std::lock_guard<std::mutex> sessions_guard(state->sessions_lock);
			state->database = database;
			return;
		}
		auto fresh = make_shared_ptr<ServerState>();
		fresh->db = db;
		fresh->token = RandomToken();
		fresh->database = database;
		fresh->http.set_keep_alive_max_count(100);
		Routes(fresh);
		if (port > 0) {
			if (!fresh->http.bind_to_port(fresh->host, port)) {
				throw IOException("notebook_start: cannot listen on 127.0.0.1:%d (in use?)", port);
			}
			fresh->port = port;
		} else {
			fresh->port = fresh->http.bind_to_any_port(fresh->host);
			if (fresh->port <= 0) {
				throw IOException("notebook_start: cannot listen on 127.0.0.1");
			}
		}
		state = fresh;
		thread = std::thread([fresh]() { fresh->http.listen_after_bind(); });
		state->http.wait_until_ready();
	}

	void Stop() {
		std::lock_guard<std::mutex> guard(lock);
		if (!state) {
			return;
		}
		state->stopping = true;
		state->http.stop();
		if (thread.joinable()) {
			// Stopped from one of the server's own threads (the database closed while a request
			// held the last reference to it): joining would wait on ourselves.
			if (in_server_thread) {
				thread.detach();
			} else {
				thread.join();
			}
		}
		{
			std::lock_guard<std::mutex> sessions(state->sessions_lock);
			state->sessions.clear();
		}
		state.reset();
	}

	//! The page for `source`. A stored notebook's page names its database, so it keeps reading and saving there
	//! whatever a later notebook_start's USE was.
	string Url(const string &source, const string &database) {
		string url = "http://127.0.0.1:" + std::to_string(state->port) + "/?token=" + state->token;
		if (!source.empty()) {
			url += "&nb=" + StringUtil::URLEncode(source);
		}
		if (!database.empty()) {
			url += "&db=" + StringUtil::URLEncode(database);
		}
		return url;
	}

	int Port() const {
		return state ? state->port : 0;
	}
	string Token() const {
		return state ? state->token : string();
	}

private:
	weak_ptr<DatabaseInstance> db;
	shared_ptr<ServerState> state;
	std::thread thread;
	std::mutex lock;
};

const char *const CACHE_KEY = "duckdb_sketchbook_server";

struct StartBindData : public TableFunctionData {
	string source;
	int port = 0;
	bool open = true;
};

unique_ptr<FunctionData> StartBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	// The server listens on a port, runs any SQL its page sends and opens a browser: none of that is for a
	// database whose configuration is locked or that may not reach outside itself.
	if (Settings::Get<LockConfigurationSetting>(context)) {
		throw PermissionException("notebook_start: the notebook server cannot be started while the configuration "
		                          "is locked (lock_configuration is set)");
	}
	if (!Settings::Get<EnableExternalAccessSetting>(context)) {
		throw PermissionException("notebook_start: the notebook server cannot be started with external access "
		                          "disabled (enable_external_access is false)");
	}
	auto result = make_uniq<StartBindData>();
	if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
		// Checked as every other function checks it: an empty path or a bad db: version is an error now, not in the
		// browser later.
		result->source = SourceArgument(input, 0, "notebook_start");
	}
	for (auto &entry : input.named_parameters) {
		auto key = entry.first.GetIdentifierName();
		if (entry.second.IsNull()) {
			continue;
		}
		if (key == "port") {
			result->port = entry.second.GetValue<int32_t>();
			if (result->port < 0 || result->port > 65535) {
				throw InvalidInputException("notebook_start: port must be between 0 (any free port) and 65535");
			}
		} else if (key == "open") {
			result->open = entry.second.GetValue<bool>();
		}
	}
	names.emplace_back("url");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("port");
	return_types.push_back(LogicalType::INTEGER);
	names.emplace_back("token");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

void OpenBrowser(const string &url) {
#if defined(__APPLE__)
	auto command = "open '" + url + "' >/dev/null 2>&1 &";
#elif defined(_WIN32)
	auto command = "start \"\" \"" + url + "\"";
#else
	if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
		return;
	}
	auto command = "xdg-open '" + url + "' >/dev/null 2>&1 &";
#endif
	auto status = std::system(command.c_str());
	(void)status;
}

void StartScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<OnceState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &bind = data.bind_data->Cast<StartBindData>();
	auto server = ObjectCache::GetObjectCache(context).GetOrCreate<NotebookServer>(CACHE_KEY, context.db);
	auto database = CurrentDatabase(context);
	server->Start(bind.port, database);
	auto url = server->Url(bind.source, database);
	if (bind.open) {
		OpenBrowser(url);
	}
	output.data[0].Append(Value(url));
	output.data[1].Append(Value::INTEGER(server->Port()));
	output.data[2].Append(Value(server->Token()));
}

unique_ptr<FunctionData> StopBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("stopped");
	return_types.push_back(LogicalType::BOOLEAN);
	return make_uniq<TableFunctionData>();
}

void StopScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<OnceState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto server = ObjectCache::GetObjectCache(context).Get<NotebookServer>(CACHE_KEY);
	bool running = server && server->Running();
	if (server) {
		server->Stop();
	}
	output.data[0].Append(Value::BOOLEAN(running));
}

} // namespace

void RegisterNotebookServer(ExtensionLoader &loader) {
	TableFunctionSet starts("notebook_start");
	for (auto arguments : {vector<LogicalType> {}, vector<LogicalType> {LogicalType::VARCHAR}}) {
		TableFunction start("notebook_start", arguments, StartScan, StartBind, OnceInit);
		start.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
			options.Add("port", LogicalType::INTEGER).Add("open", LogicalType::BOOLEAN);
		});
		starts.AddFunction(start);
	}
	loader.RegisterFunction(starts);
	loader.RegisterFunction(TableFunction("notebook_stop", {}, StopScan, StopBind, OnceInit));
}

} // namespace duckdb
