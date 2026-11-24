#pragma once

#include "Protocol.h"

#include <functional>

// The NPCs of the session (docs/DESIGN.md, "NPCs"): each NPC near a player is run by one game, its AI included, and the
// others show a copy of it moved by that game's word.
//
// This game asks the server for the NPCs it has loaded (NpcRules::Decide: the nearest player's game first, a fight or
// a follower takes one over, the runner lets go of the ones its player left behind) and hears who runs each. Of the
// ones it runs it sends where each is and how it moves, 20 or 10 times a second (kNpcStates), the events its graph
// takes (kNpcActions: an attack, a block, a dragon's take-off) and what it wears and holds (kNpcGear). Every kind of
// actor is one: people, creatures, dragons, horses, followers, and what our player or an NPC this game runs summons (a
// spawn: a reference this game made as it ran, which the others place a copy of from its base).
//
// A copy of an NPC another game runs is that NPC in this game (a spawn's copy: one this game placed). While its
// runner's states are recent and in this player's space, its AI never runs: the actor update hook (RemotePlayers.cpp)
// places it where its runner has it a short delay ago (StateBuffer), its graph's Speed and Direction are its runner's
// (the stand-ins' leg channels, GaitDrive), its walk's events come from Gait as a stand-in's do, and the runner's other
// graph events are played at the same point of its walk (NpcRules::ActionQueue). Otherwise it is left to its own AI.
//
// Health and deaths (docs/DESIGN.md, "NPC health and deaths"): the runner's game is the truth of both. Our player's hit
// on a copy goes to the runner as a numbered request (kNpcHits) and ends in one answer (kNpcHitAnswers); the copy shows
// the runner's word (kNpcHealths) less our hits not answered yet, and anything else that changed it is set back
// (CopyHealth.h). The runner says each death once (kNpcDeaths); every copy dies with it, and an NPC the session has dead
// is killed as it loads in any game but the host's (whose save is the truth of its own dead).
//
// Main thread, except UpdateCopy (the actor update hook, on the game's job threads), the graph event hook and the hit event
// sink (whatever thread sends the event).
namespace NpcSync
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// At kDataLoaded, after RemotePlayers::Install: the hook that hears the graph events of the NPCs this game runs, and
	// the hit events that tell a fight. False when the hook could not go in (copies then show no actions).
	bool Install();

	// Welcomed into a session as a_ourId; a_hostId is the host's player (0: none in).
	void Started(std::uint32_t a_ourId, std::uint32_t a_hostId);
	// Once a tick in a session after the welcome: claims, states, actions and gear out; copies placed, moved and
	// dressed. a_send sends a packet (reliable or not) the way every packet goes (in the session's numbering of forms).
	void Frame(const SendFn& a_send);
	// The session ended: nothing is run or copied any more; copies of spawns go.
	void Clear();

	void Owners(const Protocol::NpcOwners& a_msg);
	void States(const Protocol::NpcStates& a_msg);
	void Actions(const Protocol::NpcActions& a_msg);
	void Gear(const Protocol::NpcGear& a_msg);
	// Health and deaths (docs/DESIGN.md, "NPC health and deaths"): a friend's hits on NPCs this game runs, the answers
	// to ours on copies, the runners' word of their NPCs' health, and their deaths.
	void Hits(const Protocol::NpcHits& a_msg);
	void HitAnswers(const Protocol::NpcHitAnswers& a_msg);
	void Healths(const Protocol::NpcHealths& a_msg);
	void Deaths(const Protocol::NpcDeaths& a_msg);

	// From the Character update hook, for an actor that is no stand-in: true when it is a copy this game moves now (its
	// AI is not run); false: the game's own update runs.
	bool UpdateCopy(RE::Actor* a_actor, float a_delta);

	// A load or a new game takes the world: every NPC we ran is let go of (the next frame claims what the new world
	// has), and no actor of the old one is held.
	void ForgetWorld();
	// Before a save (SKSE's kSaveGame, before RemotePlayers::BeforeSave): copies of spawns go out of the world (the save
	// guard names them in the co-save).
	void BeforeSave();

	// Developer channel: `npcs` (counts), `npcs list` (each NPC: who runs it, its kind, where it is and where its copy
	// is), `npcs <form id>` (one NPC).
	[[nodiscard]] std::string Describe(std::string_view a_args);
	// Developer channel `swim` (the copies' part, after ours and the stand-ins': Water::Command): how many copies' runners
	// have them swimming and under water, how many are shown swimming, the swim events their graphs took and turned
	// away and the changes given up, how often the water moved one off its height; then each copy whose runner has it
	// swimming, and each copy of a race that does not walk (a fish), at most MAX_SWIM_LISTED: its base, whether its race
	// walks, what it is shown doing, what the engine says (its swimming flag, its character controller's state), its own
	// swim events and refloats, the engine's last move of it off its depth, and its height against its runner's.
	[[nodiscard]] std::string Swim();
}
