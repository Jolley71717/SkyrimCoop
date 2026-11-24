#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

// What the session says players took from the world (docs/DESIGN.md, "Taken from the world"): the server's book of the
// loose items picked up and the plants and trees harvested, kept for the session's run and sent to each game that joins,
// and again to one that loads a save. Free of the game and of the transports: tests/PickupTests.cpp.
//
// - A loose item is taken once: the first player who reports it is kept as its picker, and later reports change nothing
//   (two players who pick up one item before either hears of the other both have it: rare, and said by their games).
// - A plant is harvested again once it has grown back somewhere: a harvest reported at a later day of the session's clock
//   than the kept one (or with a clock where the kept one had none) replaces it, harvester and day.
// - A reference keeps the kind it was first reported with: a report of another kind is the same (a mod's plant that is
//   also an item reads as whichever came first).
// - Nobody is sent their own: their world has the item (in an inventory, or where they dropped it), and a save of theirs
//   from before they took it has it where it lay, theirs to take again. The same holds for a harvest.
class PickupBook
{
public:
	enum class Take
	{
		kNews,     // kept, and new to the session: pass it on
		kSame,     // what the session holds already: nothing to pass on
		kRefused,  // no reference (0), a runtime one (FF: no other game has it), no base, or a kind not known
		kFull,     // a new reference, and the book holds Protocol::MAX_PICKUPS already
	};

	// A day of the session's clock a harvest is said again past: an hour later (the same harvest said twice is not news).
	static constexpr double AGAIN_DAYS = 1.0 / 24.0;

	explicit PickupBook(std::size_t a_max = Protocol::MAX_PICKUPS) :
		max(a_max)
	{}

	// A player's report of an entry (its playerId and age are the server's: a_playerId, and a_day, the session clock's
	// GameDaysPassed now, none when the session has no clock). a_out: the entry as the session now holds it, for kNews.
	Take Report(const Protocol::Pickup& a_entry, std::uint32_t a_playerId, std::optional<double> a_day, Protocol::Pickup& a_out)
	{
		if (a_entry.ref == 0 || FormIds::IsRuntime(a_entry.ref) || a_entry.base == 0 || a_entry.kind > Protocol::Pickup::KIND_MAX) {
			return Take::kRefused;
		}
		if (a_day && !std::isfinite(*a_day)) {
			a_day.reset();
		}
		const auto it = known.find(a_entry.ref);
		if (it == known.end()) {
			if (known.size() >= max) {
				return Take::kFull;
			}
			Kept kept{ .ref = a_entry.ref, .base = a_entry.base, .kind = a_entry.kind, .playerId = a_playerId, .day = a_day, .order = nextOrder++ };
			known.emplace(a_entry.ref, kept);
			a_out = Out(kept, a_day);
			return Take::kNews;
		}
		auto& kept = it->second;
		if (kept.kind != Protocol::Pickup::kHarvested || a_entry.kind != Protocol::Pickup::kHarvested) {
			return Take::kSame;  // taken once (the first picker kept), or a report of another kind
		}
		const bool later = a_day && (!kept.day || *a_day >= *kept.day + AGAIN_DAYS);
		if (!later) {
			return Take::kSame;
		}
		kept.playerId = a_playerId;
		kept.day = a_day;
		kept.order = nextOrder++;  // said again last: a joiner's list ends with the newest
		a_out = Out(kept, a_day);
		return Take::kNews;
	}

	// Every kept entry not a_playerId's own, in the order the session took them, each aged by a_day (the session clock's
	// day now): its age in game hours, AGE_UNKNOWN when either day is not known, 0 when the clock went back (a load).
	[[nodiscard]] std::vector<Protocol::Pickup> For(std::uint32_t a_playerId, std::optional<double> a_day) const
	{
		std::vector<const Kept*> ordered;
		ordered.reserve(known.size());
		for (const auto& [ref, kept] : known) {
			if (kept.playerId != a_playerId) {
				ordered.push_back(&kept);
			}
		}
		std::ranges::sort(ordered, {}, &Kept::order);
		std::vector<Protocol::Pickup> out;
		out.reserve(ordered.size());
		for (const auto* kept : ordered) {
			out.push_back(Out(*kept, a_day));
		}
		return out;
	}

	[[nodiscard]] bool Has(std::uint32_t a_ref) const { return known.contains(a_ref); }
	[[nodiscard]] std::size_t Size() const { return known.size(); }

	void Clear()
	{
		known.clear();
		nextOrder = 0;
	}

private:
	struct Kept
	{
		std::uint32_t         ref = 0;
		std::uint32_t         base = 0;
		std::uint8_t          kind = Protocol::Pickup::kTaken;
		std::uint32_t         playerId = 0;
		std::optional<double> day;  // the session clock's GameDaysPassed when it was taken; none: no clock then
		std::uint64_t         order = 0;
	};

	static Protocol::Pickup Out(const Kept& a_kept, std::optional<double> a_day)
	{
		Protocol::Pickup out{ .ref = a_kept.ref, .base = a_kept.base, .kind = a_kept.kind, .playerId = a_kept.playerId };
		if (a_kept.kind == Protocol::Pickup::kHarvested && a_kept.day && a_day && std::isfinite(*a_day)) {
			const auto hours = (*a_day - *a_kept.day) * 24.0;
			out.ageHours = static_cast<float>(std::clamp(hours, 0.0, static_cast<double>(Protocol::MAX_PICKUP_AGE_HOURS)));
		}
		return out;
	}

	std::size_t                             max;
	std::unordered_map<std::uint32_t, Kept> known;
	std::uint64_t                           nextOrder = 0;
};
