#include "Config.h"

#include "Log.h"
#include "Platform.h"

namespace Config
{
	namespace
	{
		constexpr auto PATH = "Data/SKSE/Plugins/SkyrimCoop.ini"sv;

		Settings::Values values;
	}

	void Load()
	{
		std::ifstream file{ Platform::GamePath(PATH), std::ios::binary };
		if (!file) {
			logs::info("Config: {} not found, using defaults", PATH);
			return;
		}
		std::ostringstream text;
		text << file.rdbuf();

		std::vector<std::string> notes;
		values = Settings::Parse(text.str(), &notes);
		for (const auto& note : notes) {
			logs::warn("Config: {}", note);
		}
		Log::SetLevel(values.logLevel);
		// Never the password or the address: players attach this log to public bug reports.
		logs::info("Config: host {}, transport {}, port {}, up to {} players, password {}, address {}, language {}, log level {}, run in background {}, share the map {}, dev channel {}",
			values.host ? "yes" : "no", values.transport, values.port, values.maxPlayers, values.password.empty() ? "none" : "set",
			values.serverAddress.empty() ? "none" : "set", values.language.empty() ? "the game's" : values.language, Log::Level(),
			values.runInBackground ? "yes" : "no", values.shareMap ? "yes" : "no", values.devChannel ? std::format("on (port {})", values.devChannelPort) : std::string{ "off" });
	}

	const Settings::Values& Get()
	{
		return values;
	}
}
