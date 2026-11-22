#pragma once

#include "Protocol.h"

// The player of this game: what the other players are told about them. Main thread only.
namespace LocalPlayer
{
	// Whether the player stands in a loaded world: a game is loaded, the character's 3D is there and no loading
	// screen is up. Nothing is sent, and no stand-in shown, while not.
	[[nodiscard]] bool InWorld();

	// Where the player is and what they are doing, as it is sent to the others (Protocol::PlayerState; the sequence and
	// sentMs are the sender's to set). Nothing while not InWorld.
	[[nodiscard]] std::optional<Protocol::PlayerState> Read();

	// What the character looks like, as a friend's stand-in of them shows it: race, sex, weight and the armor worn.
	// Nothing while not InWorld.
	[[nodiscard]] std::optional<Protocol::PlayerLooks> Looks();

	// The character's name; empty before the character has one.
	[[nodiscard]] std::string CharacterName();

	// For the developer channel's `where`: one line of where the player is and what their graph says.
	[[nodiscard]] std::string Describe();
}
