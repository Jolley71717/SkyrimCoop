#pragma once

#include "FormIds.h"
#include "Protocol.h"
#include "game/LootRules.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// What a player takes from the world, as one class (docs/DESIGN.md, "Taken from the world"): the rules, free of the game,
// so every one is tested (tests/PickupTests.cpp); src/game/Pickups.cpp reads the game and makes what these decide.
//
// The class is judged by what the engine keeps, never by a record's or a script's name, so a mod's item or plant is
// shared as the base game's are:
// - every loose item reference of the load order our player picks up (TESContainerChangedEvent from nothing to our player,
//   naming the reference): a weapon, armor, ammo, a potion or food, an ingredient, misc (gold, gems, ore, clutter), a
//   soul gem, a scroll, a torch, an apparatus. In every other game that reference goes out of the world (disabled, never
//   deleted: a load of a save brings back what its plugin placed).
// - every plant and tree our player harvests (a flora or a tree with something to harvest): harvested in every game, and
//   growing back there on that game's own schedule; a harvest older than that game's respawn time is not made.
// What stays each player's own: keys and books (each player reads their own, as with loot), a reference a quest alias
// holds (judged by each game on its own reference), runtime references (FF: placed by one game as it ran, a dropped
// item's copy). Ore veins keep their count in their script: each player's own (no script variable is written by name).
namespace PickupRules
{
	using Clock = std::chrono::steady_clock;
	using Pickup = Protocol::Pickup;
	using ItemKind = LootRules::ItemKind;

	// Why an entry is or isn't the session's (counted by name in `pickups`).
	enum class Why : std::uint8_t
	{
		kShared,
		kRuntime,    // a runtime reference (FF)
		kNotItem,    // its base is no inventory item (taken) or no plant (harvested)
		kKey,
		kBook,
		kQuest,      // a quest alias holds it here
		kOtherBase,  // the reference here is of another base (a mod that replaced it, an ID used again)
		kRegrown,    // a harvest older than this game's respawn time: grown back here by now
	};
	inline constexpr std::size_t WHY_COUNT = static_cast<std::size_t>(Why::kRegrown) + 1;

	[[nodiscard]] constexpr const char* WhyName(Why a_why)
	{
		switch (a_why) {
		case Why::kShared:
			return "shared";
		case Why::kRuntime:
			return "runtime";
		case Why::kNotItem:
			return "notItem";
		case Why::kKey:
			return "key";
		case Why::kBook:
			return "book";
		case Why::kQuest:
			return "quest";
		case Why::kOtherBase:
			return "otherBase";
		case Why::kRegrown:
			return "regrown";
		}
		return "?";
	}

	// Whether our player's pickup of a reference of this item kind is the session's (the sender's judge; the receiver
	// judges again on its own reference, with its quest aliases).
	[[nodiscard]] constexpr Why Takeable(ItemKind a_kind, bool a_runtimeRef, bool a_questAlias)
	{
		if (a_runtimeRef) {
			return Why::kRuntime;
		}
		switch (a_kind) {
		case ItemKind::kOther:
			return Why::kNotItem;
		case ItemKind::kKey:
			return Why::kKey;
		case ItemKind::kBook:
			return Why::kBook;
		default:
			break;
		}
		return a_questAlias ? Why::kQuest : Why::kShared;
	}

	// Whether our player's harvest of a reference is the session's.
	[[nodiscard]] constexpr Why Harvestable(bool a_plant, bool a_runtimeRef, bool a_questAlias)
	{
		if (a_runtimeRef) {
			return Why::kRuntime;
		}
		if (!a_plant) {
			return Why::kNotItem;
		}
		return a_questAlias ? Why::kQuest : Why::kShared;
	}

	// What a game knows of its own reference of an entry, read by Pickups.cpp.
	struct Facts
	{
		bool          found = false;     // in memory here (a reference of a cell not loaded is not)
		std::uint32_t base = 0;          // its base form
		ItemKind      item = ItemKind::kOther;  // its base as an inventory item (kOther: none)
		bool          plant = false;     // its base is a flora, or a tree with something to harvest
		bool          questAlias = false;  // a quest alias holds it here
		bool          inCell = false;    // in a cell (not in an inventory: a persistent item picked up stays itself there)
		bool          disabled = false;
		bool          deleted = false;
		bool          harvested = false;
	};

	enum class Verdict : std::uint8_t
	{
		kApply,    // make it here now (take the reference out of the world, or harvest it)
		kAlready,  // so here already (out of the world, or harvested): nothing to make
		kHeld,     // in an inventory here (a persistent item someone here picked up): left there
		kWait,     // not in memory here: made when it loads
		kSkip,     // not this game's to make (why says why)
	};
	struct Judged
	{
		Verdict verdict = Verdict::kSkip;
		Why     why = Why::kShared;
		bool operator==(const Judged&) const = default;
	};

	// The receiver's verdict on an entry it heard, by its own reference. a_respawnHours: this game's iHoursToRespawnCell
	// (0 or less: never regrown).
	[[nodiscard]] inline Judged Receive(const Pickup& a_entry, const Facts& a_ref, float a_respawnHours)
	{
		if (FormIds::IsRuntime(a_entry.ref)) {
			return { Verdict::kSkip, Why::kRuntime };
		}
		if (!a_ref.found) {
			return { Verdict::kWait, Why::kShared };
		}
		if (a_ref.base != a_entry.base) {
			return { Verdict::kSkip, Why::kOtherBase };
		}
		if (a_entry.kind == Pickup::kHarvested) {
			if (const auto why = Harvestable(a_ref.plant, false, a_ref.questAlias); why != Why::kShared) {
				return { Verdict::kSkip, why };
			}
			if (a_entry.ageHours >= 0.0f && a_respawnHours > 0.0f && a_entry.ageHours >= a_respawnHours) {
				return { Verdict::kSkip, Why::kRegrown };
			}
			if (a_ref.deleted || a_ref.harvested) {
				return { Verdict::kAlready, Why::kShared };
			}
			return { Verdict::kApply, Why::kShared };
		}
		if (const auto why = Takeable(a_ref.item, false, a_ref.questAlias); why != Why::kShared) {
			return { Verdict::kSkip, why };
		}
		if (a_ref.deleted || a_ref.disabled) {
			return { Verdict::kAlready, Why::kShared };
		}
		if (!a_ref.inCell) {
			return { Verdict::kHeld, Why::kShared };
		}
		return { Verdict::kApply, Why::kShared };
	}

	// Our player's harvests, read from what the engine shows: an activation of a plant by our player (its harvested flag as
	// the event came), and the engine's word that our player harvested an item (TESHarvestedEvent::ItemHarvested: the
	// produce, no reference).
	// - A plant not harvested when our player activated it is watched for WATCH: it is ours once it reads harvested (the
	//   event came before the harvest; a harvest that gave nothing counts too).
	// - A plant already harvested as the event came (the event came after the harvest) is ours when a harvest of its
	//   produce comes within PAIR of it (activating a plant harvested before gives nothing, so no harvest comes).
	// - A harvest no activation explains within PAIR (a mod that harvests by script or by walking past) asks for a scan:
	//   the plants near our player of that produce that read harvested and that the session was not told of yet.
	class Harvests
	{
	public:
		static constexpr auto        WATCH = std::chrono::seconds(3);
		static constexpr auto        PAIR = std::chrono::seconds(2);
		static constexpr std::size_t MAX_WATCHED = 64;
		static constexpr std::size_t MAX_EVENTS = 64;

		// Our player activated a plant: its reference, its produce (0: any, for a plant whose produce is a leveled list: the
		// engine's harvest may name the item it gave), and whether it read harvested as the event came.
		void Activated(std::uint32_t a_ref, std::uint32_t a_produce, bool a_harvested, Clock::time_point a_now)
		{
			if (a_ref == 0) {
				return;
			}
			std::erase_if(watched, [&](const Watch& a_watch) { return a_watch.ref == a_ref; });
			if (watched.size() >= MAX_WATCHED) {
				watched.pop_front();
			}
			watched.push_back({ a_ref, a_produce, a_now, a_harvested });
		}

		// The engine says our player harvested a_produce.
		void Harvested(std::uint32_t a_produce, Clock::time_point a_now)
		{
			if (events.size() >= MAX_EVENTS) {
				events.pop_front();
			}
			events.push_back({ a_produce, a_now });
		}

		struct Due
		{
			std::vector<std::uint32_t> refs;   // harvested by our player: report each
			std::vector<std::uint32_t> scans;  // harvests no activation explains: scan near our player for each produce
		};

		// Once a tick. a_read: whether a reference reads harvested now (none: it is gone).
		Due Tend(Clock::time_point a_now, const std::function<std::optional<bool>(std::uint32_t)>& a_read)
		{
			Due out;
			for (auto it = watched.begin(); it != watched.end();) {
				const auto now = a_read(it->ref);
				bool       done = !now.has_value();
				if (!done && !it->harvestedAtEvent && *now) {
					out.refs.push_back(it->ref);  // it turned harvested after our activation
					TakeEvent(it->produce, it->at);
					done = true;
				} else if (!done && it->harvestedAtEvent && TakeEvent(it->produce, it->at)) {
					out.refs.push_back(it->ref);  // harvested as our activation's event came, and a harvest of it came
					done = true;
				}
				if (done || a_now - it->at > WATCH) {
					it = watched.erase(it);
				} else {
					++it;
				}
			}
			// A harvest no activation took within PAIR: a scan for its produce (once a produce a tick).
			for (auto it = events.begin(); it != events.end();) {
				if (a_now - it->at <= PAIR) {
					++it;
					continue;
				}
				if (std::ranges::find(out.scans, it->produce) == out.scans.end()) {
					out.scans.push_back(it->produce);
				}
				it = events.erase(it);
			}
			return out;
		}

		[[nodiscard]] std::size_t Watched() const { return watched.size(); }

		void Clear()
		{
			watched.clear();
			events.clear();
		}

	private:
		struct Watch
		{
			std::uint32_t     ref = 0;
			std::uint32_t     produce = 0;
			Clock::time_point at{};
			bool              harvestedAtEvent = false;
		};
		struct Event
		{
			std::uint32_t     produce = 0;
			Clock::time_point at{};
		};

		// Takes the first harvest of a_produce within PAIR of a_at, if there is one.
		bool TakeEvent(std::uint32_t a_produce, Clock::time_point a_at)
		{
			const auto it = std::ranges::find_if(events, [&](const Event& a_event) {
				const auto apart = a_event.at > a_at ? a_event.at - a_at : a_at - a_event.at;
				return (a_produce == 0 || a_event.produce == a_produce) && apart <= PAIR;
			});
			if (it == events.end()) {
				return false;
			}
			events.erase(it);
			return true;
		}

		std::deque<Watch> watched;
		std::deque<Event> events;
	};

	// Our entries waiting to go: one of each reference and kind, sent in batches of at most MAX_PICKUP_BATCH, a batch every
	// PACE (four a second, half the server's budget: a game never goes past it). Past MAX queued the oldest goes (counted).
	class Outbox
	{
	public:
		static constexpr auto        PACE = std::chrono::milliseconds(250);
		static constexpr std::size_t MAX = 4096;

		// False when it was queued already (the same reference and kind).
		bool Add(const Pickup& a_entry)
		{
			if (std::ranges::any_of(queue, [&](const Pickup& a_queued) { return a_queued.ref == a_entry.ref && a_queued.kind == a_entry.kind; })) {
				return false;
			}
			if (queue.size() >= MAX) {
				queue.pop_front();
				++overflow;
			}
			Pickup out{ .ref = a_entry.ref, .base = a_entry.base, .kind = a_entry.kind };
			queue.push_back(out);
			return true;
		}

		// The next batch, when one is due.
		std::optional<std::vector<Pickup>> Due(Clock::time_point a_now)
		{
			if (queue.empty() || a_now < next) {
				return std::nullopt;
			}
			next = a_now + PACE;
			const auto           count = (std::min)(queue.size(), Protocol::MAX_PICKUP_BATCH);
			std::vector<Pickup>  out(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(count));
			queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(count));
			return out;
		}

		[[nodiscard]] std::size_t   Size() const { return queue.size(); }
		[[nodiscard]] std::uint64_t Overflow() const { return overflow; }

		void Clear()
		{
			queue.clear();
			next = {};
		}

	private:
		std::deque<Pickup> queue;
		Clock::time_point  next{};
		std::uint64_t      overflow = 0;
	};

	// The session's entries this game could not make yet (their reference not in memory here: a cell not loaded), by
	// reference: a newer word on one replaces the older. Tried again when the reference's cell attaches (Pickups.cpp) and
	// a few at a time in a sweep (Sweep: round the whole list, a_count at a time). At most MAX; past it a new one is
	// refused (counted).
	class Waiting
	{
	public:
		static constexpr std::size_t MAX = Protocol::MAX_PICKUPS;

		bool Add(const Pickup& a_entry)
		{
			if (const auto it = known.find(a_entry.ref); it != known.end()) {
				it->second = a_entry;
				return true;
			}
			if (known.size() >= MAX) {
				++refused;
				return false;
			}
			known.emplace(a_entry.ref, a_entry);
			order.push_back(a_entry.ref);
			return true;
		}

		[[nodiscard]] const Pickup* Find(std::uint32_t a_ref) const
		{
			const auto it = known.find(a_ref);
			return it == known.end() ? nullptr : &it->second;
		}

		void Remove(std::uint32_t a_ref) { known.erase(a_ref); }

		// The next a_count waiting entries, round the list (each once per call at most).
		std::vector<Pickup> Sweep(std::size_t a_count)
		{
			std::vector<Pickup> out;
			std::size_t         looked = 0;
			const auto          limit = order.size();
			while (out.size() < a_count && looked < limit && !order.empty()) {
				const auto ref = order.front();
				order.pop_front();
				++looked;
				if (const auto it = known.find(ref); it != known.end()) {
					out.push_back(it->second);
					order.push_back(ref);
				}
			}
			return out;
		}

		[[nodiscard]] std::size_t   Size() const { return known.size(); }
		[[nodiscard]] std::uint64_t Refused() const { return refused; }

		void Clear()
		{
			known.clear();
			order.clear();
		}

	private:
		std::unordered_map<std::uint32_t, Pickup> known;
		std::deque<std::uint32_t>                 order;  // may hold references removed since: passed over by Sweep
		std::uint64_t                             refused = 0;
	};

	// The references this game told the session of or made from its word since the welcome or the load: a scan for an
	// unexplained harvest passes them over (it would say again what the session holds), and a friend's word on one our
	// player took too is counted as both having taken it.
	class Known
	{
	public:
		static constexpr std::size_t MAX = 2 * Protocol::MAX_PICKUPS;

		void Ours(std::uint32_t a_ref) { Note(a_ref, true); }
		void Theirs(std::uint32_t a_ref) { Note(a_ref, false); }
		[[nodiscard]] bool Has(std::uint32_t a_ref) const { return ours.contains(a_ref) || theirs.contains(a_ref); }
		[[nodiscard]] bool IsOurs(std::uint32_t a_ref) const { return ours.contains(a_ref); }
		[[nodiscard]] std::size_t Size() const { return ours.size() + theirs.size(); }

		void Clear()
		{
			ours.clear();
			theirs.clear();
		}

	private:
		void Note(std::uint32_t a_ref, bool a_ours)
		{
			if (Size() >= MAX) {
				return;  // a session past the server's book: the scan may say one again (the server keeps one)
			}
			(a_ours ? ours : theirs).insert(a_ref);
		}

		std::unordered_set<std::uint32_t> ours;
		std::unordered_set<std::uint32_t> theirs;
	};
}
