#pragma once

#include "Protocol.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Bodies of the dead, the game's side (docs/DESIGN.md, "Bodies"; the rules are src/game/BodyRules.h, the server's
// common/BodyBook.h). The runner of an NPC is the truth of how its body fell and where it lies:
// - At the death of an NPC this game runs, its fall is said at once (kBodies kFall: its pelvis's velocity, from the
//   killing blow); its ragdoll is watched until it settles, then its bones are said (kRest: each by its node's name).
// - A copy's fall is pushed the same way once it died here (every bone given that velocity, within FALL_WAIT of hearing
//   it); its rest is laid once it has been dead here a moment and lies more than CORPSE_SLACK off: each of its ragdoll's
//   bones named in the rest is written where the runner's lies.
// - The server keeps each body's rest for whoever joins or loads later (asked again with the loose objects' places).
// Physics is written on the main thread only, under the cell's Havok world lock.
//
// Main thread.
namespace Bodies
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// Welcomed into a session as a_ourId.
	void Started(std::uint32_t a_ourId);
	// Once a tick in a session after the welcome, after NpcSync::Frame (its deaths go first): our NPCs' falls and rests
	// out; the runners' put on their copies.
	void Frame(const SendFn& a_send);
	void Heard(const Protocol::Bodies& a_msg);
	// The session ended: nothing is known.
	void Clear();
	// A load or a new game takes the world: no body of the old one is watched (the session's are sent again on the
	// loose objects' ask).
	void ForgetWorld();
	// Developer channel `bodies`: falls and rests said, heard, pushed and laid, waiting, given up. `bodies probe <id>`:
	// an actor's ragdoll bones (how many, the pelvis's place and velocity, their motion), whether it is dead and in its
	// ragdoll, and what this game knows of its body.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
