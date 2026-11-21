#include "notebook/cache.hpp"

#include "notebook/chart.hpp"
#include "notebook/graph.hpp"
#include "notebook/script.hpp"
#include "notebook/sql_lexer.hpp"
#include "notebook/yaml.hpp"
#include "notebook/text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <set>

namespace notebook {

// 3: keys cover the versions of the files a cell names.
const int CACHE_FORMAT = 3;
const size_t CACHE_MAX_RESULT_BYTES = 4 * 1024 * 1024;
const char *const CACHE_STATE_ID = "@state";

namespace {

//! Builds the text a key is the hash of. Every field is written with its length, so two different
//! lists of fields never produce the same text.
struct KeyText {
	std::string text;

	KeyText &Add(const std::string &field) {
		text += std::to_string(field.size());
		text += ':';
		text += field;
		return *this;
	}
};

//! Options that never change a result: where and whether the cell is shown, and when it runs.
bool IsLayoutOption(const std::string &key) {
	return key == "hide" || key == "span" || key == "run" || key == "screen";
}

//! Options that change what a cell shows but not what the cells reading it get.
bool IsDisplayOption(const std::string &key) {
	return key == "rows" || key == "width" || key == "height" || key == "label" || key == "placeholder";
}

//! Selections that only a query cell's charts read (filter:), not its SQL.
std::vector<std::string> ChartOnlyVariables(const Cell &cell) {
	std::vector<std::string> result;
	if (cell.kind != CellKind::SQL) {
		return result;
	}
	for (auto &spec : cell.charts) {
		for (auto &name : ChartFilters(spec)) {
			if (!Contains(cell.variables, name) && !Contains(result, name)) {
				result.push_back(name);
			}
		}
	}
	return result;
}

//! A word, a string literal's text, a quoted identifier or one punctuation character of SQL code.
struct SourceToken {
	enum class Kind { WORD, STRING, NAME, PUNCT } kind;
	//! Words lower case; strings and names decoded.
	std::string text;
};

std::vector<SourceToken> SourceTokens(const std::string &sql) {
	std::vector<SourceToken> tokens;
	SqlScanner scan(sql);
	while (!scan.Done()) {
		size_t start = scan.Position();
		char c = sql[start];
		if (scan.CurrentState() != SqlScanner::State::CODE) {
			// A comment.
			scan.Advance();
			continue;
		}
		if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
			size_t end = start;
			while (end < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[end])) || sql[end] == '_')) {
				end++;
			}
			tokens.push_back({SourceToken::Kind::WORD, Lower(sql.substr(start, end - start))});
			while (scan.Position() < end) {
				scan.Advance();
			}
			continue;
		}
		scan.Advance();
		auto state = scan.CurrentState();
		if (state == SqlScanner::State::STRING || state == SqlScanner::State::DOLLAR_STRING ||
		    state == SqlScanner::State::QUOTED_IDENTIFIER) {
			SourceToken token {state == SqlScanner::State::QUOTED_IDENTIFIER ? SourceToken::Kind::NAME
			                                                                 : SourceToken::Kind::STRING,
			                   std::string()};
			while (!scan.Done() && scan.CurrentState() == state) {
				size_t at = scan.Position();
				size_t step = scan.Advance();
				if (scan.CurrentState() == state) {
					// A doubled quote is one quote.
					token.text += step == 2 ? sql.substr(at, 1) : sql.substr(at, step);
				}
			}
			tokens.push_back(std::move(token));
		} else if (state == SqlScanner::State::CODE && !std::isspace(static_cast<unsigned char>(c))) {
			tokens.push_back({SourceToken::Kind::PUNCT, std::string(1, c)});
		}
	}
	return tokens;
}

//! Table functions whose first argument is a file, a glob or a list of them.
bool IsFileReader(const std::string &name) {
	static const char *const NAMES[] = {"read_csv",
	                                    "read_csv_auto",
	                                    "read_parquet",
	                                    "parquet_scan",
	                                    "read_json",
	                                    "read_json_auto",
	                                    "read_ndjson",
	                                    "read_ndjson_auto",
	                                    "read_json_objects",
	                                    "read_ndjson_objects",
	                                    "read_json_objects_auto",
	                                    "read_text",
	                                    "read_blob",
	                                    "read_xlsx",
	                                    "sniff_csv",
	                                    "glob",
	                                    "parquet_metadata",
	                                    "parquet_schema",
	                                    "parquet_file_metadata",
	                                    "parquet_kv_metadata"};
	for (auto known : NAMES) {
		if (name == known) {
			return true;
		}
	}
	return false;
}

bool EndsWith(const std::string &text, const std::string &end) {
	return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

//! Other functions that read data from outside the database: the cache cannot tell when it changes.
bool IsExternalScan(const std::string &name) {
	return name.compare(0, 5, "read_") == 0 || EndsWith(name, "_scan") || EndsWith(name, "_query") || name == "query" ||
	       name == "query_table" || name.compare(0, 8, "iceberg_") == 0;
}

void AddSources(const std::string &sql, CacheSources &sources) {
	auto tokens = SourceTokens(sql);
	using Kind = SourceToken::Kind;
	auto is = [&](size_t i, Kind kind, const char *text = nullptr) {
		return i < tokens.size() && tokens[i].kind == kind && (!text || tokens[i].text == text);
	};
	std::string first;
	for (size_t i = 0; i < tokens.size(); i++) {
		auto &token = tokens[i];
		if (is(i, Kind::PUNCT, ";")) {
			first.clear();
			continue;
		}
		if (token.kind != Kind::WORD) {
			continue;
		}
		if (first.empty()) {
			first = token.text;
		}
		// `INSTALL x FROM 'repository'` names no data.
		if (first == "install" || first == "force") {
			continue;
		}
		if ((token.text == "from" || token.text == "join") && is(i + 1, Kind::STRING)) {
			AddUnique(sources.files, tokens[i + 1].text);
			continue;
		}
		if (!is(i + 1, Kind::PUNCT, "(") || (i > 0 && is(i - 1, Kind::PUNCT, "."))) {
			continue;
		}
		if (!IsFileReader(token.text)) {
			sources.unchecked = sources.unchecked || IsExternalScan(token.text);
			continue;
		}
		size_t j = i + 2;
		if (is(j, Kind::STRING)) {
			AddUnique(sources.files, tokens[j].text);
			continue;
		}
		if (!is(j, Kind::PUNCT, "[")) {
			sources.unchecked = true;
			continue;
		}
		for (j++; j < tokens.size() && !is(j, Kind::PUNCT, "]"); j++) {
			if (is(j, Kind::STRING)) {
				AddUnique(sources.files, tokens[j].text);
			} else if (!is(j, Kind::PUNCT, ",")) {
				sources.unchecked = true;
			}
		}
	}
}

void AddVariable(KeyText &text, const std::string &name, const std::map<std::string, std::string> &variables) {
	auto found = variables.find(name);
	text.Add(name).Add(found == variables.end() ? "unset" : "set").Add(found == variables.end() ? "" : found->second);
}

//! The files a view cell (IsLazyView) reads when a later cell selects from it: all of them, since nothing else in the
//! cell keeps what it read.
std::vector<std::string> LazyFiles(const Cell &cell) {
	return CellSources(cell).files;
}

struct KeyBuilder {
	const Notebook &notebook;
	const std::map<std::string, std::string> &variables;
	const std::map<std::string, std::string> &files;
	const std::map<size_t, std::map<std::string, std::string>> &cell_files;
	std::vector<CacheKey> keys;
	//! 0: not started, 1: being computed, 2: done.
	std::vector<int> state;
	//! Every name a cell defines.
	std::set<std::string> defined;
	//! ViewKey's results, by view and by the versions it was keyed with (ViewSignature).
	std::map<std::pair<size_t, std::string>, CacheKey> view_keys;
	//! ViewFiles and IsView, by cell.
	std::map<size_t, std::vector<std::string>> view_files;
	std::map<size_t, bool> is_view;
	std::map<size_t, bool> read_later;
	//! A view's signature with its own versions, by cell.
	std::map<size_t, std::string> own_signature;

	KeyBuilder(const Notebook &notebook, const std::map<std::string, std::string> &variables,
	           const std::map<std::string, std::string> &files,
	           const std::map<size_t, std::map<std::string, std::string>> &cell_files)
	    : notebook(notebook), variables(variables), files(files), cell_files(cell_files), keys(notebook.cells.size()),
	      state(notebook.cells.size(), 0) {
		for (auto &cell : notebook.cells) {
			defined.insert(cell.defines.begin(), cell.defines.end());
		}
	}

	//! False if the cell reads a relation that no cell defines under exactly that name: a table already in
	//! the database, a table of a database or schema a cell attached or created, a catalog view. A string
	//! after FROM is read as a relation of that name too, and is one of the cell's files instead.
	bool ReadsOnlyCells(const Cell &cell, const CacheSources &sources) const {
		for (auto &name : cell.reads) {
			if (name.empty() || name[0] == '$' || defined.count(name)) {
				continue;
			}
			bool file = false;
			for (auto &path : sources.files) {
				file = file || Lower(path) == name;
			}
			if (!file) {
				return false;
			}
		}
		return true;
	}

	const std::string &DataKey(size_t index) {
		static const std::string cycle = "cycle";
		if (state[index] == 1) {
			// A dependency cycle is already an error on its cells; the key only has to be stable.
			return cycle;
		}
		if (state[index] == 0) {
			Compute(index);
		}
		return keys[index].data;
	}

	//! The versions a cell is keyed with: those taken just before it ran (cell_files), else `files`.
	const std::map<std::string, std::string> &VersionsOf(size_t index) const {
		auto own = cell_files.find(index);
		return own == cell_files.end() ? files : own->second;
	}

	//! A view cell (IsLazyView: it makes views and keeps nothing) is read when a cell selects from it: as a
	//! dependency, it is keyed with the reading cell's versions.
	//! A cell that is not a view but defines a view (DefinesLazily): its key has the
	//! versions of when it ran, not of when a reader used what it defines, so a reader cannot be vouched for.
	bool ReadLater(size_t index) {
		auto found = read_later.find(index);
		if (found == read_later.end()) {
			found = read_later.emplace(index, !IsView(index) && DefinesLazily(notebook.cells[index])).first;
		}
		return found->second;
	}

	bool IsView(size_t index) {
		auto found = is_view.find(index);
		if (found == is_view.end()) {
			found = is_view.emplace(index, IsLazyView(notebook.cells[index])).first;
		}
		return found->second;
	}

	//! The files whose versions decide a view's key for a reader: the lazily read files of it and of the views it
	//! reads.
	const std::vector<std::string> &ViewFiles(size_t index) {
		auto found = view_files.find(index);
		if (found != view_files.end()) {
			return found->second;
		}
		// Built from the views it reads, each once.
		auto &files = view_files[index];
		files = LazyFiles(notebook.cells[index]);
		std::set<std::string> seen(files.begin(), files.end());
		for (int dependency : notebook.cells[index].depends_on) {
			if (dependency < 0 || static_cast<size_t>(dependency) >= notebook.cells.size() ||
			    static_cast<size_t>(dependency) == index || !IsView(static_cast<size_t>(dependency))) {
				continue;
			}
			// A cycle among views finds the entry being built (partial so far): it is an error on its cells anyway.
			// References into a std::map stay valid as it grows.
			auto &inner = ViewFiles(static_cast<size_t>(dependency));
			for (size_t i = 0; i < inner.size(); i++) {
				if (seen.insert(inner[i]).second) {
					files.push_back(inner[i]);
				}
			}
		}
		return files;
	}

	//! Those versions as text: readers that saw the same versions share one key.
	std::string ViewSignature(size_t index, const std::map<std::string, std::string> &versions) {
		KeyText text;
		for (auto &path : ViewFiles(index)) {
			auto found = versions.find(path);
			text.Add(path).Add(found == versions.end() ? std::string() : found->second);
		}
		return text.text;
	}

	//! The data key of view `index` as a cell keyed with `versions` reads it.
	const CacheKey &ViewKey(size_t index, const std::map<std::string, std::string> &versions) {
		if (&versions == &VersionsOf(index)) {
			DataKey(index);
			return keys[index];
		}
		auto signature = ViewSignature(index, versions);
		auto own = own_signature.find(index);
		if (own == own_signature.end()) {
			own = own_signature.emplace(index, ViewSignature(index, VersionsOf(index))).first;
		}
		if (signature == own->second) {
			DataKey(index);
			return keys[index];
		}
		auto key = std::make_pair(index, signature);
		auto found = view_keys.find(key);
		if (found != view_keys.end()) {
			return found->second;
		}
		// A cycle among views is an error on its cells already; the key only has to be stable.
		auto &slot = view_keys[key];
		slot.data = "cycle";
		CacheKey built;
		Build(index, versions, built);
		slot = built;
		return slot;
	}

	//! The data key of `index` (and its result key) with `versions` for its files and for the views it reads.
	void Build(size_t index, const std::map<std::string, std::string> &versions, CacheKey &out) {
		auto &cell = notebook.cells[index];
		auto chart_only = ChartOnlyVariables(cell);

		KeyText data;
		data.Add("data").Add(std::to_string(CACHE_FORMAT)).Add(CellKindName(cell.kind)).Add(cell.name).Add(cell.widget);
		for (auto &option : cell.options) {
			if (!IsLayoutOption(option.first) && !IsDisplayOption(option.first)) {
				data.Add(option.first).Add(option.second);
			}
		}
		// A query cell's charts are not part of what it computes for others.
		data.Add(cell.kind == CellKind::SQL ? cell.sql : cell.body);
		auto sources = CellSources(cell);
		bool verified = !sources.unchecked && ReadsOnlyCells(cell, sources);
		// `versions` is the cell's own, or a reader's when the cell is a view keyed for it (ViewKey): a view keeps
		// nothing, so all it reads is read as the reader runs.
		for (auto &path : sources.files) {
			auto found = versions.find(path);
			auto version = found == versions.end() ? std::string() : found->second;
			// `?` marks a version that may still change (ModifiedTooRecently): the key covers it, but cannot vouch
			// for it.
			verified = verified && !version.empty() && !IsUnsure(version);
			data.Add("file").Add(path).Add(StripUnsure(version));
		}
		for (int dependency : cell.depends_on) {
			if (dependency < 0 || static_cast<size_t>(dependency) >= notebook.cells.size()) {
				continue;
			}
			auto at = static_cast<size_t>(dependency);
			if (IsView(at)) {
				auto &view = ViewKey(at, versions);
				data.Add(view.data);
				verified = verified && view.verified;
			} else {
				data.Add(DataKey(at));
				verified = verified && !ReadLater(at);
				// A cell in a cycle (state 1) is already an error; it does not make the key unverified.
				verified = verified && keys[at].verified;
			}
		}
		out.verified = verified;
		std::vector<std::string> read;
		for (auto &name : cell.reads) {
			if (name.size() > 1 && name[0] == '$' && !Contains(chart_only, name.substr(1))) {
				read.push_back(name.substr(1));
			}
		}
		std::sort(read.begin(), read.end());
		for (auto &name : read) {
			AddVariable(data, name, variables);
		}
		out.data = CacheHash(data.text);

		KeyText result;
		result.Add("result").Add(out.data).Add(cell.body);
		for (auto &option : cell.options) {
			if (IsDisplayOption(option.first)) {
				result.Add(option.first).Add(option.second);
			}
		}
		std::sort(chart_only.begin(), chart_only.end());
		for (auto &name : chart_only) {
			AddVariable(result, name, variables);
		}
		out.result = CacheHash(result.text);
	}

	void Compute(size_t index) {
		state[index] = 1;
		CacheKey built;
		Build(index, VersionsOf(index), built);
		keys[index] = built;
		state[index] = 2;
	}
};

bool IsArrayOf(const Json *json, Json::Type type) {
	if (!json || json->GetType() != Json::Type::ARRAY) {
		return false;
	}
	for (auto &item : json->Items()) {
		if (item.GetType() != type) {
			return false;
		}
	}
	return true;
}

//! `{columns: [{name, type}], rows: [[...]]}`, as Runner::Page makes it.
bool ValidPage(const Json &page) {
	if (page.GetType() != Json::Type::OBJECT || !IsArrayOf(page.Get("columns"), Json::Type::OBJECT) ||
	    !IsArrayOf(page.Get("rows"), Json::Type::ARRAY)) {
		return false;
	}
	for (auto &column : page.Get("columns")->Items()) {
		auto name = column.Get("name");
		if (!name || name->GetType() != Json::Type::STRING) {
			return false;
		}
	}
	auto width = page.Get("columns")->Size();
	for (auto &row : page.Get("rows")->Items()) {
		if (row.Size() != width) {
			return false;
		}
	}
	return true;
}

//! `{render: {...}, datasets: [page...]}` or `{error}`, as Runner::Chart makes it.
bool ValidChart(const Json &chart) {
	if (chart.GetType() != Json::Type::OBJECT) {
		return false;
	}
	if (auto error = chart.Get("error")) {
		return error->GetType() == Json::Type::STRING;
	}
	auto render = chart.Get("render");
	if (!render || render->GetType() != Json::Type::OBJECT || !IsArrayOf(chart.Get("datasets"), Json::Type::OBJECT)) {
		return false;
	}
	for (auto &dataset : chart.Get("datasets")->Items()) {
		if (!ValidPage(dataset)) {
			return false;
		}
	}
	return true;
}

bool ValidResult(const Json &result) {
	if (result.GetType() != Json::Type::OBJECT) {
		return false;
	}
	auto status = result.Get("status");
	if (!status || status->GetType() != Json::Type::STRING ||
	    (status->Str() != "ok" && status->Str() != "error" && status->Str() != "skipped")) {
		return false;
	}
	auto error = result.Get("error");
	if (status->Str() == "error" && (!error || error->GetType() != Json::Type::STRING)) {
		return false;
	}
	if (auto output = result.Get("output")) {
		if (output->GetType() != Json::Type::STRING) {
			return false;
		}
	}
	if (auto data = result.Get("data")) {
		// A query cell's page, or a chart cell's chart.
		if (!ValidPage(*data) && !ValidChart(*data)) {
			return false;
		}
	}
	if (auto charts = result.Get("charts")) {
		if (charts->GetType() != Json::Type::ARRAY) {
			return false;
		}
		for (auto &chart : charts->Items()) {
			if (!ValidChart(chart)) {
				return false;
			}
		}
	}
	if (auto choices = result.Get("choices")) {
		if (choices->GetType() != Json::Type::ARRAY) {
			return false;
		}
	}
	return true;
}

const std::string *StringField(const Json &json, const char *key) {
	auto field = json.Get(key);
	return field && field->GetType() == Json::Type::STRING ? &field->Str() : nullptr;
}

bool HasFormat(const Json &json) {
	auto format = json.Get("format");
	return json.GetType() == Json::Type::OBJECT && format && format->GetType() == Json::Type::NUMBER &&
	       format->Num() == CACHE_FORMAT;
}

} // namespace

std::string CacheHash(const std::string &text) {
	uint64_t hash = 14695981039346656037ULL;
	for (unsigned char c : text) {
		hash ^= c;
		hash *= 1099511628211ULL;
	}
	char buffer[17];
	snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
	return buffer;
}

std::string CacheVariableKey(const std::string &type, const std::string &text, bool is_null) {
	KeyText key;
	key.Add(type).Add(is_null ? "null" : "value").Add(is_null ? std::string() : text);
	return key.text;
}

//! Table functions whose rows depend on their arguments alone.
bool IsPureTableFunction(const std::string &name) {
	static const char *const NAMES[] = {"range", "generate_series", "unnest", "json_each", "json_tree", "repeat_row"};
	for (auto known : NAMES) {
		if (name == known) {
			return true;
		}
	}
	return false;
}

CacheSources CellSources(const Cell &cell) {
	CacheSources sources;
	if (cell.kind == CellKind::SQL || cell.kind == CellKind::INPUT) {
		AddSources(cell.sql, sources);
	}
	// Any other table function (an extension's reader such as st_read, a table macro, duckdb_tables()) reads
	// something the cache cannot look at. File readers are judged by their arguments (AddSources).
	for (auto &name : cell.table_functions) {
		if (!IsFileReader(name) && !IsPureTableFunction(name)) {
			sources.unchecked = true;
		}
	}
	for (auto &expression : cell.expressions) {
		AddSources(expression.sql, sources);
	}
	return sources;
}

namespace {

bool IsSeparator(char c) {
	return c == '/' || c == '\\';
}

//! Whether a glob stays off other machines. Only its last segment may be a pattern, and not `**`: the glob then lists
//! one directory the path names literally, and goes into nothing it matched. Each match is judged again before it is
//! looked at (FileVersion), so a match under `/net` is refused there. A pattern in a directory segment could go
//! anywhere a link or the working directory leads (`/n?t/host`, `*/host` from `/`, `~/n?t` with $HOME at `/`,
//! `/proc/self/root/n?t`), and `**` walks everything below it. Nothing is matched here, so no reading of DuckDB's
//! glob syntax can be got wrong; a refused glob is only unverified. A link in the listed directory is still followed
//! when the directory is listed (DuckDB stats every entry), as a linked literal path always was.
bool GlobStaysLocal(const std::string &raw) {
	// `\\?\` is a prefix on Windows, but POSIX reads `\\?\C:\x` as `/?/C:/x`, a pattern in a directory: such a
	// path is unverified on both, since this code cannot tell which reads it.
	auto &path = raw;
	auto last = path.find_last_of("/\\");
	auto directory = last == std::string::npos ? std::string() : path.substr(0, last);
	auto name = last == std::string::npos ? path : path.substr(last + 1);
	return directory.find_first_of("*?[") == std::string::npos && name.find("**") == std::string::npos;
}

//! A path as the file system opens it (after a `file:` prefix is stripped) that names no other machine. Two separators
//! begin a UNC path (`\\host\share`, `//host/share`) or a device path (`\\.\UNC\host`, `\\?\UNC\host`,
//! `\\.\GLOBALROOT\…`, `\\.\C:\..\UNC\…`, which Windows normalises onto a share); of those only `\\?\` and a
//! drive (`\\?\C:\x`, an extended-length local path) is local. `\??\` is the NT
//! namespace; `\\?\C:\..\` is refused, since normalising it climbs off the drive. POSIX reads `//x` as `/x`, but it
//! is refused too: a path cannot say which system it was written for. A
//! drive letter mapped to a network share (`Z:\x`) cannot be told from a local one, and counts as local.
bool NamesThisMachine(const std::string &path) {
	// The automounter's view of other machines' exports (autofs's and macOS's -hosts map, `/net/host/...`,
	// `/Network/...`): looking is contacting them. Matched as any `net` or `network` segment, in any case, so that
	// `/./net`, `/tmp/../net` and macOS's case-insensitive `/NETWORK` are caught; a local directory of that name only
	// makes its files unverified.
	size_t begin = 0;
	for (size_t i = 0; i <= path.size(); i++) {
		if (i == path.size() || IsSeparator(path[i])) {
			auto segment = Lower(path.substr(begin, i - begin));
			if (segment == "net" || segment == "network") {
				return false;
			}
			begin = i + 1;
		}
	}
	if (!GlobStaysLocal(path)) {
		return false;
	}
	if (path.compare(0, 4, "\\??\\") == 0) {
		return false;
	}
	if (path.size() >= 2 && IsSeparator(path[0]) && IsSeparator(path[1])) {
		if (path.size() < 7 || path.compare(0, 4, "\\\\?\\") != 0 ||
		    !std::isalpha(static_cast<unsigned char>(path[4])) || path[5] != ':' || !IsSeparator(path[6])) {
			return false;
		}
		// DuckDB normalises even these paths on Windows (GetFullPathNameW), so a `.` or `..` segment could climb from
		// the drive onto `\\?\UNC\…`.
		size_t start = 7;
		for (size_t i = 7; i <= path.size(); i++) {
			if (i == path.size() || IsSeparator(path[i])) {
				auto segment = path.substr(start, i - start);
				// Windows also drops trailing dots and spaces from a name, so `.. ` or `...` may climb too.
				if (!segment.empty() && segment.find_first_not_of(". ") == std::string::npos) {
					return false;
				}
				start = i + 1;
			}
		}
		return true;
	}
	return true;
}

//! Where DuckDB's file system starts reading a `file:` URL (FileSystem::ExpandPath) on POSIX: `file:/x` at 5,
//! `file:///x` at 7, `file://localhost/x` at 16; Windows starts one later. 0 for any other form, which it opens as it
//! is.
size_t FileUrlOffset(const std::string &lower) {
	if (lower.compare(0, 6, "file:/") != 0) {
		return 0;
	}
	if (lower.size() < 7 || lower[6] != '/') {
		return 5;
	}
	if (lower.size() >= 8 && lower[7] == '/') {
		return 7;
	}
	if (lower.compare(7, 10, "localhost/") == 0) {
		return 16;
	}
	return 0;
}

} // namespace

bool IsLocalPath(const std::string &path) {
	if (path.empty()) {
		return false;
	}
	// DuckDB expands `~` as $HOME followed by the rest, separator or not: with $HOME at `/`, `~net/host` is
	// `/net/host`. A `~` must be followed by a separator for the rest to be judged as written.
	if (path[0] == '~' && path.size() > 1 && !IsSeparator(path[1])) {
		return false;
	}
	auto lower = Lower(path);
	if (lower.compare(0, 5, "file:") == 0) {
		// Whichever platform reads it: what is left after the prefix must name this machine, both ways.
		auto offset = FileUrlOffset(lower);
		if (offset == 0) {
			// `file://host/x`, or a form DuckDB does not strip.
			return false;
		}
		return NamesThisMachine(path.substr(offset)) &&
		       (offset + 1 > path.size() || NamesThisMachine(path.substr(offset + 1)));
	}
	if (path.find("://") != std::string::npos) {
		return false;
	}
	return NamesThisMachine(path);
}

bool ModifiedTooRecently(int64_t modified, int64_t now) {
	// Two seconds: one for the whole-second modified time, one for a clock that is a little behind the file system's.
	const int64_t MARGIN = 2000000;
	return modified > now - MARGIN;
}

std::vector<std::string> CacheSourceFiles(const Notebook &notebook) {
	std::vector<std::string> files;
	for (auto &cell : notebook.cells) {
		for (auto &path : CellSources(cell).files) {
			AddUnique(files, path);
		}
	}
	return files;
}

namespace {

//! A query whose CTE changes data (`WITH x AS (INSERT ... RETURNING *) SELECT * FROM x`) keeps something.
bool ChangesData(const std::string &statement) {
	for (auto &token : SourceTokens(statement)) {
		if (token.kind == SourceToken::Kind::WORD &&
		    (token.text == "insert" || token.text == "update" || token.text == "delete" || token.text == "merge")) {
			return true;
		}
	}
	return false;
}

} // namespace

bool IsLazyView(const Cell &cell) {
	if (cell.kind != CellKind::SQL) {
		return false;
	}
	auto statements = SplitStatements(cell.sql);
	bool named = cell.is_query && !cell.name.empty();
	bool view = false;
	for (size_t i = 0; i < statements.size(); i++) {
		auto &text = statements[i].text;
		if (ChangesData(text)) {
			return false;
		}
		if (i + 1 == statements.size() && named) {
			// The final query is a view, unless materialize=table makes it a table, which keeps what it read.
			if (cell.Option("materialize", "view") == "table") {
				return false;
			}
			view = true;
		} else if (IsCreateView(text)) {
			view = true;
		} else if (!IsQueryStatement(text)) {
			// It may keep what it reads (a table, a variable, an insert), as the cell ran: the whole cell is keyed
			// with its own versions, the views it reads included.
			return false;
		}
	}
	return view;
}

bool DefinesLazily(const Cell &cell) {
	if (cell.kind != CellKind::SQL) {
		return false;
	}
	if (cell.is_query && !cell.name.empty() && cell.Option("materialize", "view") != "table") {
		return true;
	}
	for (auto &statement : SplitStatements(cell.sql)) {
		if (IsCreateView(statement.text)) {
			return true;
		}
	}
	return false;
}

std::vector<size_t> CacheViewClosure(const Notebook &notebook, size_t index) {
	std::vector<size_t> views;
	if (index >= notebook.cells.size()) {
		return views;
	}
	std::set<size_t> seen;
	std::vector<int> pending = notebook.cells[index].depends_on;
	while (!pending.empty()) {
		auto dependency = pending.back();
		pending.pop_back();
		if (dependency < 0 || static_cast<size_t>(dependency) >= notebook.cells.size() ||
		    !seen.insert(static_cast<size_t>(dependency)).second) {
			continue;
		}
		auto &cell = notebook.cells[static_cast<size_t>(dependency)];
		if (!IsLazyView(cell)) {
			continue;
		}
		views.push_back(static_cast<size_t>(dependency));
		pending.insert(pending.end(), cell.depends_on.begin(), cell.depends_on.end());
	}
	std::sort(views.begin(), views.end());
	return views;
}

std::vector<std::string> CacheReadFiles(const Notebook &notebook, size_t index) {
	std::vector<std::string> files;
	if (index >= notebook.cells.size()) {
		return files;
	}
	files = CellSources(notebook.cells[index]).files;
	for (auto view : CacheViewClosure(notebook, index)) {
		for (auto &path : LazyFiles(notebook.cells[view])) {
			AddUnique(files, path);
		}
	}
	return files;
}

std::vector<CacheKey> CacheKeys(const Notebook &notebook, const std::map<std::string, std::string> &variables,
                                const std::map<std::string, std::string> &files,
                                const std::map<size_t, std::map<std::string, std::string>> &cell_files) {
	KeyBuilder builder(notebook, variables, files, cell_files);
	for (size_t i = 0; i < notebook.cells.size(); i++) {
		builder.DataKey(i);
	}
	return builder.keys;
}

std::vector<std::string> CacheCellIds(const Notebook &notebook) {
	std::vector<std::string> ids;
	std::map<std::string, int> seen;
	for (auto &cell : notebook.cells) {
		if (!cell.name.empty()) {
			ids.push_back(cell.name);
			continue;
		}
		KeyText text;
		text.Add(CellKindName(cell.kind)).Add(cell.head).Add(cell.body);
		auto id = "@" + CacheHash(text.text);
		// Two unnamed cells with the same text are told apart by their order.
		auto count = ++seen[id];
		ids.push_back(count == 1 ? id : id + "." + std::to_string(count));
	}
	return ids;
}

std::string CacheFileName(const std::string &id) {
	std::string name;
	for (unsigned char c : id) {
		if (std::isalnum(c) || c == '_' || c == '.' || c == '@' || c == '-') {
			name += static_cast<char>(c);
		} else {
			char buffer[4];
			snprintf(buffer, sizeof(buffer), "%%%02X", c);
			name += buffer;
		}
	}
	return name;
}

bool CacheEnabled(const Notebook &notebook) {
	for (auto &cell : notebook.cells) {
		if (cell.kind != CellKind::NOTEBOOK) {
			continue;
		}
		std::string error;
		auto meta = ParseYaml(cell.content, error);
		auto cache = meta.Get("cache");
		if (cache && cache->IsScalar() && cache->scalar == "false") {
			return false;
		}
	}
	return true;
}

std::set<std::string> SettableVariables(const Notebook &notebook) {
	std::set<std::string> names;
	for (auto &cell : notebook.cells) {
		if (cell.kind == CellKind::INPUT && !cell.name.empty()) {
			names.insert(cell.name);
		}
		for (auto &spec : cell.charts) {
			auto selection = ChartSelection(spec);
			if (!selection.empty()) {
				names.insert(selection);
			}
		}
	}
	return names;
}

bool ValidCacheResult(const std::string &json) {
	if (json.size() > CACHE_MAX_RESULT_BYTES) {
		return false;
	}
	std::string error;
	auto result = ParseJson(json, error);
	return error.empty() && ValidResult(result);
}

std::string DumpCacheEntry(const std::string &id, const CacheEntry &entry) {
	return Json::Object()
	    .Set("format", Json::Number(CACHE_FORMAT))
	    .Set("cell", Json::String(id))
	    .Set("key", Json::String(entry.key))
	    .Set("data", Json::String(entry.data))
	    .Set("saved_at", Json::String(entry.saved_at))
	    .Set("result", Json::Raw(entry.result))
	    .Dump();
}

bool ParseCacheEntry(const std::string &text, const std::string &id, CacheEntry &entry) {
	// The wrapper adds little to the result, so this also bounds what is parsed.
	if (text.size() > CACHE_MAX_RESULT_BYTES + 4096) {
		return false;
	}
	std::string error;
	auto json = ParseJson(text, error);
	if (!error.empty() || !HasFormat(json)) {
		return false;
	}
	auto cell = StringField(json, "cell"), key = StringField(json, "key"), saved_at = StringField(json, "saved_at");
	auto data = StringField(json, "data");
	auto result = json.Get("result");
	if (!cell || *cell != id || !key || key->empty() || !data || !saved_at || !result || !ValidResult(*result)) {
		return false;
	}
	auto dumped = result->Dump();
	if (dumped.size() > CACHE_MAX_RESULT_BYTES) {
		return false;
	}
	entry.key = *key;
	entry.data = *data;
	entry.saved_at = *saved_at;
	entry.result = std::move(dumped);
	return true;
}

std::string DumpCacheState(const CacheState &state) {
	auto variables = Json::Object();
	for (auto &entry : state.variables) {
		auto variable = Json::Object();
		variable.Set("type", Json::String(entry.second.type));
		variable.Set("value", entry.second.is_null ? Json::Null() : Json::String(entry.second.text));
		if (!entry.second.shown.IsNull()) {
			variable.Set("shown", entry.second.shown);
		}
		variables.Set(entry.first, std::move(variable));
	}
	return Json::Object().Set("format", Json::Number(CACHE_FORMAT)).Set("variables", std::move(variables)).Dump();
}

bool ParseCacheState(const std::string &text, CacheState &state) {
	if (text.size() > CACHE_MAX_RESULT_BYTES) {
		return false;
	}
	std::string error;
	auto json = ParseJson(text, error);
	if (!error.empty() || !HasFormat(json)) {
		return false;
	}
	auto variables = json.Get("variables");
	if (!variables || variables->GetType() != Json::Type::OBJECT) {
		return false;
	}
	CacheState parsed;
	for (auto &entry : variables->Entries()) {
		auto type = entry.second.GetType() == Json::Type::OBJECT ? StringField(entry.second, "type") : nullptr;
		auto value = type ? entry.second.Get("value") : nullptr;
		if (!type || !value || (!value->IsNull() && value->GetType() != Json::Type::STRING)) {
			return false;
		}
		CacheVariable variable;
		variable.type = *type;
		variable.is_null = value->IsNull();
		variable.text = value->Str();
		if (auto shown = entry.second.Get("shown")) {
			variable.shown = *shown;
		}
		parsed.variables[entry.first] = std::move(variable);
	}
	state = std::move(parsed);
	return true;
}

std::map<std::string, std::string> CacheVariableKeys(const CacheState &state) {
	std::map<std::string, std::string> keys;
	for (auto &entry : state.variables) {
		keys[entry.first] = CacheVariableKey(entry.second.type, entry.second.text, entry.second.is_null);
	}
	return keys;
}

bool IsUnsure(const std::string &text) {
	return !text.empty() && text[0] == '?';
}

std::string StripUnsure(const std::string &text) {
	return IsUnsure(text) ? text.substr(1) : text;
}

std::string MarkUnsure(const std::string &text) {
	return IsUnsure(text) ? text : "?" + text;
}

std::string CacheEntryKey(const CacheKey &key) {
	return key.verified ? key.result : MarkUnsure(key.result);
}

std::vector<CachedCell> MatchCache(const Notebook &notebook, const std::map<std::string, std::string> &variables,
                                   const std::map<std::string, CacheEntry> &entries,
                                   const std::map<std::string, std::string> &files) {
	std::vector<CachedCell> cached;
	if (entries.empty()) {
		return cached;
	}
	auto keys = CacheKeys(notebook, variables, files);
	auto ids = CacheCellIds(notebook);
	for (size_t i = 0; i < notebook.cells.size(); i++) {
		auto &cell = notebook.cells[i];
		auto found = entries.find(ids[i]);
		if (found == entries.end() || !cell.errors.empty()) {
			continue;
		}
		CachedCell match;
		match.index = cell.index;
		// `?` before the key: the result was made from data the key could not vouch for then (CacheEntryKey).
		auto &key = found->second.key;
		match.stale = StripUnsure(key) != keys[i].result;
		match.verified = keys[i].verified && !IsUnsure(key);
		match.entry = &found->second;
		cached.push_back(match);
	}
	return cached;
}

} // namespace notebook
