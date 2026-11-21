#pragma once

#include "Protocol.h"

// The player of this game: what the other players are told about them.
//
// NOT WRITTEN YET (docs/PORTING.md, "The local player"): both answer "nothing" until the reading of the player
// character is written and checked in a game.
namespace LocalPlayer
{
	// Where the player is and what they are doing, as it is sent to the others (Protocol::PlayerState). Nothing
	// while no game is loaded or the player has no place in the world (main menu, loading screen). Main thread only.
	[[nodiscard]] std::optional<Protocol::PlayerState> Read();

	// The character's name; empty before the character has one. Main thread only.
	[[nodiscard]] std::string CharacterName();
}
