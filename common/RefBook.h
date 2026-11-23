#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// What the session says of each reference's open state and lock (docs/DESIGN.md, "Doors, locks and mechanisms"): the
// server's book, kept for the session's run and sent to each game that joins, and again to one that loads a save. The
// last word stands; the server numbers nothing, it passes each report on to every game in the order it came, the
// reporter's own included, so all of them end on the same word (two players at one door at once: whichever the server
// heard last). Free of the game and of the transports: tests/RefStateTests.cpp.
class RefBook
{
public:
	enum class Take
	{
		kNews,     // kept, and it changes what the session said: pass it on
		kSame,     // what the session said already: nothing to pass on
		kRefused,  // no reference (0), a runtime one (FF: no other game has it), or a report that says nothing
		kFull,     // a new reference, and the book holds Protocol::MAX_REF_STATES already
	};

	explicit RefBook(std::size_t a_max = Protocol::MAX_REF_STATES) :
		max(a_max)
	{}

	// A player's report: its parts laid over what the book holds (Protocol::MergeRefState). a_out is the whole state the
	// session now holds of the reference, the reporter's id on it, for kNews and kSame.
	Take Report(const Protocol::RefState& a_report, Protocol::RefState& a_out)
	{
		if (a_report.refId == 0 || FormIds::IsRuntime(a_report.refId) || !a_report.Says()) {
			return Take::kRefused;
		}
		const auto it = known.find(a_report.refId);
		if (it == known.end()) {
			if (known.size() >= max) {
				return Take::kFull;
			}
			a_out = a_report;
			known.emplace(a_report.refId, a_out);
			order.push_back(a_report.refId);
			return Take::kNews;
		}
		a_out = Protocol::MergeRefState(it->second, a_report);
		const bool same = a_out.SameState(it->second);
		it->second = a_out;
		return same ? Take::kSame : Take::kNews;
	}

	// Every kept state, in the order the references were first reported, each with player id 0: the session's word,
	// sent again.
	[[nodiscard]] std::vector<Protocol::RefState> All() const
	{
		std::vector<Protocol::RefState> out;
		out.reserve(order.size());
		for (const auto id : order) {
			auto state = known.at(id);
			state.playerId = 0;
			out.push_back(state);
		}
		return out;
	}

	[[nodiscard]] std::size_t Size() const { return known.size(); }

	void Clear()
	{
		known.clear();
		order.clear();
	}

private:
	std::size_t                                           max;
	std::unordered_map<std::uint32_t, Protocol::RefState> known;
	std::vector<std::uint32_t>                            order;
};
