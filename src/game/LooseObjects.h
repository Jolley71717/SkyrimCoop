#pragma once

#include "Protocol.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Loose objects, the game's side (docs/DESIGN.md, "Loose objects"; the rules are src/game/ObjectRules.h, the server's
// common/ObjectBook.h). A loose object (a reference of the load order with one rigid body physics moves: clutter, an
// item lying about, a moveable static) ends up in the same place in every game:
// - Held: what our player holds with Z or telekinesis (PlayerCharacter's grabbed object) is said 15 times a second while
//   it moves (kObjectHolds, in our player's frame); a friend's hold makes our copy keyframed and draws it in front of
//   their stand-in. 80 ms after a let-go we say where it left the hands and how fast (kObjectRests kLetGo): their copy
//   flies on from there.
// - Knocked: four times a second the loose objects within SCAN_RANGE of our player are looked at; one that started
//   moving with no hand on it is ours to report when a cause of ours reached it and no stronger cause of a friend's did
//   (ObjectRules::Causes: our player's grab, hits, explosions, projectiles' paths, bodies; an NPC this game runs is ours,
//   a stand-in, a copy and what they launch are the friend's). Once it rests we say where (kRest); every other game puts
//   its copy there.
// - The session keeps each place for whoever joins or loads later: they wait here until their object is loaded near us.
// Physics is written on the main thread only (the tick); the explosion hook and the event sinks only record. Before a
// save, a load and the session's end, every copy held keyframed for a friend is dynamic again.
//
// Main thread, but the explosion hook and the hit and container event sinks (any thread).
namespace LooseObjects
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// At kDataLoaded: the explosion update hook and the event sinks. False when the hook did not go in (explosions are
	// then nobody's cause here; the log says so).
	bool Install();
	// Welcomed into a session as a_ourId.
	void Started(std::uint32_t a_ourId);
	// Once a tick in a session after the welcome, after NpcSync::Frame: our hold, let-gos and rests out; friends' holds
	// drawn, their let-gos and rests and the session's places put.
	void Frame(const SendFn& a_send);
	void Holds(const Protocol::ObjectHolds& a_msg);
	void Rests(const Protocol::ObjectRests& a_msg);
	// A player left: what they held falls (dynamic again where it is).
	void Left(std::uint32_t a_playerId);
	// The session ended: every copy held for a friend falls, and nothing is known.
	void Clear();
	// A load or a new game takes the world: nothing of the old one is held; the session's places are asked for again.
	void ForgetWorld();
	// SKSE's kSaveGame, before the game writes the save: every copy held keyframed for a friend is dynamic again.
	void BeforeSave();
	// Developer channel `objects`: counts (holds sent and drawn, let-gos, rests said, put, waiting, refused, quiet, echo
	// stops, causes, the explosion hook). `objects grab`: what our player holds. `objects probe <id>`: a reference's
	// pose, its bodies and motion type, and what this game knows of it (seen, watched, held by a friend, waiting).
	// `objects near [radius]`: the loose objects within radius (default 1500) of our player, nearest first (at most 20).
	[[nodiscard]] std::string Command(std::string_view a_args);
}
