#include "Text.h"

#include "Config.h"
#include "Log.h"
#include "Platform.h"
#include "net/Identity.h"

namespace Text
{
	namespace
	{
		constexpr auto LANG_FOLDER = "Data/SKSE/Plugins/SkyrimCoop/lang"sv;

		std::optional<std::string> ReadFile(const std::filesystem::path& a_path)
		{
			std::ifstream in{ a_path, std::ios::binary };
			if (!in) {
				return std::nullopt;
			}
			return std::string{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
		}

		// The language the game is set to: sLanguage of its [General] section, as the game read it from
		// Skyrim_Default.ini, Skyrim.ini and SkyrimCustom.ini (the last that has it decides). Empty when it has none.
		std::string GameLanguage()
		{
			if (const auto* setting = RE::GetINISetting("sLanguage:General"); setting && setting->GetType() == RE::Setting::Type::kString) {
				if (const char* value = setting->GetString(); value && *value) {
					return value;
				}
			}
			return {};
		}
	}

	void Load()
	{
		const auto& configured = Config::Get().language;
		const bool  fromIni = !configured.empty();
		const auto  asked = fromIni ? configured : GameLanguage();
		auto        code = asked.empty() ? std::string{ "en" } : CodeFor(asked);
		if (!IsFileCode(code)) {
			logs::warn("Text: '{}' is not a language code; using English", asked);
			code = "en";
		}

		// A key the English table lacks is a typo in the file (or a text from a newer version): it is never used.
		const auto warnUnknown = [](const Table& a_table, std::string_view a_file) {
			std::size_t unknown = 0;
			for (const auto& [key, text] : a_table) {
				if (std::ranges::none_of(English(), [&](const Entry& a_entry) { return a_entry.key == key; })) {
					if (++unknown <= 3) {
						logs::warn("Text: {} has a key the mod does not use: '{}'", a_file, key);
					}
				}
			}
		};

		Table      table;
		bool       found = false;
		const auto path = Platform::GamePath(LANG_FOLDER) / (code + ".txt");
		if (const auto text = ReadFile(path)) {
			table = Parse(*text);
			found = true;
			warnUnknown(table, code + ".txt");
			logs::info("Text: language {} ({} texts from {}.txt; the rest are English)", code, table.size(), code);
		}
		// The player's own texts for this language, over the mod's: a correction to a shipped file, or a language the
		// mod doesn't ship. The file is beside the log, outside Data, so no update or mod manager replaces it (an edit
		// to a file in the lang folder is replaced by the next version's file).
		if (const auto folder = Identity::DataFolder()) {
			const auto name = OwnFileName(code);
			if (const auto text = ReadFile(*folder / name)) {
				auto own = Parse(*text);
				warnUnknown(own, name);
				logs::info("Text: language {} ({} texts of the player's own from {}, over the mod's)", code, own.size(), name);
				Overlay(table, std::move(own));
				found = true;
			}
		}
		if (!found) {
			if (code == "en") {
				logs::info("Text: language en (built in)");
			} else {
				logs::warn("Text: no {}.txt in {} for {} '{}'; using English", code, LANG_FOLDER, fromIni ? "sLanguage in SkyrimCoop.ini" : "the game's language", asked);
				code = "en";
			}
		}
		Use(code, std::move(table));
	}
}
