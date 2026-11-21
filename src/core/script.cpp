#include "notebook/script.hpp"

#include "notebook/sql_lexer.hpp"

#include <cctype>
#include <cmath>
#include <set>

namespace notebook {

namespace {

std::string WithSemicolon(const std::string &sql) {
	return sql.empty() ? sql : sql + ";\n";
}

} // namespace

std::string QuoteIdentifier(const std::string &name) {
	std::string quoted = "\"";
	for (char c : name) {
		quoted += c;
		if (c == '"') {
			quoted += '"';
		}
	}
	return quoted + "\"";
}

std::string QuoteString(const std::string &text) {
	std::string quoted = "'";
	for (char c : text) {
		quoted += c;
		if (c == '\'') {
			quoted += '\'';
		}
	}
	return quoted + "'";
}

namespace {

//! Splits `STRUCT(a DATE, b VARCHAR)` into field types, and `DATE[]` into its element type.
std::string ElementType(const std::string &type) {
	if (type.size() > 2 && type.compare(type.size() - 2, 2, "[]") == 0) {
		return type.substr(0, type.size() - 2);
	}
	return type;
}

std::string FieldType(const std::string &type, const std::string &field) {
	auto open = type.find('(');
	if (type.compare(0, 6, "STRUCT") != 0 || open == std::string::npos) {
		return type;
	}
	int depth = 0;
	size_t start = open + 1;
	for (size_t i = open + 1; i < type.size(); i++) {
		char c = type[i];
		if (c == '(') {
			depth++;
		} else if ((c == ',' || c == ')') && depth == 0) {
			auto part = type.substr(start, i - start);
			while (!part.empty() && part[0] == ' ') {
				part.erase(0, 1);
			}
			auto space = part.find(' ');
			if (space != std::string::npos) {
				auto name = part.substr(0, space);
				if (name.size() >= 2 && name.front() == '"') {
					name = name.substr(1, name.size() - 2);
				}
				if (name == field) {
					return part.substr(space + 1);
				}
			}
			start = i + 1;
		} else if (c == ')') {
			depth--;
		}
	}
	return std::string();
}

} // namespace

std::string JsonToSqlLiteral(const Json &value, const std::string &type) {
	switch (value.GetType()) {
	case Json::Type::NUL:
		return type.empty() ? "NULL" : "NULL::" + type;
	case Json::Type::BOOL:
		return value.Truthy() ? "true" : "false";
	case Json::Type::NUMBER: {
		if (!std::isfinite(value.Num())) {
			// Not a SQL number literal: DuckDB reads 'Infinity', '-Infinity' and 'NaN' as doubles.
			return QuoteString(FormatNumber(value.Num())) + "::" + (type.empty() ? "DOUBLE" : type);
		}
		auto literal = value.Dump();
		return type.empty() ? literal : literal + "::" + type;
	}
	case Json::Type::STRING: {
		auto literal = QuoteString(value.Str());
		return type.empty() ? literal : literal + "::" + type;
	}
	case Json::Type::RAW: {
		// JSON text kept as it was serialised: the literal of the value it holds.
		std::string error;
		auto parsed = ParseJson(value.Dump(), error);
		return error.empty() ? JsonToSqlLiteral(parsed, type) : "NULL";
	}
	case Json::Type::ARRAY: {
		std::string out = "[";
		auto element = ElementType(type);
		for (size_t i = 0; i < value.Items().size(); i++) {
			out += (i ? ", " : "") + JsonToSqlLiteral(value.Items()[i], element);
		}
		return out + "]";
	}
	case Json::Type::OBJECT: {
		std::string out = "{";
		bool first = true;
		for (auto &entry : value.Entries()) {
			out += (first ? "" : ", ") + QuoteString(entry.first) + ": " +
			       JsonToSqlLiteral(entry.second, FieldType(type, entry.first));
			first = false;
		}
		return out + "}";
	}
	}
	return "NULL";
}

std::string StoredTable(const std::string &store, const std::string &name) {
	return QuoteIdentifier(store) + ".main." + QuoteIdentifier(name);
}

namespace {

bool Materialized(const Cell &cell) {
	return cell.kind == CellKind::SQL && cell.is_query && !cell.name.empty() &&
	       cell.Option("materialize", "view") == "table";
}

//! The first words of a statement, upper-cased, skipping comments before and between them: enough to tell what it
//! creates.
std::vector<std::string> LeadingWords(const std::string &statement, size_t count) {
	std::vector<std::string> words;
	auto text = StripLeadingComments(statement);
	size_t i = 0;
	while (words.size() < count && i < text.size()) {
		// Comments may come between words too: `CREATE /* x */ VIEW`.
		text = StripLeadingComments(text.substr(i));
		i = 0;
		std::string word;
		while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) {
			word += static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
			i++;
		}
		if (word.empty()) {
			break;
		}
		words.push_back(word);
	}
	return words;
}

} // namespace

bool IsCreateTempTable(const std::string &statement) {
	if (!IsConnectionState(statement)) {
		return false;
	}
	auto words = LeadingWords(statement, 6);
	if (words.empty() || words[0] != "CREATE") {
		return false;
	}
	for (auto &word : words) {
		if (word == "TABLE") {
			return true;
		}
		// What follows AS is the query: CREATE TEMP VIEW v AS TABLE t makes a view.
		if (word == "AS" || word == "VIEW") {
			return false;
		}
	}
	return false;
}

namespace {

//! The first `count` words of a statement after its leading comments, upper-cased, a double-quoted identifier
//! unquoted: `SET "file_search_path" = 'x'` gives SET, FILE_SEARCH_PATH.
std::vector<std::string> LeadingNames(const std::string &statement, size_t count) {
	std::vector<std::string> words;
	auto text = StripLeadingComments(statement);
	size_t i = 0;
	while (words.size() < count && i < text.size()) {
		// Comments may come between words too: `SET /* x */ file_search_path`.
		text = StripLeadingComments(text.substr(i));
		i = 0;
		std::string word;
		if (i < text.size() && text[i] == '"') {
			for (i++; i < text.size(); i++) {
				if (text[i] == '"' && i + 1 < text.size() && text[i + 1] == '"') {
					word += '"';
					i++;
				} else if (text[i] == '"') {
					i++;
					break;
				} else {
					word += static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
				}
			}
		} else {
			while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) {
				word += static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
				i++;
			}
		}
		if (word.empty()) {
			break;
		}
		words.push_back(word);
	}
	return words;
}

} // namespace

bool SetsSetting(const Notebook &notebook, const std::string &name) {
	std::string wanted;
	for (char c : name) {
		wanted += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	for (auto &cell : notebook.cells) {
		if (cell.kind != CellKind::SQL) {
			continue;
		}
		for (auto &statement : SplitStatements(cell.sql)) {
			// SET and RESET in any scope, and PRAGMA name = value, which DuckDB still takes for a SET.
			auto words = LeadingNames(statement.text, 3);
			if (words.empty() || (words[0] != "SET" && words[0] != "RESET" && words[0] != "PRAGMA")) {
				continue;
			}
			size_t at = 1;
			if (words[0] != "PRAGMA" && at < words.size() &&
			    (words[at] == "SESSION" || words[at] == "LOCAL" || words[at] == "GLOBAL")) {
				at++;
			}
			if (at < words.size() && words[at] == wanted) {
				return true;
			}
		}
	}
	return false;
}

bool IsCreateView(const std::string &statement) {
	// CREATE [OR REPLACE] [TEMP | TEMPORARY] [SECURE] [RECURSIVE] VIEW
	auto words = LeadingWords(statement, 7);
	if (words.empty() || words[0] != "CREATE") {
		return false;
	}
	for (size_t i = 1; i < words.size(); i++) {
		if (words[i] == "VIEW") {
			return true;
		}
		if (words[i] != "OR" && words[i] != "REPLACE" && words[i] != "TEMP" && words[i] != "TEMPORARY" &&
		    words[i] != "SECURE" && words[i] != "RECURSIVE") {
			return false;
		}
	}
	return false;
}

bool MayWrite(const Cell &cell) {
	if (cell.kind != CellKind::SQL) {
		return false;
	}
	for (auto &statement : SplitStatements(cell.sql)) {
		std::string variable;
		if (IsQueryStatement(statement.text) || IsSetVariable(statement.text, variable)) {
			continue;
		}
		auto words = LeadingWords(statement.text, 1);
		if (!words.empty() && (words[0] == "SET" || words[0] == "RESET" || words[0] == "USE")) {
			continue;
		}
		return true;
	}
	return false;
}

std::string CellSql(const Cell &cell, const std::string &store) {
	if (cell.kind == CellKind::INPUT) {
		std::string result;
		for (auto &statement : SplitStatements(cell.sql)) {
			result += WithSemicolon(statement.text);
		}
		return result;
	}
	if (cell.kind != CellKind::SQL) {
		return std::string();
	}
	auto statements = SplitStatements(cell.sql);
	if (statements.empty()) {
		return std::string();
	}
	std::string result;
	bool wrap = cell.is_query && !cell.name.empty();
	for (size_t i = 0; i + (wrap ? 1 : 0) < statements.size(); i++) {
		result += WithSemicolon(statements[i].text);
	}
	if (wrap) {
		// Comments written above the query stay above the CREATE, where they still read naturally.
		const auto &last = statements.back().text;
		auto query = StripLeadingComments(last);
		result += last.substr(0, last.size() - query.size());
		if (Materialized(cell) && !store.empty()) {
			// The table lives in the session's database; the name the other cells read is a view of it.
			result += "CREATE OR REPLACE TABLE " + StoredTable(store, cell.name) + " AS\n" + WithSemicolon(query);
			result += "CREATE OR REPLACE TEMP VIEW " + QuoteIdentifier(cell.name) + " AS SELECT * FROM " +
			          StoredTable(store, cell.name) + ";\n";
			return result;
		}
		const char *object = Materialized(cell) ? "TABLE" : "VIEW";
		result += std::string("CREATE OR REPLACE TEMP ") + object + " " + QuoteIdentifier(cell.name) + " AS\n";
		result += WithSemicolon(query);
	}
	return result;
}

std::string ReplaySql(const Cell &cell, const std::string &store) {
	if (cell.kind != CellKind::SQL) {
		return std::string();
	}
	// A named query cell's last statement becomes its view, replayed in CellSql's wrapped form.
	bool defines_view = cell.is_query && !cell.name.empty();
	auto statements = SplitStatements(cell.sql);
	std::string replay;
	for (size_t i = 0; i < statements.size() && !(defines_view && i + 1 == statements.size()); i++) {
		// With a store, temp tables come back from the copies kept there, rows and all.
		if (IsConnectionState(statements[i].text) && (store.empty() || !IsCreateTempTable(statements[i].text))) {
			replay += statements[i].text + ";\n";
		}
	}
	if (defines_view && Materialized(cell) && !store.empty()) {
		// Reads the table the run stored instead of computing it again.
		replay += "CREATE OR REPLACE TEMP VIEW " + QuoteIdentifier(cell.name) + " AS SELECT * FROM " +
		          StoredTable(store, cell.name) + ";\n";
	} else if (defines_view) {
		auto wrapped = SplitStatements(CellSql(cell));
		replay += wrapped.empty() ? std::string() : wrapped.back().text + ";\n";
	}
	return replay;
}

std::string DisplaySql(const Cell &cell) {
	if (cell.kind != CellKind::SQL || !cell.is_query) {
		return std::string();
	}
	if (!cell.name.empty()) {
		return "SELECT * FROM " + QuoteIdentifier(cell.name);
	}
	auto statements = SplitStatements(cell.sql);
	return statements.empty() ? std::string() : statements.back().text;
}

std::string ChartFrom(const Cell &cell) {
	if (cell.kind == CellKind::CHART) {
		return cell.Option("from");
	}
	if (cell.kind != CellKind::SQL || !cell.is_query) {
		return std::string();
	}
	if (!cell.name.empty()) {
		return cell.name;
	}
	auto statements = SplitStatements(cell.sql);
	// On a line of its own (a statement that ends in a -- comment already ends its line), for the parenthesis.
	auto &text = statements.back().text;
	return "(\n" + text + (EndsInLineComment(text) || text.back() == '\n' ? ")" : "\n)");
}

std::vector<std::string> UndefinedVariables(const Notebook &notebook) {
	std::set<std::string> defined;
	for (auto &cell : notebook.cells) {
		if (cell.kind == CellKind::INPUT) {
			defined.insert(cell.name);
		} else if (cell.kind == CellKind::SQL) {
			for (auto &statement : SplitStatements(cell.sql)) {
				std::string name;
				if (IsSetVariable(statement.text, name)) {
					defined.insert(name);
				}
			}
		}
	}
	std::vector<std::string> undefined;
	std::set<std::string> seen;
	for (auto &cell : notebook.cells) {
		for (auto &name : cell.variables) {
			if (!defined.count(name) && seen.insert(name).second) {
				undefined.push_back(name);
			}
		}
	}
	return undefined;
}

std::string ParameterError(const Notebook &notebook, const std::string &name) {
	for (auto &cell : notebook.cells) {
		if (cell.kind == CellKind::INPUT && cell.name == name) {
			return std::string();
		}
	}
	for (auto &undefined : UndefinedVariables(notebook)) {
		if (undefined == name) {
			return std::string();
		}
	}
	for (auto &cell : notebook.cells) {
		if (cell.kind != CellKind::SQL) {
			continue;
		}
		for (auto &statement : SplitStatements(cell.sql)) {
			std::string set;
			if (IsSetVariable(statement.text, set) && set == name) {
				return "variable '" + name + "' is set by the SQL cell at line " + std::to_string(cell.start_line) +
				       ", so it cannot be a parameter; make it an input cell";
			}
		}
	}
	return "the notebook has no input or variable named '" + name + "'";
}

std::string ExpandNotebook(const Notebook &notebook, const std::string &source_name) {
	std::string script = "-- Expanded from " + source_name + ". Runs in any DuckDB 2.0+ client; no extension needed.\n";
	script += "-- Generated by notebook_sql(); edit the notebook, not this file.\n";
	auto undefined = UndefinedVariables(notebook);
	if (!undefined.empty()) {
		script += "\n-- Variables read by cells but set only interactively start as NULL.\n";
		for (auto &name : undefined) {
			script += "SET VARIABLE " + QuoteIdentifier(name) + " = NULL;\n";
		}
	}
	for (auto &cell : notebook.cells) {
		auto sql = CellSql(cell);
		if (sql.empty()) {
			continue;
		}
		script += "\n-- " + std::string(CellKindName(cell.kind)) + " cell";
		if (!cell.name.empty()) {
			script += " " + cell.name;
		}
		script += " (line " + std::to_string(cell.start_line) + ")\n" + sql;
	}
	return script;
}

std::vector<double> InputSteps(double lo, double hi, double step, size_t limit) {
	std::vector<double> values;
	if (!std::isfinite(lo) || !std::isfinite(hi) || !std::isfinite(step) || step <= 0 || hi < lo) {
		return values;
	}
	// A thousandth of a step for rounding: 0 to 1 in steps of 0.1 has 11 values.
	double count = std::floor((hi - lo) / step + 1.0 / 1000) + 1;
	if (!(count >= 1) || count > static_cast<double>(limit)) {
		return values;
	}
	for (size_t i = 0; i < static_cast<size_t>(count); i++) {
		auto value = lo + static_cast<double>(i) * step;
		if (!values.empty() && !(value > values.back())) {
			return std::vector<double>();
		}
		values.push_back(value);
	}
	return values;
}

} // namespace notebook
