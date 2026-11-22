#pragma once

// The repository's own files, for the tests that guard them (every language file has every key, no player text is a
// literal in the plugin's code): those rules are on code that only runs in the game, so they are checked where it is
// written. Carried over from F4Multiplayer's tests/SourceTree.h.

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#	include <cstdlib>
#endif

namespace SourceTree
{
	// The repository the tests were built in (they run from its build folder), or else the one they run inside.
	inline std::optional<std::filesystem::path> Root()
	{
		std::vector<std::filesystem::path> starts;
#ifdef _WIN32
		if (char* exe = nullptr; _get_pgmptr(&exe) == 0 && exe && *exe) {
			starts.emplace_back(std::filesystem::path{ exe }.parent_path());
		}
#else
		std::error_code error;
		if (const auto exe = std::filesystem::read_symlink("/proc/self/exe", error); !error) {
			starts.emplace_back(exe.parent_path());
		}
#endif
		starts.emplace_back(std::filesystem::current_path());
		for (auto at : starts) {
			for (;; at = at.parent_path()) {
				std::error_code error;
				if (std::filesystem::exists(at / "src" / "TextEnglish.h", error) && std::filesystem::exists(at / "common" / "Server.cpp", error)) {
					return at;
				}
				if (at == at.parent_path()) {
					break;
				}
			}
		}
		return std::nullopt;
	}

	inline std::string Read(const std::filesystem::path& a_file)
	{
		std::ifstream      file{ a_file, std::ios::binary };
		std::ostringstream text;
		text << file.rdbuf();
		return text.str();
	}

	// a_text without its comments, line breaks kept: what a source guard looks for is code, not a call named in a
	// comment. String literals stay (the guards look for them).
	inline std::string WithoutComments(std::string_view a_text)
	{
		std::string out;
		out.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			const char c = a_text[i];
			if (c == '/' && i + 1 < a_text.size() && a_text[i + 1] == '/') {
				while (i < a_text.size() && a_text[i] != '\n') {
					++i;
				}
				out += '\n';
				continue;
			}
			if (c == '/' && i + 1 < a_text.size() && a_text[i + 1] == '*') {
				for (i += 2; i + 1 < a_text.size() && !(a_text[i] == '*' && a_text[i + 1] == '/'); ++i) {
					out += a_text[i] == '\n' ? "\n" : "";
				}
				++i;
				continue;
			}
			if (c == '"' || c == '\'') {
				const auto start = i;
				for (++i; i < a_text.size() && a_text[i] != c; ++i) {
					i += a_text[i] == '\\';  // what a backslash escapes is skipped
				}
				out.append(a_text.substr(start, i - start + 1));
				continue;
			}
			out += c;
		}
		return out;
	}

	// The .cpp and .h files under a_folder (relative to the root, with '/'), each with its text without comments.
	inline std::vector<std::pair<std::string, std::string>> Code(const std::filesystem::path& a_root, std::string_view a_folder)
	{
		std::vector<std::pair<std::string, std::string>> files;
		std::error_code                                  error;
		for (const auto& entry : std::filesystem::recursive_directory_iterator{ a_root / a_folder, error }) {
			const auto ext = entry.path().extension();
			if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h")) {
				continue;
			}
			files.emplace_back(std::filesystem::relative(entry.path(), a_root).generic_string(), WithoutComments(Read(entry.path())));
		}
		return files;
	}
}
