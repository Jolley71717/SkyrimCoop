#pragma once

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// What players take from the world (docs/DESIGN.md, "Taken from the world"; the rules, free of the game, are
// src/game/PickupRules.h, the server's book common/PickupBook.h). A loose item of the load order our player picks up is
// told to the session (kPickups), and in every other game that reference goes out of the world (disabled, never deleted);
// a plant or tree our player harvests is told the same way and harvested in every other game, where it grows back on that
// game's own schedule. A game that joins, or loads a save, hears every one the session holds that was not its own player's
// (kRemotePickups, catchUp; kAskPickups after a load): its own come back with its save, its to take again.
//
// A reference not in memory here (its cell not loaded) waits, and is made when its cell attaches. Keys, books, a quest
// alias's reference and runtime references stay each player's own.
//
// From the tick (GameThread) and SKSE's messages: every entry point takes one lock over the state, never while sending.
// The event sinks never take it: they queue.
namespace Pickups
{
	using SendFn = std::function<void(std::vector<std::uint8_t>)>;

	// At kDataLoaded: the event sinks (our player's pickups and activations, the engine's harvests, cells attaching). False
	// when the game's event source is not there.
	bool Install();

	// The session let us in as a_playerId: the server sends what it holds, so nothing is asked.
	void Welcomed(std::uint32_t a_playerId);

	// Once a tick while in a session and let in: tells the session our pickups and harvests, makes what it told us.
	void Frame(const SendFn& a_send);

	// From the session: another player's pickups (kRemotePickups), or the kept ones (catchUp).
	void Heard(const Protocol::PickupBatch& a_batch);

	// A save was loaded or a new game started: what was made here is the save's now, so the session's are asked for again.
	void AfterLoad();
	void NewGame();

	// The session ended: what it told us is forgotten (what was made here stays made: this world's now).
	void Clear();

	// Developer channel: `pickups` (one line of counts), `pickups near [radius]` (loose items and plants near our player),
	// `pickups pick <item|book|key|flora> [radius]` (the nearest one of that sort, for the rig to walk to), `pickups ref
	// <ref hex>` (what became of a reference here), `pickups take <ref hex>` (our player's activation of it, within reach:
	// a Take, or a harvest). No names.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
