#include "notebook/graph.hpp"

#include "notebook/chart.hpp"
#include "notebook/yaml.hpp"
#include "notebook/text.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

namespace notebook {

namespace {

//! How a name from `defines` or `reads` is shown in a message.
std::string Describe(const std::string &name) {
	if (!name.empty() && name[0] == '$') {
		return "variable '" + name.substr(1) + "'";
	}
	return "'" + name + "'";
}

std::string CellLabel(const Cell &cell) {
	return cell.name.empty() ? "line " + std::to_string(cell.start_line) : cell.name;
}

//! Tarjan's strongly connected components over `depends_on`.
struct Components {
	const Notebook &notebook;
	std::vector<int> index, low;
	std::vector<bool> on_stack;
	std::vector<int> stack;
	std::vector<std::vector<int>> result;
	int counter = 0;

	explicit Components(const Notebook &notebook)
	    : notebook(notebook), index(notebook.cells.size(), -1), low(notebook.cells.size(), 0),
	      on_stack(notebook.cells.size(), false) {
		for (size_t i = 0; i < notebook.cells.size(); i++) {
			if (index[i] < 0) {
				Visit(static_cast<int>(i));
			}
		}
	}

	void Visit(int v) {
		index[v] = low[v] = counter++;
		stack.push_back(v);
		on_stack[v] = true;
		for (int w : notebook.cells[v].depends_on) {
			if (index[w] < 0) {
				Visit(w);
				low[v] = std::min(low[v], low[w]);
			} else if (on_stack[w]) {
				low[v] = std::min(low[v], index[w]);
			}
		}
		if (low[v] == index[v]) {
			std::vector<int> component;
			int w;
			do {
				w = stack.back();
				stack.pop_back();
				on_stack[w] = false;
				component.push_back(w);
			} while (w != v);
			result.push_back(std::move(component));
		}
	}
};

//! Fills the cell's defines and reads. Returns the reads that only its charts' filters make.
std::set<std::string> CollectReferences(Cell &cell, const ReferenceExtractor &extract) {
	auto analyse = [&](const std::string &sql, const std::string &what) {
		auto refs = extract(sql);
		if (!refs.error.empty()) {
			cell.warnings.push_back("could not analyse " + what + ", so what it reads is unknown: " + refs.error);
		}
		for (auto &name : refs.defines) {
			AddUnique(cell.defines, name);
		}
		for (auto &name : refs.reads) {
			AddUnique(cell.reads, name);
		}
		for (auto &name : refs.table_functions) {
			AddUnique(cell.table_functions, name);
		}
	};
	switch (cell.kind) {
	case CellKind::NOTEBOOK:
		break;
	case CellKind::SQL:
		if (cell.is_query && !cell.name.empty()) {
			cell.defines.push_back(cell.name);
		}
		if (!cell.sql.empty()) {
			analyse(cell.sql, "this cell's SQL");
		}
		break;
	case CellKind::INPUT: {
		if (!cell.name.empty()) {
			cell.defines.push_back("$" + cell.name);
		}
		analyse(cell.body, "this input's default");
		// from=<cell>.<column>: the input reads the cell.
		std::string relation, column;
		if (cell.InputFrom(relation, column) && !relation.empty()) {
			AddUnique(cell.reads, RelationName(relation));
		}
		break;
	}
	case CellKind::MARKDOWN:
		for (auto &expression : cell.expressions) {
			analyse("SELECT (" + expression.sql + "\n)", "{{ " + expression.sql + " }}");
		}
		break;
	case CellKind::CHART: {
		auto from = cell.Option("from");
		if (!from.empty()) {
			AddUnique(cell.reads, RelationName(from));
		}
		break;
	}
	}
	for (auto &spec : cell.charts) {
		auto selection = ChartSelection(spec);
		if (selection.empty()) {
			continue;
		}
		if (std::find(cell.variables.begin(), cell.variables.end(), selection) != cell.variables.end()) {
			cell.errors.push_back("this cell's SQL reads $" + selection +
			                      ", which its own chart sets; read it in another cell");
		}
		AddUnique(cell.defines, "$" + selection);
	}
	// What a cell defines for itself is not a dependency (CREATE TABLE t; INSERT INTO t ...).
	auto &reads = cell.reads;
	reads.erase(std::remove_if(reads.begin(), reads.end(),
	                           [&](const std::string &name) {
		                           return std::find(cell.defines.begin(), cell.defines.end(), name) !=
		                                  cell.defines.end();
	                           }),
	            reads.end());
	// A chart may filter on a selection of its own cell, so these are kept.
	std::set<std::string> chart_only;
	for (auto &spec : cell.charts) {
		for (auto &filter : ChartFilters(spec)) {
			if (std::find(cell.reads.begin(), cell.reads.end(), "$" + filter) == cell.reads.end()) {
				chart_only.insert("$" + filter);
				cell.reads.push_back("$" + filter);
			}
		}
	}
	return chart_only;
}

} // namespace

std::string ChartSelection(const std::string &spec) {
	return ChartSelectionInfo(spec).name;
}

std::vector<std::string> ChartFilters(const std::string &spec) {
	std::string error;
	auto yaml = ParseYaml(spec, error);
	auto filter = yaml.Get("filter");
	return filter ? filter->Strings() : std::vector<std::string>();
}

std::string RelationName(const std::string &dotted) {
	std::vector<std::string> parts;
	size_t start = 0;
	while (true) {
		auto dot = dotted.find('.', start);
		parts.push_back(Lower(dotted.substr(start, dot == std::string::npos ? std::string::npos : dot - start)));
		if (dot == std::string::npos) {
			break;
		}
		start = dot + 1;
	}
	if (parts.size() >= 2 && parts[0] == "temp") {
		parts.erase(parts.begin());
	}
	if (parts.size() >= 2 && parts[parts.size() - 2] == "main") {
		parts.erase(parts.end() - 2);
	}
	std::string name;
	for (auto &part : parts) {
		name += (name.empty() ? "" : ".") + part;
	}
	return name;
}

void AnalyzeNotebook(Notebook &notebook, const ReferenceExtractor &extract) {
	auto &cells = notebook.cells;
	std::vector<std::set<std::string>> chart_only(cells.size());
	for (auto &cell : cells) {
		cell.defines.clear();
		cell.reads.clear();
		cell.depends_on.clear();
		chart_only[cell.index] = CollectReferences(cell, extract);
	}

	// One definition per name.
	std::map<std::string, std::vector<int>> definers;
	for (auto &cell : cells) {
		for (auto &name : cell.defines) {
			definers[name].push_back(cell.index);
		}
	}
	for (auto &entry : definers) {
		if (entry.second.size() < 2) {
			continue;
		}
		std::string lines;
		for (int index : entry.second) {
			lines += (lines.empty() ? "" : ", ") + std::to_string(cells[index].start_line);
		}
		for (int index : entry.second) {
			auto &cell = cells[index];
			// Two cells with the same name are already reported by the parser.
			auto duplicate_name = "duplicate cell name '" + entry.first + "'";
			if (std::find(cell.errors.begin(), cell.errors.end(), duplicate_name) != cell.errors.end()) {
				continue;
			}
			cell.errors.push_back(Describe(entry.first) + " is defined by more than one cell (lines " + lines + ")");
		}
	}

	// Resolve reads to the cells that define them.
	for (auto &cell : cells) {
		std::set<int> depends_on;
		for (auto &name : cell.reads) {
			if (name.empty()) {
				continue;
			}
			// `wh.orders` depends on the cell that defines it, or else on the one that
			// attached `wh` or created schema `wh`.
			auto lookup = name;
			auto found = definers.find(lookup);
			while (found == definers.end() && lookup.find('.') != std::string::npos) {
				lookup = lookup.substr(0, lookup.rfind('.'));
				found = definers.find(lookup);
			}
			if (found == definers.end()) {
				// Relations that no cell defines are catalog objects and need no cell.
				if (name[0] == '$') {
					cell.warnings.push_back(Describe(name) + " is not set by any cell, so it reads as NULL");
				}
				continue;
			}
			int definer = found->second.front();
			if (definer == cell.index) {
				continue;
			}
			// A query cell's chart redraws when a selection it filters on changes; its query does
			// not depend on it, so a selection made further down is neither out of order nor a cycle.
			if (cell.kind == CellKind::SQL && chart_only[cell.index].count(name)) {
				continue;
			}
			depends_on.insert(definer);
			bool ordered = cell.kind == CellKind::SQL || cell.kind == CellKind::INPUT;
			if (ordered && definer > cell.index) {
				cell.errors.push_back(Describe(name) + " is defined further down, at line " +
				                      std::to_string(cells[definer].start_line) +
				                      "; SQL and input cells can only use cells above them");
			}
		}
		cell.depends_on.assign(depends_on.begin(), depends_on.end());
	}

	// Cycles. SQL and input cells only read upwards, so a cycle runs through a chart, or
	// through a forward reference that is already an error.
	Components components(notebook);
	for (auto &component : components.result) {
		if (component.size() < 2) {
			continue;
		}
		std::sort(component.begin(), component.end());
		std::string members;
		for (int index : component) {
			members += (members.empty() ? "" : ", ") + CellLabel(cells[index]);
		}
		for (int index : component) {
			cells[index].errors.push_back("dependency cycle between " + members);
		}
	}
}

std::vector<int> RunOrder(const Notebook &notebook) {
	struct Key {
		int anchor;
		int deferred;
		int index;
		bool operator<(const Key &other) const {
			return std::tie(anchor, deferred, index) < std::tie(other.anchor, other.deferred, other.index);
		}
	};
	std::vector<Key> keys;
	for (auto &cell : notebook.cells) {
		Key key {cell.index, 0, cell.index};
		if (cell.kind == CellKind::MARKDOWN || cell.kind == CellKind::CHART) {
			for (int dependency : cell.depends_on) {
				if (dependency > key.anchor) {
					key.anchor = dependency;
					key.deferred = 1;
				}
			}
		}
		keys.push_back(key);
	}
	std::sort(keys.begin(), keys.end());
	std::vector<int> order;
	for (auto &key : keys) {
		order.push_back(key.index);
	}
	return order;
}

} // namespace notebook
