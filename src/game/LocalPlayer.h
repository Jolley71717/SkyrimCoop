#pragma once

#include "Protocol.h"
#include "game/Gear.h"

// The player of this game: what the other players are told about them. Main thread only.
namespace LocalPlayer
{
	// Whether the player stands in a loaded world: a game is loaded, the character's 3D is there and no loading
	// screen is up. Nothing is sent, and no stand-in shown, while not.
	[[nodiscard]] bool InWorld();

	// Where the player is and what they are doing, as it is sent to the others (Protocol::PlayerState; the sequence and
	// sentMs are the sender's to set). Nothing while not InWorld.
	[[nodiscard]] std::optional<Protocol::PlayerState> Read();

	// What the character looks like, as a friend's stand-in of them shows it: race, sex, weight, the armor worn, and
	// what each hand, the quiver and the voice hold. Nothing while not InWorld.
	[[nodiscard]] std::optional<Protocol::PlayerLooks> Looks();
	// What each hand, the quiver and the voice hold, alone: read often (a few times a second), so a sword taken out is
	// on the friend's stand-in before its swing is. Nothing while not InWorld.
	[[nodiscard]] std::optional<Gear::Hands> Hands();

	// At data load, before any game: the name the game gives a character that has none yet ("Prisoner").
	void NoteDefaultName();

	// The character's name; empty before the character has one (still the game's default name: a new game before
	// the character is made, or a game started with coc from the main menu). The name sent is then the Steam name.
	[[nodiscard]] std::string CharacterName();

	// The menus open now ("HUD Menu,MessageBoxMenu modal=1"): for `where`, so the rig sees a box that blocks input.
	[[nodiscard]] std::string OpenMenus();

	// For the developer channel's `where`: one line of where the player is, what their graph says and what menus are
	// open.
	[[nodiscard]] std::string Describe();
}
