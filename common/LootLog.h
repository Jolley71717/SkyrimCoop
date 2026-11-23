#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// The session's moves of loot (docs/DESIGN.md, "Containers and loot"): the server's numbered log, kept for the
// session's run. Every move a game reports that the log takes gets the next number and goes to every game, the
// reporter's own included, in that order; a game keeps the number of the first move its world does not hold yet (its
// ledger, kept in its co-save), and after a load or a join asks for the moves from there. A new session (its first player
// in) starts the log again under a new id, so a ledger of another session's log is answered from the first move.
// Free of the game and of the transports: tests/LootTests.cpp.
class LootLog
{
public:
	enum class Take
	{
		kNews,     // numbered and kept: pass it on to every game
		kFull,     // the log holds its most (Protocol::MAX_LOOT_LOG): not kept, not passed on
		kRefused,  // no holder (0), a runtime holder or item (FF: no other game has it), or a move no game could make
	};

	explicit LootLog(std::size_t a_max = Protocol::MAX_LOOT_LOG) :
		max(a_max)
	{}

	// A player's move: a_out is it numbered, with the player's id, for kNews.
	Take Report(const Protocol::LootEntry& a_move, std::uint32_t a_playerId, Protocol::LootEntry& a_out)
	{
		if (!Takes(a_move)) {
			return Take::kRefused;
		}
		if (moves.size() >= max) {
			return Take::kFull;
		}
		a_out = a_move;
		a_out.number = next++;
		a_out.playerId = a_playerId;
		a_out.answer = 0;
		moves.push_back(a_out);
		return Take::kNews;
	}

	// Whether the log would number a_move (its room aside).
	[[nodiscard]] static bool Takes(const Protocol::LootEntry& a_move)
	{
		if (a_move.holderId == 0 || FormIds::IsRuntime(a_move.holderId) || !Protocol::LootMoveReadable(a_move)) {
			return false;
		}
		return a_move.kind == Protocol::LootEntry::kEmptied || !FormIds::IsRuntime(a_move.itemId);
	}

	// The moves numbered a_from and later, in order (all of them for a_from 0 or 1).
	[[nodiscard]] std::vector<Protocol::LootEntry> From(std::uint32_t a_from) const
	{
		const auto first = std::ranges::lower_bound(moves, a_from, {}, &Protocol::LootEntry::number);
		return std::vector<Protocol::LootEntry>(first, moves.end());
	}

	// Where a catch-up asked by a game with ledger (a_logId, a_from) starts: its number when the ledger is of this log
	// and not past its end, else the first move (another session's ledger, none, or one this log never reached).
	[[nodiscard]] std::uint32_t StartFor(std::uint64_t a_logId, std::uint32_t a_from) const
	{
		return a_logId == id && a_from >= 1 && a_from <= next ? a_from : 1;
	}

	// A new session: no moves, numbers from 1 again, under a_id (never 0: a game's ledger of no log is 0).
	void Restart(std::uint64_t a_id)
	{
		moves.clear();
		next = 1;
		id = a_id != 0 ? a_id : 1;
	}

	[[nodiscard]] std::uint64_t Id() const { return id; }
	[[nodiscard]] std::uint32_t Next() const { return next; }
	[[nodiscard]] std::size_t   Size() const { return moves.size(); }

private:
	std::size_t                     max;
	std::vector<Protocol::LootEntry> moves;  // numbered 1 up, in order
	std::uint32_t                   next = 1;
	std::uint64_t                   id = 1;
};
