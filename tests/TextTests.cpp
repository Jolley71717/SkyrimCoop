#include "Test.h"

#include "SourceTree.h"
#include "Text.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The mod's own texts: the placeholder formatter, the language files and the guards that keep a key from missing in
// any language and a new message from being written as a bare English literal. Carried over from F4Multiplayer's
// tests/TextTests.cpp.

namespace
{
	// Leaves the language as the plugin has it by default (English, nothing loaded) however a test ends.
	struct English
	{
		English() { Text::Use("en", {}); }
		~English() { Text::Use("en", {}); }
	};

	// Every language the mod ships, each a file in dist/lang.
	constexpr std::string_view LANGUAGES[] = { "en", "en-gb", "de", "es", "es-mx", "fr", "it", "ja", "pl", "pt-br", "ru", "uk", "zh-cn", "zh-tw" };

	std::vector<std::filesystem::path> LanguageFiles(const std::filesystem::path& a_root)
	{
		std::vector<std::filesystem::path> files;
		std::error_code                    error;
		for (const auto& entry : std::filesystem::directory_iterator{ a_root / "dist" / "lang", error }) {
			if (entry.is_regular_file() && entry.path().extension() == ".txt") {
				files.push_back(entry.path());
			}
		}
		std::ranges::sort(files);
		return files;
	}

	// Where the argument a call's text is in starts (after its spaces), the call opening at a_open ('('): the
	// a_index-th argument, counted from 0, or npos when the call has fewer. Commas inside brackets and string
	// literals don't count.
	std::size_t ArgumentAt(std::string_view a_code, std::size_t a_open, int a_index)
	{
		int  depth = 0;
		int  index = 0;
		auto start = a_open + 1;
		for (auto i = a_open + 1; i < a_code.size(); ++i) {
			const char c = a_code[i];
			if (c == '"' || c == '\'') {
				for (++i; i < a_code.size() && a_code[i] != c; ++i) {
					i += a_code[i] == '\\';
				}
			} else if (c == '(' || c == '{' || c == '[') {
				++depth;
			} else if (c == ')' || c == '}' || c == ']') {
				if (depth-- == 0) {
					break;
				}
			} else if (c == ',' && depth == 0) {
				if (index == a_index) {
					break;
				}
				++index;
				start = i + 1;
			}
		}
		if (index != a_index) {
			return std::string_view::npos;
		}
		return a_code.find_first_not_of(" \t\r\n", start);
	}

	// Where the argument that starts at a_at ends: at the ',' or ')' that closes it, or at the statement's ';'.
	std::size_t ArgumentEnd(std::string_view a_code, std::size_t a_at)
	{
		int depth = 0;
		for (auto i = a_at; i < a_code.size(); ++i) {
			const char c = a_code[i];
			if (c == '"' || c == '\'') {
				for (++i; i < a_code.size() && a_code[i] != c; ++i) {
					i += a_code[i] == '\\';  // what a backslash escapes is skipped
				}
			} else if (c == '(' || c == '{' || c == '[') {
				++depth;
			} else if ((c == ')' || c == '}' || c == ']') && depth > 0) {
				--depth;
			} else if (c == ';' || (depth == 0 && (c == ',' || c == ')'))) {
				return i;
			}
		}
		return a_code.size();
	}

	// Whether the argument that starts at a_at picks between literals: cond ? "a" : "b". A Text call's key
	// ("Text::Get(\"key\")") is no such pick: its ':' is part of a "::".
	bool PicksLiteral(std::string_view a_code, std::size_t a_at)
	{
		const auto end = ArgumentEnd(a_code, a_at);
		for (auto i = a_at; i < end; ++i) {
			const bool colon = a_code[i] == ':' && (i + 1 >= end || a_code[i + 1] != ':') && (i == 0 || a_code[i - 1] != ':');
			if (a_code[i] != '?' && !colon) {
				continue;
			}
			const auto next = a_code.find_first_not_of(" \t\r\n", i + 1);
			if (next < end && (a_code[next] == '"' || a_code.substr(next).starts_with("std::format(\""))) {
				return true;
			}
		}
		return false;
	}

	// Whether the text an argument gives starts as an English literal: "text", or std::format("text", ...), or picks
	// between literals.
	bool IsLiteral(std::string_view a_code, std::size_t a_at)
	{
		if (a_at == std::string_view::npos) {
			return false;
		}
		auto rest = a_code.substr(a_at);
		if (rest.starts_with("std::format(")) {
			rest.remove_prefix(12);
			rest.remove_prefix(std::min(rest.size(), rest.find_first_not_of(" \t\r\n")));
		}
		return rest.starts_with('"') || rest.starts_with("u8\"") || rest.starts_with("L\"") || PicksLiteral(a_code, a_at);
	}
}

TEST("text: placeholders are filled by name, and a missing one stays visible")
{
	CHECK(Text::Expand("{player} found {item}", { { "item", "a sweetroll" }, { "player", "Lydia" } }) == "Lydia found a sweetroll");
	CHECK(Text::Expand("{n} and {n} again", { { "n", 3 } }) == "3 and 3 again");
	CHECK(Text::Expand("Hello {player}", { { "other", "x" } }) == "Hello {player}");
	CHECK(Text::Expand("Hello {player}", {}) == "Hello {player}");
	CHECK(Text::Expand("a { b } c {} d {", { { "b", "x" } }) == "a { b } c {} d {");
	CHECK(Text::Expand("{n}{n}", { { "n", std::size_t{ 12 } } }) == "1212");
	CHECK(Text::Expand("{a}", { { "a", "{b}" }, { "b", "x" } }) == "{b}");  // what an argument holds is not read again
}

TEST("text: a language file is key = text lines, with a byte order mark, CRs and spaces ignored")
{
	const auto table = Text::Parse("\xEF\xBB\xBF# a comment\r\n\r\nreject.password = Falsches Passwort.\r\n  reject.full=Voll  \r\nk = eins\\nzwei \\\\ drei\r\nno equals sign\r\n= no key\r\n# reject.mods = ignored\r\nx = a = b # c");
	CHECK(table.size() == 4u);
	CHECK(table.at("reject.password") == "Falsches Passwort.");
	CHECK(table.at("reject.full") == "Voll");
	CHECK(table.at("k") == "eins\nzwei \\ drei");
	CHECK(table.at("x") == "a = b # c");
	CHECK(!table.contains("reject.mods"));
	CHECK(Text::Parse("").empty());
	CHECK(Text::Parse("a =\n").at("a").empty());
	CHECK(Text::Parse("k = " + Text::Escape("one\ntwo\\n three")).at("k") == "one\ntwo\\n three");
	// Only a mark at the very start is one.
	CHECK(!Text::Parse("a = 1\n\xEF\xBB\xBF" "b = 2\n").contains("b"));
}

TEST("text: a key the language file lacks, or leaves empty, shows the English")
{
	English english;
	Text::Use("de", Text::Parse("reject.password = Falsches Passwort.\nreject.full =\n"));
	CHECK(Text::Get("reject.password") == "Falsches Passwort.");
	CHECK(Text::Get("reject.full") == Text::GetEnglish("reject.full"));        // an empty text is one left to translate
	CHECK(Text::Get("steam.status_playing") == "Playing Skyrim co-op");  // not in the file at all
	CHECK(Text::Language() == "de");
	CHECK(Text::Get("no.such.key") == "no.such.key");  // a key nobody has shows as itself, where it is read
	CHECK(Text::FormatEnglish("reject.full", { { "n", 4 } }) == "The game is full (4 players).");
	// A translation that dropped or misspelled a placeholder shows the fault instead of a gap.
	Text::Use("de", Text::Parse("reject.full = Voll ({spieler})\n"));
	CHECK(Text::Format("reject.full", { { "n", 4 } }) == "Voll ({spieler})");
}

TEST("text: the game's, Steam's and the ini's language names map to the language files")
{
	// The game's own sLanguage values.
	CHECK(Text::CodeFor("ENGLISH") == "en");
	CHECK(Text::CodeFor("FRENCH") == "fr");
	CHECK(Text::CodeFor("GERMAN") == "de");
	CHECK(Text::CodeFor("ITALIAN") == "it");
	CHECK(Text::CodeFor("SPANISH") == "es");
	CHECK(Text::CodeFor("POLISH") == "pl");
	CHECK(Text::CodeFor("RUSSIAN") == "ru");
	CHECK(Text::CodeFor("JAPANESE") == "ja");
	CHECK(Text::CodeFor("CHINESE") == "zh-tw");  // the game's own Chinese is Traditional
	// Steam's names and short codes, for the languages the game lacks too.
	CHECK(Text::CodeFor("latam") == "es-mx");
	CHECK(Text::CodeFor("brazilian") == "pt-br");
	CHECK(Text::CodeFor("schinese") == "zh-cn");
	CHECK(Text::CodeFor("tchinese") == "zh-tw");
	CHECK(Text::CodeFor("ukrainian") == "uk");
	CHECK(Text::CodeFor("ua") == "uk");
	CHECK(Text::CodeFor("pt-BR") == "pt-br");
	CHECK(Text::CodeFor("es_MX") == "es-mx");
	CHECK(Text::CodeFor("en_GB") == "en-gb");
	CHECK(Text::CodeFor("  Russian ") == "ru");
	CHECK(Text::CodeFor("ko") == "ko");  // unknown: its own name, for a file the player adds
	CHECK(Text::IsFileCode("zh-tw"));
	CHECK(!Text::IsFileCode(""));
	CHECK(!Text::IsFileCode("..\\..\\x"));
	CHECK(!Text::IsFileCode("a/b"));
	CHECK(!Text::IsFileCode("two words"));
}

TEST("text: every English key is unique and plain")
{
	std::set<std::string_view> keys;
	for (const auto& entry : Text::English()) {
		CHECK(keys.insert(entry.key).second);
		CHECK(!entry.text.empty());
		CHECK(std::ranges::all_of(entry.key, [](char a_c) { return (a_c >= 'a' && a_c <= 'z') || (a_c >= '0' && a_c <= '9') || a_c == '_' || a_c == '.'; }));
	}
	CHECK(keys.size() >= 8u);
}

TEST("text: a server's refusals read in the player's language, and a changed server's own text is shown as it came")
{
	English english;
	const std::vector<std::string> sent = {
		Text::GetEnglish("reject.password"),
		Text::FormatEnglish("reject.full", { { "n", 4 } }),
		Text::FormatEnglish("reject.version", { { "you", 2 }, { "host", 3 } }),
		Text::GetEnglish("reject.mods"),
		"Some refusal a changed server made up",
	};
	// The wire's texts stay what they were before the keys (the tests of the server check them byte for byte).
	CHECK(sent[0] == "Wrong password.");
	CHECK(sent[2] == "This game runs another version of the mod (protocol 2, the host has 3). Both players need the same version.");
	CHECK(sent[3] == "Your mods differ from the host's.");
	// In English nothing changes.
	for (const auto& reason : sent) {
		CHECK(Text::Reject(reason) == reason);
	}
	Text::Use("de", Text::Parse(
		"reject.password = Falsches Passwort.\n"
		"reject.version = Andere Version: du {you}, der Host {host}.\n"
		"reject.mods = Andere Mods.\n"));
	CHECK(Text::Reject(sent[0]) == "Falsches Passwort.");
	CHECK(Text::Reject(sent[1]) == sent[1]);  // no German text: the English
	CHECK(Text::Reject(sent[2]) == "Andere Version: du 2, der Host 3.");
	CHECK(Text::Reject(sent[3]) == "Andere Mods.");
	CHECK(Text::Reject(sent[4]) == sent[4]);
	CHECK(Text::Reject("") == "");
}

TEST("text: every language file holds every key and only the mod's, with the placeholders they have in English")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);  // run the tests from the repository or its build folder
	const auto files = LanguageFiles(*root);
	REQUIRE(!files.empty());

	std::map<std::string, std::vector<std::string>> placeholders;
	for (const auto& entry : Text::English()) {
		placeholders[std::string{ entry.key }] = Text::Placeholders(entry.text);
	}

	std::vector<std::string> codes;
	for (const auto& file : files) {
		codes.push_back(file.stem().string());
	}
	for (const auto wanted : LANGUAGES) {
		if (std::ranges::find(codes, wanted) == codes.end()) {
			Test::Fail(__FILE__, __LINE__, std::format("dist/lang has no {}.txt", wanted));
		}
	}
	// What the game calls its languages finds one of those files.
	for (const std::string_view game : { "ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH", "POLISH", "RUSSIAN", "JAPANESE", "CHINESE" }) {
		CHECK(std::ranges::find(codes, Text::CodeFor(game)) != codes.end());
	}

	for (const auto& file : files) {
		const auto name = file.filename().string();
		const auto text = SourceTree::Read(file);
		CHECK(Text::IsFileCode(file.stem().string()));
		CHECK(!text.starts_with("\xEF\xBB\xBF"));  // plain UTF-8, no byte order mark
		const auto table = Text::Parse(text);
		for (const auto& [key, translated] : table) {
			const auto expected = placeholders.find(key);
			if (expected == placeholders.end()) {
				Test::Fail(__FILE__, __LINE__, std::format("{}: key '{}' is not one the mod uses", name, key));
				continue;
			}
			if (Text::Placeholders(translated) != expected->second) {
				Test::Fail(__FILE__, __LINE__, std::format("{}: '{}' has different placeholders than the English ({})", name, key, translated));
			}
			if (translated.empty()) {
				Test::Fail(__FILE__, __LINE__, std::format("{}: '{}' is empty", name, key));
			}
		}
		// Whole files: a missing key would show in English in the middle of a translated game.
		for (const auto& [key, ignored] : placeholders) {
			if (!table.contains(key)) {
				Test::Fail(__FILE__, __LINE__, std::format("{}: lacks the key '{}'", name, key));
			}
		}
		if (file.stem() != "en" && file.stem() != "en-gb") {
			// It says how it was made, for the players who correct it.
			const bool machine = text.find("# machine-translated first pass, corrections welcome") != std::string::npos;
			const bool reviewed = text.find("# reviewed against the game's") != std::string::npos || text.find("# Reviewed against the game's") != std::string::npos;
			CHECK(machine || reviewed);
			// And it is translated: a text with words that is the English as it stands was copied, not translated.
			for (const auto& [key, translated] : table) {
				const auto english = Text::GetEnglish(key);
				// Its own words, without its placeholders: a text that is only placeholders and marks ("{name} ({health}%)")
				// is the same in every language.
				std::string words;
				for (bool inside = false; const char c : english) {
					if (c == '{' || c == '}') {
						inside = c == '{';
					} else if (!inside) {
						words += c;
					}
				}
				if (translated == english && std::ranges::count_if(words, [](char a_c) { return std::isalpha(static_cast<unsigned char>(a_c)); }) > 3) {
					Test::Fail(__FILE__, __LINE__, std::format("{}: '{}' is still the English text", name, key));
				}
			}
		}
	}
}

TEST("text: dist/lang/en.txt is the built-in English, and en-gb.txt differs from it only in spelling")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);
	const auto en = Text::Parse(SourceTree::Read(*root / "dist" / "lang" / "en.txt"));
	CHECK(en.size() == Text::English().size());
	for (const auto& entry : Text::English()) {
		const auto it = en.find(entry.key);
		CHECK(it != en.end() && it->second == entry.text);
	}
	const auto gb = Text::Parse(SourceTree::Read(*root / "dist" / "lang" / "en-gb.txt"));
	for (const auto& [key, text] : gb) {
		auto american = Text::GetEnglish(key);
		for (const auto& [us, uk] : std::vector<std::pair<std::string, std::string>>{ { "color", "colour" }, { "armor", "armour" }, { "favor", "favour" }, { "honor", "honour" }, { "center", "centre" }, { "defense", "defence" }, { "gray", "grey" }, { "ize", "ise" } }) {
			for (auto at = american.find(us); at != std::string::npos; at = american.find(us, at + uk.size())) {
				american.replace(at, us.size(), uk);
			}
		}
		CHECK(american == text);
	}
}

TEST("text: Latin American Spanish is its own file, not Spain's with another name")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);
	const auto spain = Text::Parse(SourceTree::Read(*root / "dist" / "lang" / "es.txt"));
	const auto latam = Text::Parse(SourceTree::Read(*root / "dist" / "lang" / "es-mx.txt"));
	std::size_t different = 0;
	for (const auto& [key, text] : latam) {
		const auto it = spain.find(key);
		different += it != spain.end() && it->second != text;
		for (const std::string_view form : { "vuestr", "vosotros", " os ", "Pulsa" }) {
			CHECK(text.find(form) == std::string::npos);
		}
	}
	CHECK(different * 2 > latam.size());
}

TEST("text: no player-facing text is a bare English literal in the code")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);
	// Each call that puts text in front of a player, and which of its arguments holds the text. The game's
	// notification and message box, a name shown over an actor (its name, or a text field of a menu: the name over a
	// friend's head), Steam's status line, and the server's refusals.
	struct Shown
	{
		std::string_view call;
		int              argument;
	};
	constexpr Shown SHOWN[] = {
		{ "DebugNotification(", 0 },
		{ "DebugMessageBox(", 0 },
		{ "ShowHUDMessage(", 0 },
		{ "SetDisplayName(", 0 },
		{ "SetOverrideName(", 0 },
		{ "SetMember(", 1 },  // a Flash text field's text (FriendMarkers: the name over a friend's head)
		{ "refuse(", 1 },
	};
	int         found = 0;
	std::size_t refusals = 0;  // what the guard saw, so a renamed call does not leave it checking nothing
	std::size_t statuses = 0;
	for (const std::string_view folder : { "src", "common" }) {
		for (const auto& [name, code] : SourceTree::Code(*root, folder)) {
			if (name == "src/TextEnglish.h" || name == "src/Text.h") {
				continue;
			}
			for (const auto& shown : SHOWN) {
				for (auto at = code.find(shown.call); at != std::string::npos; at = code.find(shown.call, at + 1)) {
					const auto open = at + shown.call.size() - 1;
					refusals += shown.call == "refuse(";
					if (IsLiteral(code, ArgumentAt(code, open, shown.argument))) {
						++found;
						Test::Fail(__FILE__, __LINE__, std::format("{} shows a literal through {}..): use a Text key", name, shown.call));
					}
				}
			}
			// Steam's status line: SetRichPresence(friends, "status", text) is read by friends; "connect" is not.
			for (auto at = code.find("SetRichPresence("); at != std::string::npos; at = code.find("SetRichPresence(", at + 1)) {
				const auto open = at + 15;
				const auto key = ArgumentAt(code, open, 1);
				const bool status = key != std::string_view::npos && code.substr(key).starts_with("\"status\"");
				statuses += status;
				if (status && IsLiteral(code, ArgumentAt(code, open, 2))) {
					++found;
					Test::Fail(__FILE__, __LINE__, std::format("{} sets Steam's status line from a literal: use a Text key", name));
				}
			}
		}
	}
	CHECK(found == 0);
	CHECK(refusals >= 6u);  // common/Server.cpp's refusals
	CHECK(statuses >= 1u);  // src/steam/Steam.cpp's status line

	// The guard finds what it looks for (so a rename of a call does not leave it checking nothing).
	const std::string sample = "RE::DebugNotification(\"Hello\");\nrefuse(a_key, std::format(\"{} left\", n), x);\nrefuse(a_key, Text::GetEnglish(\"reject.password\"), \"wrong password\");\n";
	CHECK(IsLiteral(sample, ArgumentAt(sample, sample.find('('), 0)));
	CHECK(IsLiteral(sample, ArgumentAt(sample, sample.find("refuse(") + 6, 1)));
	CHECK(!IsLiteral(sample, ArgumentAt(sample, sample.rfind("refuse(") + 6, 1)));
	const std::string picked = "api->SetRichPresence(api->friends, \"status\", hosting ? \"Hosting\" : \"Playing\");\n";
	CHECK(IsLiteral(picked, ArgumentAt(picked, picked.find('('), 2)));
	const std::string keyed = "api->SetRichPresence(api->friends, \"status\", Text::Get(hosting ? \"steam.a\" : \"steam.b\").c_str());\n";
	CHECK(IsLiteral(keyed, ArgumentAt(keyed, keyed.find('('), 2)));  // keys picked inline: hand the call a variable
	CHECK(!IsLiteral("x(status.c_str());", 2));
	const std::string later = "refuse(a_key, Text::GetEnglish(\"reject.full\"), full ? \"full\" : \"replaced\");\n";
	CHECK(!IsLiteral(later, ArgumentAt(later, later.find('('), 1)));  // a pick in a later argument is that argument's
	CHECK(IsLiteral(later, ArgumentAt(later, later.find('('), 2)));
}

TEST("text: every key the code asks for by name is in the English table")
{
	// A key nobody wrote shows as the key itself in the game.
	const auto root = SourceTree::Root();
	REQUIRE(root);
	const std::regex call{ R"(Text::(?:Format|Get|FormatEnglish|GetEnglish)\("([a-z0-9_.]+)\")" };
	const std::regex chosen{ R"re("((?:reject|steam)\.[a-z0-9_]+)")re" };  // a key picked by a condition, as Steam's status is
	std::size_t      keys = 0;
	for (const std::string_view folder : { "src", "common" }) {
		for (const auto& [name, code] : SourceTree::Code(*root, folder)) {
			if (name == "src/TextEnglish.h") {
				continue;
			}
			for (const auto* pattern : { &call, &chosen }) {
				for (auto it = std::sregex_iterator(code.begin(), code.end(), *pattern); it != std::sregex_iterator(); ++it) {
					const auto key = (*it)[1].str();
					++keys;
					if (std::ranges::none_of(Text::English(), [&](const auto& a_entry) { return a_entry.key == key; })) {
						Test::Fail(__FILE__, __LINE__, std::format("{} asks for the text key '{}', which src/TextEnglish.h lacks", name, key));
					}
				}
			}
		}
	}
	CHECK(keys >= 10u);
}

TEST("text: a player's own file goes over the mod's texts for that language, key by key")
{
	CHECK(Text::OwnFileName("ru") == "SkyrimCoop_lang_ru.txt");
	auto table = Text::Parse("reject.password = shipped\nreject.full = shipped full\n");
	Text::Overlay(table, Text::Parse("# mine\nreject.full = my full\nreject.mods = my mods\n"));
	CHECK(table.size() == 3);
	CHECK(table["reject.password"] == "shipped");
	CHECK(table["reject.full"] == "my full");
	CHECK(table["reject.mods"] == "my mods");
}

TEST("text: the package ships every language file, and the install guide names the player's own files")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);
	const auto package = SourceTree::Read(*root / "tools" / "package.ps1");
	CHECK(package.find("dist\\lang") != std::string::npos);
	CHECK(package.find("SKSE\\Plugins\\SkyrimCoop\\lang") != std::string::npos);
	const auto guide = SourceTree::Read(*root / "dist" / "INSTALL.md");
	CHECK(guide.find("SkyrimCoop_lang_") != std::string::npos);
	CHECK(guide.find("sLanguage") != std::string::npos);
}
