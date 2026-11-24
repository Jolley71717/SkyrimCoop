#pragma once

#include "Protocol.h"
#include "game/LootRules.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace SKSE
{
	class SerializationInterface;
}

// Containers and loot in a session (docs/DESIGN.md, "Containers and loot"; the rules, free of the game, are
// src/game/LootRules.h). The game's own event says what our player moved (TESContainerChangedEvent: an item from a
// container or a body to our player, or from our player into one); each is read on the next frame, judged, and reported
// if it is the session's (kLoot). The server numbers every move it takes and passes it to every game; each game makes
// the others' moves in its own copy of the holder (at once when it is loaded, once it loads otherwise; a body's once it
// is dead here too), and keeps a ledger of the numbers its world holds, in its co-save, so a game that loads a save or
// joins again asks for the moves from there (kAskLoot) and makes the ones its world lacks.
//
// From the tick (GameThread: on the task pool's job threads once in the world), and from SKSE's messages and co-save
// callbacks on the main thread: every entry point takes one lock over LootSync's state. The event sinks never take it:
// the container event's only queues, and the container menu's (on the UI's thread) only says which holder the menu
// shows, where no move is made until it shuts. The game writes made from the tick are counted when they are made off
// the main thread (`loot`: offmain=).
namespace LootSync
{
	using SendFn = std::function<void(std::vector<std::uint8_t>)>;

	// At kDataLoaded: the event sinks (our player's moves, the container menu's open and close) and the merchants'
	// chests. False when the game's event source is not there.
	bool Install();

	// The session let us in as a_playerId: what this world lacks of the session's moves is asked for (moves passed on
	// are held from now until that catch-up is done).
	void Welcomed(std::uint32_t a_playerId);

	// Once a tick while in a session and let in: reports our player's moves, sends a catch-up's ask, and makes the moves
	// that waited for their holder.
	void Frame(const SendFn& a_send);

	// From the session: its moves (kRemoteLoot), the end of a catch-up (kLootCaughtUp), and the answers to our moves it
	// did not take (kLootAnswer).
	void Heard(const Protocol::LootBatch& a_batch);
	void CaughtUp(const Protocol::LootCaughtUp& a_msg);
	void Answered(const Protocol::LootBatch& a_batch);

	// A save was loaded (its co-save read first) or a new game started: the world's ledger is the save's (none for a new
	// game), and the moves it lacks are asked for again; in a session, moves passed on are held from now (the loading
	// screen still up), not from when the ask can go out.

	void AfterLoad();

	// The session ended. The ledger and the moves still waiting stay (they are this world's).
	void Clear();

	// The co-save: the ledger (the log's id and the first number this world lacks) and the moves still waiting for their
	// holder. Read before kPostLoadGame; a load whose save has none starts this world with no ledger.
	inline constexpr std::uint32_t COSAVE_LOOT = 0x544C4B53;  // "SKLT"
	inline constexpr std::uint32_t COSAVE_LOOT_VERSION = 1;
	void WriteCoSave(SKSE::SerializationInterface* a_intf);
	void ReadCoSave(SKSE::SerializationInterface* a_intf, std::uint32_t a_version, std::uint32_t a_length);
	void RevertCoSave();

	// The kind of an item as the loot rules judge it (kOther: nothing a player carries), and its name in the dev lines.
	// Quest rewards (QuestRewards.cpp) judge items by it too.
	[[nodiscard]] LootRules::ItemKind KindOf(const RE::TESForm* a_form);
	[[nodiscard]] const char*         KindName(LootRules::ItemKind a_kind);

	// Developer channel: `loot` (one line of counts), `loot near [radius]` (the holders near our player: containers and
	// bodies, and living actors as kind=npc), `loot inv <hex>` (what a reference holds, each item's plain count and
	// scope), `loot take <holder> <item> <n>` and `loot put <holder> <item> <n>` (our player takes or puts as the
	// container menu does), `loot find <book|key|weapon|misc>` (a playable item of that kind of the base game, for the
	// rig: both games find the same).
	[[nodiscard]] std::string Command(std::string_view a_args);
}
