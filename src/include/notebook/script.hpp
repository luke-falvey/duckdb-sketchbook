#pragma once

#include "notebook/format.hpp"
#include "notebook/json.hpp"

#include <string>
#include <vector>

namespace notebook {

//! Wraps a name in double quotes, doubling any quote inside it.
std::string QuoteIdentifier(const std::string &name);

//! The SQL that running a cell means. Empty for cells that only display something.
//!  - named query cell: its leading statements, then the last one wrapped as
//!    CREATE OR REPLACE TEMP VIEW <name> (or TEMP TABLE with materialize=table)
//!  - input cell: its SET VARIABLE statement
//!  - anything else: the body unchanged
//! With `store` (the database a long-lived session keeps its tables in, see Runner), a
//! materialize=table cell creates its table there and a temp view of it under its own name.
std::string CellSql(const Cell &cell, const std::string &store = std::string());

//! What a fresh connection must run to have what a SQL cell left in the last one: its statements that
//! set connection state (SET, USE, CREATE TEMP ..., notebook::IsConnectionState) and, for a named
//! query cell, the view or temp table it defines. Each statement ends in `;\n`. Empty for other cells.
//! With `store`, a materialize=table cell's replay is a view of the table kept there (nothing is
//! computed again), and CREATE TEMP TABLE statements are left out: the session restores those tables
//! from copies in the store, with the rows later statements put in them.
std::string ReplaySql(const Cell &cell, const std::string &store = std::string());

//! `"store".main."name"`: where a session's store keeps the table `name`.
std::string StoredTable(const std::string &store, const std::string &name);

//! Whether a statement is CREATE [OR REPLACE] TEMP[ORARY] TABLE.
bool IsCreateTempTable(const std::string &statement);

//! Whether a statement is CREATE [OR REPLACE] [TEMP[ORARY]] VIEW.
bool IsCreateView(const std::string &statement);

//! Whether a SQL cell of the notebook SETs or RESETs the setting `name` (any scope), such as `file_search_path`.
bool SetsSetting(const Notebook &notebook, const std::string &name);

//! Whether a SQL cell has a statement that may change tables: anything but queries, SET, RESET, USE
//! and SET VARIABLE.
bool MayWrite(const Cell &cell);

//! The SQL whose rows a query cell displays: `SELECT * FROM <name>` for a named one, its last
//! statement for an unnamed one. Empty for other cells.
std::string DisplaySql(const Cell &cell);

//! The relation a cell's charts read, as a chart `from`: a chart cell's from=, a named query
//! cell's name, or an unnamed query cell's query in parentheses. Empty for other cells.
std::string ChartFrom(const Cell &cell);

//! Variables that cells read with `$name` but that no input cell or SET VARIABLE
//! in the notebook defines (chart selections, typically). `$name` on an unset
//! variable is an error in DuckDB, so the runner starts these as NULL.
std::vector<std::string> UndefinedVariables(const Notebook &notebook);

//! Empty if `name` can be given as a parameter to a run, otherwise why not. Parameters
//! override input defaults and set variables that nothing in the notebook sets (chart
//! selections); a variable that an SQL cell sets would be overwritten, so it is refused.
std::string ParameterError(const Notebook &notebook, const std::string &name);

//! A SQL literal for a JSON value sent by the browser, cast to `type` when given. Arrays become
//! lists and objects become structs, each element cast to the matching part of a LIST or STRUCT
//! type: `{"lo": "2026-01-01", "hi": "2026-02-01"}` with `STRUCT(lo DATE, hi DATE)` or with `DATE`
//! (applied to each field).
std::string JsonToSqlLiteral(const Json &value, const std::string &type = std::string());

//! A string as a SQL string literal.
std::string QuoteString(const std::string &text);

//! A plain SQL script equivalent to running every cell in file order. It needs no
//! extension: this is the `sql` export described in docs/FORMAT.md section 7.
std::string ExpandNotebook(const Notebook &notebook, const std::string &source_name);

//! The values a slider or number input from `lo` to `hi` in steps of `step` can take, for a precomputed export: at
//! most `limit` of them, all different. Empty when there would be more, or none, or they cannot be told apart (a
//! step too small to change a large value), or an argument is not a finite number.
std::vector<double> InputSteps(double lo, double hi, double step, size_t limit);

} // namespace notebook
