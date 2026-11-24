#pragma once

#include "Protocol.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Furniture, the game's side (docs/DESIGN.md, "Furniture"; the rules are src/game/SitRules.h, the server's
// common/SitBook.h). This game says what furniture its player is in and what each NPC it runs is in (kSits), as each
// changes: a chair, a bench, a bed, a railing, a crafting station, a work spot, a perch. It hears the same of every other
// game, and puts the actor that shows each one here (a friend's stand-in, a copy of an NPC another game runs) in its own
// copy of that furniture through the engine's own way in, so the engine plays the sitting down, the idle and the getting
// up. An actor in furniture, getting in or out, or waiting by the furniture its friend is in, is not placed by the actor
// update hooks (Held); while the engine gets it in or up, the engine's own update runs for it (Assisted). Graph events
// the engine's furniture play sends an actor's graph are never sent (EngineOwnsEvents): every game plays its own.
//
// Main thread, but Held, Assisted and EngineOwnsEvents (the update and graph event hooks, on the game's job threads).
namespace Furniture
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// Welcomed into a session as a_ourId.
	void Started(std::uint32_t a_ourId);
	// Once a tick in a session after the welcome, after NpcSync::Frame: ours out, every actor here put in or got out.
	void Frame(const SendFn& a_send);
	void Sits(const Protocol::Sits& a_msg);
	// A player left: they and the NPCs their game ran are out of their furniture here.
	void Left(std::uint32_t a_playerId);
	// The session ended: every actor this game put in furniture gets up, and nothing is known.
	void Clear();
	// A load or a new game takes the world: no actor of the old one is held; they are put in again in the new one.
	void ForgetWorld();
	// An actor of ours on its way out of the world (a stand-in, a copy of a spawn): out of its furniture at once, so none
	// leaves the world, or goes in a save, still in it.
	void Release(RE::Actor* a_actor);
	// Whether a_actor is held in or by its furniture: the update hooks neither place it nor tell it its walk (any thread).
	[[nodiscard]] bool Held(const RE::Actor* a_actor);
	// Whether the engine's own update runs for a_actor now: it is getting in or getting up (any thread).
	[[nodiscard]] bool Assisted(const RE::Actor* a_actor);
	// Whether a graph event a_actor's graph takes now is the engine's furniture play, never sent (any thread; counted).
	[[nodiscard]] bool EngineOwnsEvents(RE::Actor* a_actor);
	// Developer channel `sit`: ours (in furniture or not, its state, the furniture, its kind and marker, the sits said),
	// the sits of the NPCs we run as said (said npc=), then each actor another game says is in furniture: the furniture
	// said and the one found here (found=; lacked=1 none), taken by our player (taken=), near it (near=, dist=), the actor
	// that shows it here, its sit state and what its process names (occupies=), in it (seated=), held, assisted, the
	// rounds and ways tried and the way that took (seatedBy=), given up, the times it was stood up (stands=) or taken out
	// at once (quicks=), entries that stalled (stalls=), the marker said and the one it took here (markerHere=), whether it
	// waits for a new runner (handover=); and the counts. `sit find <sit|sleep|lean|work|perch|any> [radius] [markers]`:
	// the nearest furniture of that kind no one is in within radius (default 3000) of our player, with its markers,
	// distance and place, and whether the crosshair is on it (aimed=), for the rig; invisible marker furniture (the idle
	// spots only NPCs use) only with `markers`. `sit off <id>`: an NPC this game runs gets up (set-up).
	[[nodiscard]] std::string Command(std::string_view a_args);
}
