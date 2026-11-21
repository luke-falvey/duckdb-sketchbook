#include "notebook/functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "notebook/source.hpp"

namespace duckdb {

namespace {

string ReadFile(FileSystem &fs, const string &path) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto size = handle->GetFileSize();
	string contents(size, '\0');
	idx_t total = 0;
	while (total < size) {
		auto n = handle->Read(&contents[total], size - total);
		if (n <= 0) {
			throw IOException("Could not read notebook \"%s\"", path);
		}
		total += NumericCast<idx_t>(n);
	}
	return contents;
}

//! Writes `content` to a new file at `path`, syncs it and closes it, so write, sync and close errors all surface.
void WriteWhole(FileSystem &fs, const string &path, const string &content) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	handle->Write(const_cast<char *>(content.data()), content.size());
	handle->Sync();
	handle->Close();
}

//! Removes a temporary file if it is there; a failure to remove it must not hide the error being reported.
void RemoveQuietly(FileSystem &fs, const string &path) {
	try {
		fs.TryRemoveFile(path);
	} catch (...) { // NOLINT: best effort
	}
}

//! Replaces `path` atomically: the text goes to a uniquely named file beside it, which is then moved over it. A
//! failed write leaves the old file as it was, and a reader (the editor's file watcher) never sees half a file.
//! A symbolic link stays a link: the file it points to is the one replaced. The new file has the default mode,
//! not the old one's, and a hard link to the old file keeps the old text. An object store's path (s3://, gs://,
//! az://, http(s)://: FileSystem::IsRemoteFile) is written directly: it publishes a file only once its upload
//! completes, so that is already all-or-nothing, and a temporary file there would cost a second upload. Every other
//! file system (a network share, hdfs://, an extension's) shows a file as it is written, so it gets the move too.
void WriteFile(FileSystem &fs, const string &path, const string &content) {
	if (FileSystem::IsRemoteFile(path)) {
		WriteWhole(fs, path, content);
		return;
	}
	// Resolves a link (a path that is not local is left as it is). The writes after it go through the file system's
	// opener, which checks the resolved target against allowed_directories.
	auto target = fs.CanonicalizePath(path);
	auto temporary = target + "." + UUID::ToString(UUID::GenerateRandomUUID()) + ".tmp";
	try {
		WriteWhole(fs, temporary, content);
	} catch (...) {
		RemoveQuietly(fs, temporary);
		throw;
	}
	try {
		fs.MoveFile(temporary, target);
		return;
	} catch (NotImplementedException &) { // NOLINT: this file system cannot move files, so the target is written below
	} catch (...) {
		RemoveQuietly(fs, temporary);
		throw;
	}
	RemoveQuietly(fs, temporary);
	WriteWhole(fs, target, content);
}

//! A stored notebook from `database` (the default database when empty), named by a `db:` source.
string ReadStored(DatabaseInstance &db, const string &database, const string &source) {
	auto stored = notebook::ParseStoredSource(source);
	if (!stored.error.empty()) {
		throw InvalidInputException("%s", stored.error);
	}
	Connection connection(db);
	int32_t found;
	return StoredNotebook(connection, database, stored.name, stored.version, found);
}

} // namespace

string ReadNotebookText(DatabaseInstance &db, const string &source) {
	return ReadNotebookText(db, string(), source);
}

string ReadNotebookText(DatabaseInstance &db, const string &database, const string &source) {
	if (notebook::IsStoredSource(source)) {
		return ReadStored(db, database, source);
	}
	return ReadFile(FileSystem::GetFileSystem(db), source);
}

string ReadNotebookText(ClientContext &context, const string &source) {
	if (notebook::IsStoredSource(source)) {
		return ReadStored(DatabaseInstance::GetDatabase(context), CurrentDatabase(context), source);
	}
	return ReadFile(FileSystem::GetFileSystem(context), source);
}

string CurrentDatabase(ClientContext &context) {
	return DatabaseManager::TryGetDefaultDatabase(context).GetIdentifierName();
}

string StringArgument(TableFunctionBindInput &input, idx_t index, const char *function, const char *what) {
	auto &value = input.inputs[index];
	if (value.IsNull()) {
		throw InvalidInputException("%s: %s must not be NULL", function, what);
	}
	return value.GetValue<string>();
}

string SourceArgument(TableFunctionBindInput &input, idx_t index, const char *function) {
	auto source = StringArgument(input, index, function, "the notebook path");
	if (source.empty()) {
		throw InvalidInputException("%s: the notebook path is empty", function);
	}
	if (notebook::IsStoredSource(source)) {
		auto stored = notebook::ParseStoredSource(source);
		if (!stored.error.empty()) {
			throw InvalidInputException("%s: %s", function, stored.error);
		}
	}
	return source;
}

void WriteTextFile(ClientContext &context, const string &path, const string &content) {
	WriteFile(FileSystem::GetFileSystem(context), path, content);
}

void WriteTextFile(DatabaseInstance &db, const string &path, const string &content) {
	WriteFile(FileSystem::GetFileSystem(db), path, content);
}

notebook::Notebook AnalyzeText(ClientContext &context, const string &text) {
	auto notebook = notebook::ParseNotebook(text);
	notebook::AnalyzeNotebook(notebook, [&](const std::string &sql) { return ExtractReferences(context, sql); });
	return notebook;
}

notebook::Notebook AnalyzeText(DatabaseInstance &db, const string &text) {
	Connection connection(db);
	return AnalyzeText(*connection.context, text);
}

notebook::Notebook ReadNotebook(ClientContext &context, const string &path) {
	return AnalyzeText(context, ReadNotebookText(context, path));
}

void ThrowIfInvalid(const notebook::Notebook &notebook, const string &path) {
	for (auto &cell : notebook.cells) {
		if (!cell.errors.empty()) {
			// Report one error here; notebook_cells reports all of them as data.
			throw InvalidInputException("Notebook \"%s\" has errors. First one, at line %d: %s\n"
			                            "List them all with: SELECT start_line, errors FROM notebook_cells('%s') "
			                            "WHERE len(errors) > 0",
			                            path, cell.start_line, cell.errors[0], path);
		}
	}
}

unique_ptr<GlobalTableFunctionState> OnceInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<OnceState>();
}

string NamedString(TableFunctionBindInput &input, const char *key) {
	auto found = input.named_parameters.find(key);
	return found == input.named_parameters.end() || found->second.IsNull() ? string()
	                                                                       : found->second.GetValue<string>();
}

bool NamedBool(TableFunctionBindInput &input, const char *key) {
	auto found = input.named_parameters.find(key);
	return found != input.named_parameters.end() && !found->second.IsNull() && found->second.GetValue<bool>();
}

} // namespace duckdb
