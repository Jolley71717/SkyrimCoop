#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

// What the session says became of its world traps (docs/DESIGN.md, "Traps"): the server's book of the traps one game set
// off or disarmed (and the bear traps set again), kept for the session's run and sent to each game that joins, and again
// to one that loads a save. Free of the game and of the transports: tests/TrapTests.cpp.
//
// - A one-shot trap is spent once: the first player who reports it is kept, and later reports change nothing (two games
//   that set off one tripwire before either hears of the other have both spent it: the same end).
// - A bear trap keeps its last word of the other side: snapped (or disarmed) and set again take turns; a word of the side
//   the session holds is no news (Protocol::TrapSupersedes).
// - A reference keeps the kind it was first reported with: a report of another kind is the same (two games that read one
//   reference's scripts differently: the first said is kept).
// - Nobody is sent their own: their world spent the trap itself, and a save of theirs from before has it armed, theirs to
//   set off again (as with what a player takes: the sister project's lesson).
class TrapBook
{
public:
	enum class Take
	{
		kNews,     // kept, and new to the session: pass it on
		kSame,     // what the session holds already: nothing to pass on
		kRefused,  // no reference (0), a runtime one (FF: no other game has it), no base, or a word that can't be held
		kFull,     // a new reference, and the book holds its most already
	};

	explicit TrapBook(std::size_t a_max = Protocol::MAX_TRAPS) :
		max(a_max)
	{}

	// A player's report of an entry (its playerId is the server's: a_playerId). a_out: the entry as the session now holds
	// it, for kNews.
	Take Report(const Protocol::Trap& a_entry, std::uint32_t a_playerId, Protocol::Trap& a_out)
	{
		if (a_entry.ref == 0 || FormIds::IsRuntime(a_entry.ref) || a_entry.base == 0 || !Protocol::TrapWordFits(a_entry.kind, a_entry.phase)) {
			return Take::kRefused;
		}
		const auto it = known.find(a_entry.ref);
		if (it == known.end()) {
			if (known.size() >= max) {
				return Take::kFull;
			}
			Kept kept{ .entry = a_entry, .order = nextOrder++ };
			kept.entry.playerId = a_playerId;
			known.emplace(a_entry.ref, kept);
			a_out = kept.entry;
			return Take::kNews;
		}
		auto& kept = it->second;
		if (kept.entry.kind != a_entry.kind || !Protocol::TrapSupersedes(kept.entry.kind, a_entry.phase, kept.entry.phase)) {
			return Take::kSame;
		}
		kept.entry.phase = a_entry.phase;
		kept.entry.playerId = a_playerId;
		kept.order = nextOrder++;  // said again last: a joiner's list ends with the newest
		a_out = kept.entry;
		return Take::kNews;
	}

	// Every kept entry not a_playerId's own, in the order the session took them.
	[[nodiscard]] std::vector<Protocol::Trap> For(std::uint32_t a_playerId) const
	{
		std::vector<const Kept*> ordered;
		ordered.reserve(known.size());
		for (const auto& [ref, kept] : known) {
			if (kept.entry.playerId != a_playerId) {
				ordered.push_back(&kept);
			}
		}
		std::ranges::sort(ordered, {}, &Kept::order);
		std::vector<Protocol::Trap> out;
		out.reserve(ordered.size());
		for (const auto* kept : ordered) {
			out.push_back(kept->entry);
		}
		return out;
	}

	// The word kept for a_ref, if any.
	[[nodiscard]] std::optional<Protocol::Trap> Find(std::uint32_t a_ref) const
	{
		const auto it = known.find(a_ref);
		if (it == known.end()) {
			return std::nullopt;
		}
		return it->second.entry;
	}

	[[nodiscard]] std::size_t Size() const { return known.size(); }

	void Clear()
	{
		known.clear();
		nextOrder = 0;
	}

private:
	struct Kept
	{
		Protocol::Trap entry;
		std::uint64_t  order = 0;
	};

	std::size_t                             max;
	std::unordered_map<std::uint32_t, Kept> known;
	std::uint64_t                           nextOrder = 0;
};
