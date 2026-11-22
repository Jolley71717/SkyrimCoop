#pragma once

#include <algorithm>
#include <cctype>
#include <concepts>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "TextEnglish.h"

// Everything the player reads that the mod wrote: the server's refusals as the joining game shows them, and every
// notice, menu page and button to come. Each is a key with an English text compiled in (TextEnglish.h); a language
// file (Data/SKSE/Plugins/SkyrimCoop/lang/<code>.txt, "key = text" lines) replaces the texts it has, and a key it
// lacks shows the English, so a missing or half-done file never blanks a message. Carried over from F4Multiplayer's
// src/Text.h, with Skyrim's folders and language names.
//
// A text holds named placeholders, {player}, {n}, {plugin}: Format fills them in from the arguments. One the
// arguments lack stays in the text as written ("{player}"), so a typo in a translation shows up where it is read
// rather than as a gap. In a file, "\n" is a line break and "\\" a backslash.
//
// This header is free of the game, so the tests run all of it and the server uses it for its refusals' English;
// Text.cpp picks the language (sLanguage in SkyrimCoop.ini, else the game's own) and reads the file.
namespace Text
{
	// One placeholder's value: Arg{ "n", 3 }, Arg{ "player", name }.
	struct Arg
	{
		std::string name;
		std::string value;

		Arg(std::string_view a_name, std::string_view a_value) :
			name(a_name), value(a_value)
		{}
		Arg(std::string_view a_name, const char* a_value) :
			name(a_name), value(a_value ? a_value : "")
		{}
		Arg(std::string_view a_name, const std::string& a_value) :
			name(a_name), value(a_value)
		{}
		template <std::integral T>
		Arg(std::string_view a_name, T a_value) :
			name(a_name), value(std::to_string(a_value))
		{}
	};

	// The compiled-in English: every key there is, with its text (TextEnglish.h).
	[[nodiscard]] inline std::span<const Entry> English()
	{
		return ENGLISH_TABLE;
	}

	using Table = std::map<std::string, std::string, std::less<>>;

	namespace detail
	{
		inline bool IsNameChar(char a_c)
		{
			return std::isalnum(static_cast<unsigned char>(a_c)) || a_c == '_';
		}

		inline std::string_view Trim(std::string_view a_str)
		{
			const auto first = a_str.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			return a_str.substr(first, a_str.find_last_not_of(" \t\r\n") - first + 1);
		}

		// Where the placeholder that opens at a_open ends (the '}'), or npos when the brace opens no name.
		inline std::size_t PlaceholderEnd(std::string_view a_text, std::size_t a_open)
		{
			const auto close = a_text.find('}', a_open + 1);
			if (close != std::string_view::npos && close > a_open + 1 && std::ranges::all_of(a_text.substr(a_open + 1, close - a_open - 1), IsNameChar)) {
				return close;
			}
			return std::string_view::npos;
		}

		struct State
		{
			Table table;                  // the language file's texts, by key
			std::string code = "en";      // the language they are in
		};

		inline State& Current()
		{
			static State state;
			return state;
		}

		// Built once, on first use, by whichever thread asks first: the server's thread (its refusals' English) and the
		// game's (a text) may both be first, and a function's static is filled by one of them while the other waits.
		inline const std::unordered_map<std::string_view, std::string_view>& EnglishMap()
		{
			static const auto map = [] {
				std::unordered_map<std::string_view, std::string_view> built;
				for (const auto& entry : English()) {
					built.emplace(entry.key, entry.text);
				}
				return built;
			}();
			return map;
		}
	}

	// a_template with each {name} replaced by the argument of that name. A {name} with no argument, and a brace
	// that opens no name, stay as they are.
	[[nodiscard]] inline std::string Expand(std::string_view a_template, std::span<const Arg> a_args)
	{
		std::string out;
		out.reserve(a_template.size() + 16);
		for (std::size_t i = 0; i < a_template.size(); ++i) {
			if (a_template[i] == '{') {
				if (const auto close = detail::PlaceholderEnd(a_template, i); close != std::string_view::npos) {
					const auto name = a_template.substr(i + 1, close - i - 1);
					const auto arg = std::ranges::find_if(a_args, [&](const Arg& a_arg) { return a_arg.name == name; });
					if (arg != a_args.end()) {
						out += arg->value;
						i = close;
						continue;
					}
				}
			}
			out += a_template[i];
		}
		return out;
	}

	[[nodiscard]] inline std::string Expand(std::string_view a_template, std::initializer_list<Arg> a_args)
	{
		return Expand(a_template, std::span<const Arg>{ a_args.begin(), a_args.size() });
	}

	// The text of a language file: "key = text" lines, '#' lines and blank lines skipped, "\n" and "\\" unescaped, a
	// leading UTF-8 byte order mark and the lines' CRs ignored. A line with no '=' or no key is skipped.
	[[nodiscard]] inline Table Parse(std::string_view a_file)
	{
		Table table;
		if (a_file.starts_with("\xEF\xBB\xBF")) {
			a_file.remove_prefix(3);
		}
		while (!a_file.empty()) {
			const auto end = a_file.find('\n');
			const auto line = detail::Trim(a_file.substr(0, end));
			a_file = end == std::string_view::npos ? std::string_view{} : a_file.substr(end + 1);
			if (line.empty() || line.front() == '#') {
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			const auto key = detail::Trim(line.substr(0, eq));
			const auto raw = detail::Trim(line.substr(eq + 1));
			if (key.empty()) {
				continue;
			}
			std::string text;
			text.reserve(raw.size());
			for (std::size_t i = 0; i < raw.size(); ++i) {
				if (raw[i] == '\\' && i + 1 < raw.size() && (raw[i + 1] == 'n' || raw[i + 1] == '\\')) {
					text += raw[i + 1] == 'n' ? '\n' : '\\';
					++i;
				} else {
					text += raw[i];
				}
			}
			table[std::string{ key }] = std::move(text);
		}
		return table;
	}

	// The player's own file for a language, beside the log (Documents\My Games\Skyrim Special Edition\SKSE): its texts
	// go over the mod's file for that language (Overlay). It is the place for a player's corrections or a language the
	// mod doesn't ship, because no update replaces it: the files in the lang folder are the mod's, and each version
	// brings its own.
	[[nodiscard]] inline std::string OwnFileName(std::string_view a_code)
	{
		return std::string{ "SkyrimCoop_lang_" }.append(a_code).append(".txt");
	}

	// a_base with a_own's texts over it: a key both have takes a_own's text, the rest of a_base stays.
	inline void Overlay(Table& a_base, Table a_own)
	{
		for (auto& [key, text] : a_own) {
			a_base.insert_or_assign(key, std::move(text));
		}
	}

	// A text as a language file holds it (Parse's inverse), for the tests and for writing a file from the table.
	[[nodiscard]] inline std::string Escape(std::string_view a_text)
	{
		std::string out;
		for (const char c : a_text) {
			if (c == '\n') {
				out += "\\n";
			} else if (c == '\\') {
				out += "\\\\";
			} else {
				out += c;
			}
		}
		return out;
	}

	// The names of a text's placeholders, sorted (a translation must have the English text's).
	[[nodiscard]] inline std::vector<std::string> Placeholders(std::string_view a_text)
	{
		std::vector<std::string> names;
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			if (a_text[i] != '{') {
				continue;
			}
			if (const auto close = detail::PlaceholderEnd(a_text, i); close != std::string_view::npos) {
				names.emplace_back(a_text.substr(i + 1, close - i - 1));
				i = close;
			}
		}
		std::ranges::sort(names);
		return names;
	}

	// A language as the game, Steam or the ini names it, as the code of a language file ("en", "en-gb", "de", "es",
	// "es-mx", "fr", "it", "ja", "pl", "pt-br", "ru", "uk", "zh-cn", "zh-tw"). The game's own sLanguage is the
	// upper-case English name of its strings files: ENGLISH, FRENCH, GERMAN, ITALIAN, SPANISH, POLISH, RUSSIAN,
	// JAPANESE or CHINESE. The game's own Chinese is Traditional, so CHINESE is "zh-tw"; a player on a Simplified
	// Chinese translation sets sLanguage = zh-cn in SkyrimCoop.ini. Steam's own names (latam, brazilian, schinese,
	// tchinese, ukrainian) and the ini's short codes work too; the game has no Brazilian Portuguese, Latin American
	// Spanish or Ukrainian of its own. Case, '-', '_' and spaces do not matter. What this does not know comes back
	// lower-cased with '_' as '-', so a file the player adds (lang/ko.txt) is found by its own name.
	[[nodiscard]] inline std::string CodeFor(std::string_view a_name)
	{
		std::string plain;
		std::string kept;
		for (const char c : detail::Trim(a_name)) {
			const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			kept += lower == '_' ? '-' : lower;
			if (lower != '-' && lower != '_' && lower != ' ') {
				plain += lower;
			}
		}
		static const std::pair<std::string_view, std::string_view> KNOWN[] = {
			{ "en", "en" }, { "english", "en" }, { "enus", "en" }, { "americanenglish", "en" },
			{ "engb", "en-gb" }, { "enuk", "en-gb" }, { "britishenglish", "en-gb" },
			{ "ru", "ru" }, { "russian", "ru" },
			{ "uk", "uk" }, { "ua", "uk" }, { "ukua", "uk" }, { "ukrainian", "uk" },
			{ "es", "es" }, { "spanish", "es" }, { "eses", "es" },
			{ "esmx", "es-mx" }, { "latam", "es-mx" }, { "latinamericanspanish", "es-mx" }, { "mexicanspanish", "es-mx" },
			{ "de", "de" }, { "german", "de" },
			{ "fr", "fr" }, { "french", "fr" },
			{ "it", "it" }, { "italian", "it" },
			{ "pl", "pl" }, { "polish", "pl" },
			{ "ptbr", "pt-br" }, { "pt", "pt-br" }, { "brazilian", "pt-br" }, { "portuguese", "pt-br" },
			{ "ja", "ja" }, { "jp", "ja" }, { "japanese", "ja" },
			{ "cn", "zh-cn" }, { "zhcn", "zh-cn" }, { "zhhans", "zh-cn" }, { "schinese", "zh-cn" }, { "simplifiedchinese", "zh-cn" },
			{ "chinese", "zh-tw" }, { "zh", "zh-tw" }, { "tw", "zh-tw" }, { "zhtw", "zh-tw" }, { "zhhant", "zh-tw" }, { "tchinese", "zh-tw" }, { "traditionalchinese", "zh-tw" },
		};
		for (const auto& [name, code] : KNOWN) {
			if (plain == name) {
				return std::string{ code };
			}
		}
		return kept;
	}

	// Whether a code can name a file in the lang folder (letters, digits and '-', so it never leaves the folder).
	[[nodiscard]] inline bool IsFileCode(std::string_view a_code)
	{
		return !a_code.empty() && a_code.size() <= 16 && std::ranges::all_of(a_code, [](char a_c) { return std::isalnum(static_cast<unsigned char>(a_c)) || a_c == '-'; });
	}

	// The language the texts are in now, and what its file held (replacing the last). The plugin sets it once at
	// startup; the tests set it to try a language.
	inline void Use(std::string a_code, Table a_table)
	{
		auto& state = detail::Current();
		state.code = std::move(a_code);
		state.table = std::move(a_table);
	}

	[[nodiscard]] inline const std::string& Language()
	{
		return detail::Current().code;
	}

	// Picks the language (sLanguage in SkyrimCoop.ini, else the game's own sLanguage, else English) and reads its
	// file from Data/SKSE/Plugins/SkyrimCoop/lang. Once, when the game's data is loaded (Text.cpp, the plugin only).
	void Load();

	// The English text of a key (the key itself for one that does not exist, so a typo shows where it is read).
	[[nodiscard]] inline std::string GetEnglish(std::string_view a_key)
	{
		const auto& english = detail::EnglishMap();
		const auto  it = english.find(a_key);
		return it != english.end() ? std::string{ it->second } : std::string{ a_key };
	}

	// The text of a key in the current language: the file's, else the English.
	[[nodiscard]] inline std::string Get(std::string_view a_key)
	{
		const auto& table = detail::Current().table;
		if (const auto it = table.find(a_key); it != table.end() && !it->second.empty()) {
			return it->second;
		}
		return GetEnglish(a_key);
	}

	[[nodiscard]] inline std::string Format(std::string_view a_key, std::initializer_list<Arg> a_args)
	{
		return Expand(Get(a_key), a_args);
	}

	// The English text with its placeholders filled: what the server sends, and what the log says of a notice,
	// whatever language the player reads.
	[[nodiscard]] inline std::string FormatEnglish(std::string_view a_key, std::initializer_list<Arg> a_args)
	{
		return Expand(GetEnglish(a_key), a_args);
	}

	// The placeholders' values when a_text is a_template filled in, or nothing when it is not. Each placeholder takes
	// what lies up to the next fixed part of the template; the last fixed part must end the text, so a value may hold
	// what follows it elsewhere ("Mod.esp" before a closing ".").
	[[nodiscard]] inline std::optional<std::vector<Arg>> Match(std::string_view a_template, std::string_view a_text)
	{
		std::vector<std::string_view> fixed{ {} };
		std::vector<std::string_view> names;
		std::size_t                   start = 0;
		for (std::size_t i = 0; i < a_template.size(); ++i) {
			if (a_template[i] != '{') {
				continue;
			}
			if (const auto close = detail::PlaceholderEnd(a_template, i); close != std::string_view::npos) {
				fixed.back() = a_template.substr(start, i - start);
				names.push_back(a_template.substr(i + 1, close - i - 1));
				fixed.emplace_back();
				start = close + 1;
				i = close;
			}
		}
		fixed.back() = a_template.substr(start);
		if (names.empty()) {
			return a_text == a_template ? std::optional{ std::vector<Arg>{} } : std::nullopt;
		}
		if (!a_text.starts_with(fixed.front()) || a_text.size() < fixed.front().size() + fixed.back().size() || !a_text.ends_with(fixed.back())) {
			return std::nullopt;
		}
		auto             rest = a_text.substr(fixed.front().size(), a_text.size() - fixed.front().size() - fixed.back().size());
		std::vector<Arg> values;
		for (std::size_t n = 0; n < names.size(); ++n) {
			if (n + 1 == names.size()) {
				values.emplace_back(names[n], rest);
				break;
			}
			const auto next = fixed[n + 1];
			const auto at = next.empty() ? std::string_view::npos : rest.find(next);
			if (at == std::string_view::npos) {
				return std::nullopt;  // two placeholders side by side, or a fixed part missing
			}
			values.emplace_back(names[n], rest.substr(0, at));
			rest.remove_prefix(at + next.size());
		}
		return values;
	}

	// A server's refusal as the joining player reads it. The server's texts are English on the wire (it builds them
	// from the reject.* keys with FormatEnglish, so any version of the server, or a dedicated one, sends the same);
	// the ones this game knows are shown in its language, and any other text (a changed server's) as it came.
	[[nodiscard]] inline std::string Reject(std::string_view a_reason)
	{
		for (const auto& entry : English()) {
			if (!entry.key.starts_with("reject.")) {
				continue;
			}
			if (const auto values = Match(entry.text, a_reason)) {
				return Expand(Get(entry.key), std::span<const Arg>{ *values });
			}
		}
		return std::string{ a_reason };
	}
}
