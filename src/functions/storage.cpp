#include "notebook/functions.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "notebook/source.hpp"
#include "notebook/script.hpp"

// Notebooks stored inside a database (docs/DESIGN.md section 5). The `_sketchbook` schema holds the
// same text as the file, one row per saved version. Databases written before the extension was renamed
// keep them in `_notebook`: they are read from there until the first save, which copies them over.
//   notebook_save(path, name := ..., message := ..., database := ..., force := ...)
//   notebook_checkout(name, file := ..., version := ..., database := ..., force := ...)
// Both run their SQL on a connection of their own, created in the scan and released after it, so they commit
// on their own: the caller's transaction does not cover them, and a ROLLBACK does not undo a save (docs/FORMAT.md
// section 7). They use the caller's current database (USE) unless `database :=` names one. A save reads the newest
// version and inserts the next one in one transaction, and (name, version) is the primary key, so two concurrent
// saves cannot take the same number: the one that loses the race is retried.
namespace duckdb {

namespace {

string Prefix(const string &database) {
	return database.empty() ? string() : notebook::QuoteIdentifier(database) + ".";
}

//! True if `schema.versions` exists in the database (the current one when `database` is empty).
bool HasVersions(Connection &connection, const string &database, const string &schema) {
	// Database and schema names are case-insensitive, as identifiers are.
	auto result = connection.Query(
	    "SELECT 1 FROM duckdb_tables() WHERE lower(schema_name) = lower(" + notebook::QuoteString(schema) +
	    ") AND table_name = 'versions' AND lower(database_name) = lower(" +
	    (database.empty() ? string("current_database()") : notebook::QuoteString(database)) + ")");
	if (result->HasError()) {
		return false;
	}
	auto chunk = result->Fetch();
	return chunk && chunk->size() > 0;
}

void Check(unique_ptr<QueryResult> result) {
	if (result->HasError()) {
		result->ThrowError();
	}
}

//! A transaction on a connection of the extension's own: begun here unless one is already open (then the caller's
//! owns it), committed by Commit, rolled back if it goes out of scope first (an error was thrown).
class OwnTransaction {
public:
	explicit OwnTransaction(Connection &connection) : connection(connection), active(connection.IsAutoCommit()) {
		if (active) {
			Check(connection.Query("BEGIN TRANSACTION"));
		}
	}
	~OwnTransaction() {
		if (active) {
			connection.Query("ROLLBACK"); // An error result here is ignored: the transaction is gone either way.
		}
	}
	void Commit() {
		if (active) {
			active = false;
			Check(connection.Query("COMMIT"));
		}
	}

private:
	Connection &connection;
	bool active;
};

//! True for the errors a concurrent writer causes (a conflict on the same key or catalog entry, or a version
//! number another save took first), after which the whole transaction can be tried again.
//! Decided by the error's type, not by words in its message: a notebook named "conflict" is no conflict.
bool IsConflict(const ErrorData &error) {
	return error.Type() == ExceptionType::TRANSACTION ||
	       (error.Type() == ExceptionType::CONSTRAINT &&
	        StringUtil::Contains(StringUtil::Lower(error.RawMessage()), "duplicate key"));
}

//! How many times a save is tried when concurrent saves keep conflicting with it.
constexpr int SAVE_ATTEMPTS = 20;

template <class... ARGS>
unique_ptr<QueryResult> Execute(Connection &connection, const string &sql, ARGS... args) {
	auto prepared = connection.Prepare(sql);
	if (prepared->HasError()) {
		throw InvalidInputException("%s", prepared->GetError());
	}
	vector<Value> values {Value(args)...};
	auto result = prepared->Execute(values);
	if (result->HasError()) {
		result->ThrowError();
	}
	return result;
}

//! Rows of a result as values, for the small queries here.
vector<vector<Value>> Rows(unique_ptr<QueryResult> result) {
	vector<vector<Value>> rows;
	while (auto chunk = result->Fetch()) {
		for (idx_t r = 0; r < chunk->size(); r++) {
			vector<Value> row;
			for (idx_t c = 0; c < chunk->ColumnCount(); c++) {
				row.push_back(chunk->GetValue(c, r));
			}
			rows.push_back(std::move(row));
		}
	}
	return rows;
}

string FileStem(const string &path) {
	auto name = path.substr(path.find_last_of("/\\") + 1);
	auto dot = name.rfind('.');
	return dot == string::npos || dot == 0 ? name : name.substr(0, dot);
}

struct SaveBindData : public TableFunctionData {
	string path, name, message, database;
	bool force = false;
};

unique_ptr<FunctionData> SaveBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<SaveBindData>();
	result->path = SourceArgument(input, 0, "notebook_save");
	result->name = NamedString(input, "name");
	if (result->name.empty()) {
		result->name = notebook::IsStoredSource(result->path) ? notebook::ParseStoredSource(result->path).name
		                                                      : FileStem(result->path);
	}
	auto bad_name = notebook::CheckStoredName(result->name);
	if (!bad_name.empty()) {
		throw InvalidInputException("notebook_save: %s", bad_name);
	}
	result->message = NamedString(input, "message");
	result->database = NamedString(input, "database");
	if (result->database.empty()) {
		result->database = CurrentDatabase(context);
	}
	result->force = NamedBool(input, "force");
	names.emplace_back("name");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("version");
	return_types.push_back(LogicalType::INTEGER);
	names.emplace_back("saved");
	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("note");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

void SaveScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<OnceState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &bind = data.bind_data->Cast<SaveBindData>();
	auto content = ReadNotebookText(context, bind.path);

	Connection connection(DatabaseInstance::GetDatabase(context));
	bool saved;
	string note;
	auto version = SaveNotebookVersion(connection, bind.database, bind.name, content, bind.message, bind.force, saved,
	                                   note, bind.path);
	output.data[0].Append(Value(bind.name));
	output.data[1].Append(Value::INTEGER(version));
	output.data[2].Append(Value::BOOLEAN(saved));
	output.data[3].Append(Value(note));
}

struct CheckoutBindData : public TableFunctionData {
	string name, to, database;
	int32_t version = 0;
	bool force = false;
};

unique_ptr<FunctionData> CheckoutBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<CheckoutBindData>();
	result->name = StringArgument(input, 0, "notebook_checkout", "the notebook name");
	if (result->name.empty()) {
		throw InvalidInputException("notebook_checkout: the notebook name is empty");
	}
	result->to = NamedString(input, "file");
	result->database = NamedString(input, "database");
	if (result->database.empty()) {
		result->database = CurrentDatabase(context);
	}
	result->force = NamedBool(input, "force");
	auto version = input.named_parameters.find("version");
	if (version != input.named_parameters.end() && !version->second.IsNull()) {
		auto problem = notebook::CheckStoredVersion(version->second.GetValue<int64_t>());
		if (!problem.empty()) {
			throw InvalidInputException("notebook_checkout: %s", problem);
		}
		result->version = version->second.GetValue<int32_t>();
	}
	names.emplace_back("name");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("version");
	return_types.push_back(LogicalType::INTEGER);
	names.emplace_back("path");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("content");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

void CheckoutScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<OnceState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &bind = data.bind_data->Cast<CheckoutBindData>();
	Connection connection(DatabaseInstance::GetDatabase(context));
	int32_t version;
	auto content = StoredNotebook(connection, bind.database, bind.name, bind.version, version);
	if (!bind.to.empty()) {
		auto &fs = FileSystem::GetFileSystem(context);
		if (fs.FileExists(bind.to) && !bind.force) {
			auto existing = ReadNotebookText(context, bind.to);
			if (existing != content) {
				auto known = Rows(Execute(connection,
				                          "SELECT count(*) FROM " + Prefix(bind.database) +
				                              StorageSchema(connection, bind.database) +
				                              ".versions WHERE name = ? AND content_hash = sha256(?)",
				                          bind.name, existing));
				if (known[0][0].GetValue<int64_t>() == 0) {
					throw InvalidInputException(
					    "notebook_checkout: \"%s\" has changes that are not saved in '%s'. Save them with "
					    "notebook_save, or check out with force := true",
					    bind.to, bind.name);
				}
			}
		}
		WriteTextFile(context, bind.to, content);
	}
	output.data[0].Append(Value(bind.name));
	output.data[1].Append(Value::INTEGER(version));
	output.data[2].Append(bind.to.empty() ? Value(LogicalType::VARCHAR) : Value(bind.to));
	output.data[3].Append(Value(content));
}

//! One attempt at SaveNotebookVersion: the newest version is read and the next one inserted in one transaction.
int32_t SaveOnce(Connection &connection, const string &database, const string &name, const string &content,
                 const string &message, bool force, bool &saved, string &note, const string &origin) {
	OwnTransaction transaction(connection);
	EnsureStorage(connection, database);
	auto prefix = Prefix(database);
	auto versions = Rows(Execute(connection,
	                             "SELECT version, content_hash FROM " + prefix +
	                                 "_sketchbook.versions WHERE name = ? ORDER BY version DESC",
	                             name));
	auto hash = Rows(Execute(connection, "SELECT sha256(?)", content))[0][0].GetValue<string>();
	int32_t current = versions.empty() ? 0 : versions[0][0].GetValue<int32_t>();
	if (!versions.empty() && versions[0][1].GetValue<string>() == hash) {
		transaction.Commit();
		saved = false;
		note = "unchanged since version " + std::to_string(current);
		return current;
	}
	if (!force) {
		for (idx_t i = 1; i < versions.size(); i++) {
			if (versions[i][1].GetValue<string>() == hash) {
				// The text is an older version: saving it would hide newer work.
				throw InvalidInputException(
				    "notebook_save: \"%s\" matches version %d of '%s', but version %d is newer. "
				    "Check out the newer version, or save with force := true",
				    origin, versions[i][0].GetValue<int32_t>(), name, current);
			}
		}
	}
	if (current == NumericLimits<int32_t>::Maximum()) {
		throw InvalidInputException("notebook_save: '%s' already has version %d, the last a version number can be",
		                            name, current);
	}
	Execute(connection, "INSERT INTO " + prefix + "_sketchbook.notebooks (name) VALUES (?) ON CONFLICT DO NOTHING",
	        name);
	Execute(connection,
	        "INSERT INTO " + prefix +
	            "_sketchbook.versions (name, version, content, content_hash, message) VALUES (?, ?, ?, ?, ?)",
	        name, current + 1, content, hash, message.empty() ? Value() : Value(message));
	transaction.Commit();
	saved = true;
	note = current == 0 ? "first version" : "new version";
	return current + 1;
}

} // namespace

int32_t SaveNotebookVersion(Connection &connection, const string &database, const string &name, const string &content,
                            const string &message, bool force, bool &saved, string &note, const string &origin) {
	for (int attempt = 1;; attempt++) {
		try {
			return SaveOnce(connection, database, name, content, message, force, saved, note, origin);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			// Another save of the same notebook committed first: read the newest version again and retry.
			if (attempt >= SAVE_ATTEMPTS || !connection.IsAutoCommit() || !IsConflict(error)) {
				throw;
			}
			// A short, growing, jittered pause, so that writers that keep meeting do not retry in step.
			ThreadUtil::SleepMicroSeconds(NumericCast<idx_t>(attempt) * 200 + UUID::GenerateRandomUUID().lower % 1000);
		}
	}
}

void EnsureStorage(Connection &connection, const string &database) {
	auto prefix = Prefix(database);
	// One transaction, so a failure (in the copy of an old history, say) leaves nothing behind: the old
	// history stays readable through StorageSchema and the copy is tried again on the next save. Whether
	// there is a new schema yet is read inside it.
	OwnTransaction transaction(connection);
	bool created = !HasVersions(connection, database, STORAGE_SCHEMA);
	Check(connection.Query("CREATE SCHEMA IF NOT EXISTS " + prefix + "_sketchbook"));
	Check(connection.Query("CREATE TABLE IF NOT EXISTS " + prefix +
	                       "_sketchbook.notebooks (name VARCHAR PRIMARY KEY, created_at TIMESTAMPTZ NOT NULL "
	                       "DEFAULT now())"));
	Check(connection.Query("CREATE TABLE IF NOT EXISTS " + prefix +
	                       "_sketchbook.versions (name VARCHAR NOT NULL REFERENCES " + prefix +
	                       "_sketchbook.notebooks (name), version INTEGER NOT NULL, content VARCHAR NOT NULL, "
	                       "content_hash VARCHAR NOT NULL, saved_at TIMESTAMPTZ NOT NULL DEFAULT now(), message "
	                       "VARCHAR, PRIMARY KEY (name, version))"));
	Check(connection.Query("CREATE VIEW IF NOT EXISTS " + prefix + "_sketchbook.current AS SELECT * FROM " + prefix +
	                       "_sketchbook.versions QUALIFY version = max(version) OVER (PARTITION BY name)"));
	// A database from before the rename: its history comes along on the first save, so versions carry
	// on from where they were. The old schema is left as it was.
	if (created && HasVersions(connection, database, LEGACY_STORAGE_SCHEMA)) {
		Check(connection.Query("INSERT INTO " + prefix + "_sketchbook.notebooks (name, created_at) SELECT name, " +
		                       "created_at FROM " + prefix + "_notebook.notebooks"));
		Check(connection.Query("INSERT INTO " + prefix +
		                       "_sketchbook.versions (name, version, content, content_hash, saved_at, message) " +
		                       "SELECT name, version, content, content_hash, saved_at, message FROM " + prefix +
		                       "_notebook.versions"));
	}
	transaction.Commit();
}

string StorageSchema(Connection &connection, const string &database) {
	if (!HasVersions(connection, database, STORAGE_SCHEMA) &&
	    HasVersions(connection, database, LEGACY_STORAGE_SCHEMA)) {
		return LEGACY_STORAGE_SCHEMA;
	}
	return STORAGE_SCHEMA;
}

string StoredNotebook(Connection &connection, const string &database, const string &name, int32_t version,
                      int32_t &found_version) {
	auto schema = StorageSchema(connection, database);
	if (!HasVersions(connection, database, schema)) {
		throw InvalidInputException("No notebooks are stored in %s: there is no _sketchbook.versions table",
		                            database.empty() ? string("this database") : "database \"" + database + "\"");
	}
	auto versions = Prefix(database) + schema + ".versions";
	vector<vector<Value>> rows;
	if (version > 0) {
		rows = Rows(Execute(connection, "SELECT version, content FROM " + versions + " WHERE name = ? AND version = ?",
		                    name, version));
	} else {
		rows = Rows(Execute(
		    connection, "SELECT version, content FROM " + versions + " WHERE name = ? ORDER BY version DESC LIMIT 1",
		    name));
	}
	if (rows.empty()) {
		throw InvalidInputException("No stored notebook '%s'%s", name,
		                            version > 0 ? " with version " + std::to_string(version) : string());
	}
	found_version = rows[0][0].GetValue<int32_t>();
	return rows[0][1].GetValue<string>();
}

void RegisterNotebookStorage(ExtensionLoader &loader) {
	TableFunction save("notebook_save", {LogicalType::VARCHAR}, SaveScan, SaveBind, OnceInit);
	save.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("name", LogicalType::VARCHAR)
		    .Add("message", LogicalType::VARCHAR)
		    .Add("database", LogicalType::VARCHAR)
		    .Add("force", LogicalType::BOOLEAN);
	});
	loader.RegisterFunction(save);

	TableFunction checkout("notebook_checkout", {LogicalType::VARCHAR}, CheckoutScan, CheckoutBind, OnceInit);
	checkout.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("file", LogicalType::VARCHAR)
		    .Add("version", LogicalType::INTEGER)
		    .Add("database", LogicalType::VARCHAR)
		    .Add("force", LogicalType::BOOLEAN);
	});
	loader.RegisterFunction(checkout);
}

} // namespace duckdb
