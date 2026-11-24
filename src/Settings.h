#pragma once

#include "IniFile.h"
#include "Protocol.h"

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The plugin's settings (Data/SKSE/Plugins/SkyrimCoop.ini) read from the file's text. No game code here: Config.cpp
// reads the file and logs, the unit tests check the reading.
namespace Settings
{
	// 7791, not F4Multiplayer's 7790: the rig's tunnels and tools for the two games never reach the wrong one.
	inline constexpr std::uint16_t DEV_CHANNEL_PORT = 7791;

	struct Values
	{
		// Name shown to the other players: "character" (the character's name), empty (the Steam name) or any name.
		std::string playerName = "character";
		// This game hosts the session.
		bool host = false;
		// "auto", "steam" or "enet" (direct UDP).
		std::string transport = "auto";
		// For joining over direct UDP: "ip" or "ip:port". Empty joins through Steam.
		std::string serverAddress;
		std::uint16_t port = Protocol::DEFAULT_PORT;
		// When hosting: players in the session at once, the host included.
		std::uint32_t maxPlayers = 4;
		std::string   password;
		// The plugin uses the game's Steam library (lobbies, invites, relay). Off: it leaves Steam alone, for
		// finding out whether a start-up problem is Steam's; only direct UDP is left.
		bool steam = true;
		// The language of the mod's texts: empty for the game's own, or a language file's code ("de", "pt-br") or name.
		std::string language;
		// "info" or "debug".
		std::string logLevel = "info";
		// While in a session, the game keeps running when its window is not focused (the game's own bAlwaysActive,
		// put back as it was after the session; src/Background.h).
		bool runInBackground = true;
		// Map markers a friend discovers in the session go on our map, ready for fast travel, and ours go on theirs
		// (src/game/MapShare.h). Off: neither.
		bool shareMap = true;
		// The developer channel (DevChannel.h): test tools on this PC drive the game through it. Off for players.
		bool          devChannel = false;
		std::uint16_t devChannelPort = DEV_CHANNEL_PORT;

		friend bool operator==(const Values&, const Values&) = default;
	};

	namespace detail
	{
		inline bool ParseBool(std::string_view a_value, bool a_current)
		{
			if (a_value == "1" || a_value == "true" || a_value == "True" || a_value == "TRUE") {
				return true;
			}
			if (a_value == "0" || a_value == "false" || a_value == "False" || a_value == "FALSE") {
				return false;
			}
			return a_current;
		}

		// The whole value as a number, or nothing.
		inline bool ParseNumber(std::string_view a_value, std::uint32_t& a_out)
		{
			std::uint32_t value = 0;
			const auto [end, error] = std::from_chars(a_value.data(), a_value.data() + a_value.size(), value);
			if (error != std::errc{} || end != a_value.data() + a_value.size()) {
				return false;
			}
			a_out = value;
			return true;
		}
	}

	// The settings a_text holds; a key that is missing, or whose value can't be used, keeps its default. What could
	// not be used is said in a_notes, one line each, for the log (never a password's value).
	[[nodiscard]] inline Values Parse(std::string_view a_text, std::vector<std::string>* a_notes = nullptr)
	{
		Values     values;
		const auto note = [&](std::string a_line) {
			if (a_notes) {
				a_notes->push_back(std::move(a_line));
			}
		};

		if (const auto name = IniFile::GetValue(a_text, "sPlayerName")) {
			values.playerName = Protocol::Utf8Prefix(*name, Protocol::MAX_NAME_LENGTH);
		}
		if (const auto host = IniFile::GetValue(a_text, "bHost")) {
			values.host = detail::ParseBool(*host, values.host);
		}
		if (const auto transport = IniFile::GetValue(a_text, "sTransport")) {
			if (*transport == "auto" || *transport == "steam" || *transport == "enet") {
				values.transport = *transport;
			} else {
				note("sTransport = " + *transport + " is not auto, steam or enet; it is auto");
			}
		}
		if (const auto address = IniFile::GetValue(a_text, "sServerAddress")) {
			values.serverAddress = *address;
		}
		if (const auto port = IniFile::GetValue(a_text, "iPort")) {
			std::uint32_t number = 0;
			if (detail::ParseNumber(*port, number) && number >= 1 && number <= 65535) {
				values.port = static_cast<std::uint16_t>(number);
			} else {
				note("iPort = " + *port + " is not a port (1 to 65535); it is " + std::to_string(values.port));
			}
		}
		if (const auto players = IniFile::GetValue(a_text, "iMaxPlayers")) {
			std::uint32_t number = 0;
			if (detail::ParseNumber(*players, number) && number >= 2 && number <= Protocol::MAX_PLAYERS) {
				values.maxPlayers = number;
			} else {
				note("iMaxPlayers = " + *players + " is not 2 to " + std::to_string(Protocol::MAX_PLAYERS) + "; it is " + std::to_string(values.maxPlayers));
			}
		}
		if (const auto password = IniFile::GetValue(a_text, "sPassword")) {
			if (password->size() <= Protocol::MAX_PASSWORD_LENGTH) {
				values.password = *password;
			} else {
				note("sPassword is longer than " + std::to_string(Protocol::MAX_PASSWORD_LENGTH) + " characters; no password is used");
			}
		}
		if (const auto language = IniFile::GetValue(a_text, "sLanguage")) {
			values.language = *language;
		}
		if (const auto steam = IniFile::GetValue(a_text, "bSteam")) {
			values.steam = detail::ParseBool(*steam, values.steam);
		}
		if (const auto background = IniFile::GetValue(a_text, "bRunInBackground")) {
			values.runInBackground = detail::ParseBool(*background, values.runInBackground);
		}
		if (const auto share = IniFile::GetValue(a_text, "bShareMap")) {
			values.shareMap = detail::ParseBool(*share, values.shareMap);
		}
		if (const auto dev = IniFile::GetValue(a_text, "bDevChannel")) {
			values.devChannel = detail::ParseBool(*dev, values.devChannel);
		}
		if (const auto port = IniFile::GetValue(a_text, "iDevChannelPort")) {
			std::uint32_t number = 0;
			if (detail::ParseNumber(*port, number) && number >= 1024 && number <= 65535) {
				values.devChannelPort = static_cast<std::uint16_t>(number);
			} else {
				note("iDevChannelPort = " + *port + " is not a port (1024 to 65535); it is " + std::to_string(values.devChannelPort));
			}
		}
		if (const auto level = IniFile::GetValue(a_text, "sLogLevel")) {
			if (*level == "info" || *level == "debug") {
				values.logLevel = *level;
			} else {
				note("sLogLevel = " + *level + " is not info or debug; it is info");
			}
		}
		return values;
	}
}
