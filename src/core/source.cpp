#include "notebook/source.hpp"

#include <limits>

namespace notebook {

bool IsStoredSource(const std::string &source) {
	return source.compare(0, 3, "db:") == 0;
}

std::string CheckStoredName(const std::string &name) {
	if (name.find_first_not_of(" \t\r\n") == std::string::npos) {
		return "a notebook needs a name to be stored under: pass name := '...'";
	}
	if (name.find('@') != std::string::npos) {
		return "the name \"" + name +
		       "\" holds '@', which starts a version in db:<name>@<version>: pass name := '...' " + "without one";
	}
	return std::string();
}

namespace {
//! What both version errors say: a version given as a number (version :=) or after the `@` of db:<name>@<version>.
const char *const VERSION_RULE = "a version is a whole number from 1 to 2147483647; leave it out for the newest";
} // namespace

std::string CheckStoredVersion(int64_t version) {
	if (version < 1 || version > std::numeric_limits<int32_t>::max()) {
		return "version " + std::to_string(version) + " is not valid: " + VERSION_RULE;
	}
	return std::string();
}

StoredSource ParseStoredSource(const std::string &source) {
	StoredSource result;
	auto rest = IsStoredSource(source) ? source.substr(3) : source;
	auto at = rest.rfind('@');
	result.name = at == std::string::npos ? rest : rest.substr(0, at);
	if (result.name.empty()) {
		result.error = "\"" + source + "\" names no notebook: write db:<name> or db:<name>@<version>";
		return result;
	}
	if (at == std::string::npos) {
		return result;
	}
	auto digits = rest.substr(at + 1);
	int64_t version = 0;
	bool valid = !digits.empty() && digits.size() <= 10;
	for (auto c : digits) {
		valid = valid && c >= '0' && c <= '9';
		if (valid) {
			version = version * 10 + (c - '0');
		}
	}
	if (!valid || !CheckStoredVersion(version).empty()) {
		result.error = "\"" + source + "\" has version \"" + digits + "\": " + VERSION_RULE;
		return result;
	}
	result.version = static_cast<int32_t>(version);
	return result;
}

} // namespace notebook
