#pragma once

#include "Protocol.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Riding, the game's side (docs/DESIGN.md, "Riding"; the rules are src/game/RideRules.h, the server's
// common/RideBook.h). This game says what its player rides and what each NPC it runs rides (kRides), as each changes;
// it hears the same of every other game, and seats the actor that shows each such rider here (a friend's stand-in, a
// copy of an NPC another game runs) on the actor that shows its mount (the copy of the friend's horse), through the
// engine's own way onto a mount, so the engine carries it in the saddle and plays the getting on and off. A seated
// rider is not placed by the actor update hooks (Seated); one that drifts from its saddle is put back on it.
//
// Main thread, but Seated (the update hooks, on the game's job threads).
namespace Riding
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// Welcomed into a session as a_ourId.
	void Started(std::uint32_t a_ourId);
	// Once a tick in a session after the welcome, after NpcSync::Frame: our rides out, every rider here seated or got off.
	void Frame(const SendFn& a_send);
	void Rides(const Protocol::Rides& a_msg);
	// A player left: their ride ends here, and those of the NPCs their game ran.
	void Left(std::uint32_t a_playerId);
	// The session ended: every rider this game seated gets off, and nothing is known.
	void Clear();
	// A load or a new game takes the world: no actor of the old one is held; riders are seated again in the new one.
	void ForgetWorld();
	// An actor of ours on its way out of the world (a stand-in, a copy of a spawn): off its mount, or its rider off it.
	void Release(RE::Actor* a_actor);
	// Whether this game seated a_actor on its mount and the engine has it there (any thread).
	[[nodiscard]] bool Seated(const RE::Actor* a_actor);
	// Developer channel `ride`: ours (on a mount or not, the mount's key and actor, the rides said), then each rider
	// another game says rides: on or not, the mount's key and the actor that shows it here, the rider's actor, what the
	// engine says (on that mount, on another, getting on or off), seated, the rounds tried and how, given up, its distance
	// from its saddle and the times it was put back; and the counts. `ride find [radius]`: the nearest mount no one rides
	// within radius (default 3000) of our player, its base, distance, and whether the crosshair is on it (aimed=), for the
	// rig to mount. `ride base`: the base of a plain horse of the load order, for the console's placeatme.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
