#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

// The lobby a friend's invite started this game for. When a player accepts an invite (or clicks "Join Game") while
// the game is not running, Steam starts it with the lobby on its command line: "+connect_lobby <id>" (Steam's own,
// for a lobby invite) or our rich presence "connect" string, "+skyrimcoop_lobby <id>" (src/steam/Steam.cpp,
// AdvertiseLobby). No game here, so it is unit-tested.
namespace LaunchArgs
{
	// A Steam lobby id: its account type (bits 52-55) is 8, a chat, and its universe (bits 56-63) public.
	[[nodiscard]] inline bool LooksLikeLobby(std::uint64_t a_id)
	{
		return ((a_id >> 52) & 0xF) == 8 && (a_id >> 56) == 1;
	}

	// The lobby id after the first of the two switches in a_commandLine, or nothing (no switch, no number after it,
	// or a number that is no lobby id).
	[[nodiscard]] inline std::optional<std::uint64_t> Lobby(std::string_view a_commandLine)
	{
		for (const auto key : { std::string_view{ "+connect_lobby" }, std::string_view{ "+skyrimcoop_lobby" } }) {
			auto at = a_commandLine.find(key);
			while (at != std::string_view::npos) {
				auto rest = a_commandLine.substr(at + key.size());
				// The switch whole: "+connect_lobbyX" is another word.
				if (rest.empty() || (rest.front() != ' ' && rest.front() != '\t' && rest.front() != '"')) {
					at = a_commandLine.find(key, at + key.size());
					continue;
				}
				while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t' || rest.front() == '"')) {
					rest.remove_prefix(1);
				}
				std::uint64_t id = 0;
				const auto [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), id);
				if (error == std::errc{} && end != rest.data() && LooksLikeLobby(id)) {
					return id;
				}
				break;
			}
		}
		return std::nullopt;
	}
}
