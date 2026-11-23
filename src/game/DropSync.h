#pragma once

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace SKSE
{
	class SerializationInterface;
}

// Items players give each other by dropping them (docs/DESIGN.md, "Dropped items"; the rules, free of the game, are
// src/game/DropRules.h, the server's book common/DropStore.h). Our player drops an item (the inventory's Drop): once it
// comes to rest it is reported (kReportDrop); the server offers it back to us, and our game takes our item out of our
// world and shows the session's copy in its place (kClaimDrop kPlaced), so the item is in no world twice. Every game
// shows a copy of each item lying in the session near its player, blocked from activation: our player's Take on a copy
// is a claim (kClaimDrop kTake), and only the player the server grants it to picks it up (kDropGone names them). A take
// that can't be finished goes back to the session (kGiveBack).
//
// Copies are never saved: they are taken out before a save (and named in the co-save, so a load removes any a save kept),
// and placed again after. The co-save holds this world's record of the session's drops (what left it, what it took in),
// sent to the server after every welcome and load, so a load is made to agree with the session.
//
// As our player leaves the session, our drops still lying, and our drops the server had not confirmed yet, go back to our
// world (where they lay, or at our player's feet): the server withdraws them for everyone as we leave.
//
// From the tick (GameThread), and from SKSE's messages and co-save callbacks: every entry point takes one lock over
// DropSync's state, never while sending. The event sinks never take it: they queue.
namespace DropSync
{
	using SendFn = std::function<void(std::vector<std::uint8_t>)>;

	// At kDataLoaded: the event sinks (our player's drops, our player's activations). False when the game's event source
	// is not there.
	bool Install();

	// The session let us in as a_playerId, in session a_sessionId (the server's run): this world's record of it is sent.
	void Welcomed(std::uint32_t a_playerId, std::uint64_t a_sessionId);

	// Once a tick while in a session and let in: reports our drops that came to rest, claims the copies our player took,
	// finishes our grants, places and removes the copies near our player, sends the record when it is due.
	void Frame(const SendFn& a_send);

	// Every tick, in a session or not: the copies on their way out, and our items going back to our world after a session.
	void Idle();

	// From the session: an item offered to us or lying there (kDropPlaced), one that went (kDropGone), the list of what
	// lies there (kDropList).
	void Placed(const Protocol::DroppedItem& a_drop);
	void Gone(const Protocol::DropGone& a_gone);
	void Listed(const Protocol::DropList& a_list);

	// kSaveGame: the copies are taken out of the world (named in the co-save), placed again after.
	void BeforeSave();

	// A save was loaded (its co-save read first) or a new game started: the copies of the world before are forgotten,
	// those the save kept removed, the record is the save's (none for a new game), and it is sent again in a session.
	void AfterLoad();
	void NewGame();

	// The session ended: our items lying there go back to our world, the others' copies go.
	void Clear();

	// The co-save: the record, and the copies taken out for the save (to be removed should a load bring any back).
	inline constexpr std::uint32_t COSAVE_DROPS = 0x52444B53;  // "SKDR"
	inline constexpr std::uint32_t COSAVE_DROPS_VERSION = 1;
	void WriteCoSave(SKSE::SerializationInterface* a_intf);
	void ReadCoSave(SKSE::SerializationInterface* a_intf, std::uint32_t a_version, std::uint32_t a_length);
	void RevertCoSave();

	// Developer channel: `drops` (one line of counts), `drops list` (what lies in the session, each with its copy here),
	// `drops drop <item hex> <n>` (our player drops n of a plain stack, as the inventory's Drop does), `drops take <drop
	// id>` (our player takes the copy, as Take on it does), `drops count <item hex>` (how many our player has). No names.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
