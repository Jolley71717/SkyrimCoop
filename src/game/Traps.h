#pragma once

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// World traps (docs/DESIGN.md, "Traps"; the rules, free of the game, are src/game/TrapRules.h, the server's book
// common/TrapBook.h). A one-shot trap of the load order (a tripwire, a trapped chest's or door's hinge, a rigged rockfall,
// a breakaway floor, a magic trap's soul gem) that goes off or is disarmed in our game is told to the session (kTraps),
// and in every other game it is spent too: set off its own way when the word is fresh and that player is near, made spent
// quietly otherwise. A bear trap our player snaps or sets again is told the same way. A game that joins, or loads a save,
// hears every word the session holds that was not its own player's (kRemoteTraps, catchUp; kAskTraps after a load).
//
// Traps are read, not hooked: those loaded near our player are found once a second and their script's state read a few
// times a second, and once more as one leaves the world (a soul gem taken is disarmed and disabled in one call). A change
// with a friend's stand-in nearer to the trap than our player is that friend's, told by their game. A word on a trap not
// loaded here waits until it is. A friend's stand-in never makes a use of a trap here, and a pickup's word never takes
// one out of the world (src/game/WorldObjects.cpp and Pickups.cpp ask IsTrap): the trap's word says what became of it.
//
// From the tick (GameThread) and SKSE's messages: every entry point takes one lock over the state, never while sending.
namespace Traps
{
	using SendFn = std::function<void(std::vector<std::uint8_t>)>;

	// At kDataLoaded.
	bool Install();

	// The session let us in as a_playerId: the server sends what it holds, so nothing is asked.
	void Welcomed(std::uint32_t a_playerId);

	// Once a tick while in a session and let in: reads the traps near our player, tells the session our words, makes
	// what it told us.
	void Frame(const SendFn& a_send);

	// From the session: another player's words (kRemoteTraps), or the kept ones (catchUp).
	void Heard(const Protocol::TrapBatch& a_batch);

	// A save was loaded or a new game started: its traps are the save's now, so the session's words are asked for again.
	void AfterLoad();
	void NewGame();

	// The session ended: what it told us is forgotten (what was made here stays made: this world's now).
	void Clear();

	// Whether a reference is a trap this class shares (its scripts say so): a stand-in's use of it, and what a use did to
	// it, are not made (the trap's word is). Any thread.
	[[nodiscard]] bool IsTrap(RE::TESObjectREFR* a_ref);

	// Developer channel: `traps` (one line of counts), `traps near [radius]` (the traps near our player, nearest first),
	// `traps pick <tripwire|hinge|rockfall|breakaway|bear|soulgem|oneshot|any> [armed|spent] [radius]` (the nearest one,
	// for the rig to walk to), `traps ref <ref hex>` (how a trap reads here and the word on it), `traps use <ref hex>` (our
	// player's activation of it, within reach, as the activate key does). No names.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
