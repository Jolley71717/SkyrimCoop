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

		// --- A friend's name as their stand-in shows it, over its head and where the crosshair names it, with their
		// health when hurt (src/game/Party.cpp, Tag) ---
		{ "party.tag_hurt", "{name} ({health}%)" },

		// --- A friend discovered a place: it is now on our map too, ready for fast travel (src/game/MapShare.cpp) ---
		{ "map.found", "{player} discovered {place}" },

		// --- Fast travel together: a friend near us fast travelled or took a carriage, and we may follow; why following was
		// refused (src/game/TravelTogether.cpp) ---
		{ "travel.offer", "{player} is traveling to {place}. Press {key} to follow." },
		{ "travel.offer_somewhere", "{player} is traveling. Press {key} to follow." },
		{ "travel.refused_combat", "You can't follow while in combat." },
		{ "travel.refused_weight", "You are carrying too much to follow." },
		{ "travel.refused_now", "You can't follow right now." },
		{ "travel.refused_place", "You can't fast travel from here." },
	};
}
