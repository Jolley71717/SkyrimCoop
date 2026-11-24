#pragma once

#include <string_view>

// The mod's player-facing texts in English (American): the fallback for every language and the source of
// dist/lang/en.txt (a test keeps the two the same). A text holds {named} placeholders; a line break is "\n".
// Add a key here and to every dist/lang file (the test says which file lacks it); the tests check each file's
// placeholders against these. Keep the English plain: short words, no idioms, so it translates well.
//
// What is not here is not read by players: the log, the dedicated server's console and the dev channel's replies
// are English whatever the language.
namespace Text
{
	struct Entry
	{
		std::string_view key;
		std::string_view text;
	};

	inline constexpr Entry ENGLISH_TABLE[] = {
		// --- What the server says when it refuses a player (common/Server.cpp sends the English, filled in; the
		// joining game shows it in its own language with Text::Reject) ---
		{ "reject.unreadable", "The join request could not be read." },
		{ "reject.not_this_mod", "This is not a SkyrimCoop game." },
		{ "reject.version", "This game runs another version of the mod (protocol {you}, the host has {host}). Both players need the same version." },
		{ "reject.password", "Wrong password." },
		{ "reject.full", "The game is full ({n} players)." },
		{ "reject.mods", "Your mods differ from the host's." },

		// --- Steam: the status line friends see beside our name in their friends list (src/steam/Steam.cpp), in the
		// language of the player who sets it ---
		{ "steam.status_hosting", "Hosting Skyrim co-op" },
		{ "steam.status_playing", "Playing Skyrim co-op" },

		// --- A player brought down and helped up (src/game/Downed.cpp), on the screen's top left ---
		{ "down.you", "You are down. A friend who stays by your side can help you up. You have {n} seconds." },
		{ "down.left", "{n} seconds left for a friend to help you up." },
		{ "down.friend", "{player} is down. Stay by their side to help them up." },
		{ "down.helping", "Helping {player} up..." },
		{ "down.helped_them", "You helped {player} up." },
		{ "down.helped_you", "{player} helped you up." },
	};
}
