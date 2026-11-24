#pragma once

#include "Protocol.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// A player brought down and helped up, the game's side (docs/DESIGN.md, "Downed and helped up"; the rules are
// src/game/DownRules.h).
// - Ours: with a friend in the session our player is essential by our own flag (the player's base and the actor's own,
//   never in a save: BeforeSave takes it and the no-recovery flag off, the next frame puts them back; a save made while
//   down, loaded, is stood up), so a blow that would kill them brings them
//   down into the engine's bleedout, its own recovery off. Our game says the down (kDowned) and waits for a friend's
//   help (kHelpUp) for DownRules::BLEED_OUT; helped, our player gets HEALTH_ON_UP of their health and the engine stands
//   them up (by hand when it does not); no help in time, our flag comes off and they die: the game loads their save.
// - Theirs: a friend who waits for help kneels here (their stand-in's graph, DownRules::StandInKneel), and our player
//   standing within HELP_RANGE of them for HELP_TIME helps them up (DownRules::Helper): a kHelpUp naming them.
//
// Main thread.
namespace Downed
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// Welcomed into a session as a_ourId.
	void Started(std::uint32_t a_ourId);
	// Each tick, in a session or not (a_send: none outside one): our down judged and said, friends' stand-ins kneeled,
	// helps sent. a_friends: the other players in the session (0 outside one).
	void Frame(std::size_t a_friends, const SendFn& a_send);
	// A friend's down (the server names them).
	void Heard(const Protocol::Downed& a_msg);
	// A friend helped our player up (the server names them).
	void HelpedBy(std::uint32_t a_helper);
	// A friend left: their down is forgotten (their stand-in goes with them).
	void Left(std::uint32_t a_playerId);
	// The session ended: friends' downs are forgotten. Ours goes on (its clock ends it; a friend may come back).
	void Clear();
	// SKSE's kSaveGame (every save: ours, an autosave, another mod's): our flags off (essential, no recovery), so no save
	// keeps them (the next frame puts them back while down).
	void BeforeSave();
	// A load or a new game: our flags off the player first (its base is a form a load may not write again), and the world
	// of the down is gone.
	void ForgetWorld();

	// Developer channel `downed`: ours (phase, flag, life state, seconds left, counts) and each friend's (said, kneel,
	// help). For the rig.
	[[nodiscard]] std::string Describe();
}
