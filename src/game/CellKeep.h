#pragma once

#include <cstdint>
#include <optional>

// An actor this game moves by its position alone (a friend's stand-in, the copy of an NPC another game runs: their AI,
// which would change their cell as they walk, never runs) kept in the cell under it (docs/DESIGN.md, "The other
// players": "When it is there"). The engine does not take a change of position as a change of cell, so as the group walks
// on, the cell such an actor was placed in is let go under it and takes its 3D. Once a second each is moved into the
// loaded cell under it. One the game holds somewhere else is not moved itself: a rider in the saddle goes with its mount,
// which is moved instead; one in furniture or a paired animation stays where those put it. The rules are
// src/game/SpaceRules.h (Recell, Whom). Main thread.
namespace CellKeep
{
	struct Moved
	{
		std::uint32_t actor = 0;  // the actor moved (a_actor, or the mount it rides)
		std::uint32_t from = 0;
		std::uint32_t to = 0;
		bool          mount = false;
	};

	// Moves a_actor (or the mount it is seated on) into the loaded exterior cell under it when that is another: what was
	// moved, or none.
	std::optional<Moved> Keep(RE::Actor* a_actor);
}
