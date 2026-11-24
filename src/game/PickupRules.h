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
		bool          attached = false;  // ... and that cell is attached (loaded around a player here)
		bool          disabled = false;
		bool          deleted = false;
		bool          harvested = false;
	};

	enum class Verdict : std::uint8_t
	{
		kApply,    // make it here now (take the reference out of the world, or harvest it)
		kAlready,  // so here already (deleted, or harvested): nothing to make
		kParked,   // taken, and its reference reads disabled here: out of sight now, but a disabled reference is no proof it
		           // was taken (an enable parent off, a quest's or a script's disable, our own making before): watched, and
		           // taken out when it reads enabled
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
		if (a_ref.deleted) {
			return { Verdict::kAlready, Why::kShared };
		}
		if (a_ref.disabled) {
			return { Verdict::kParked, Why::kShared };
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

	// The session's entries this game keeps watching, by reference: a newer word on one replaces the older.
	// - kWaiting: its reference is not in memory here (a cell not loaded): made when it loads.
	// - kParked: a taken reference that reads disabled here (Verdict::kParked): taken out if it ever reads enabled.
	// - kMade: a taken reference this game took out of its world: taken out again if something enables it again (its
	//   enable parent, a quest's or a script's Enable).
	// Tried again when the reference's cell attaches (Pickups.cpp), a few at a time in a sweep round the whole list
	// (Sweep), and often for those whose cell is loaded here (SweepLoaded: an enable there is undone within a moment). At
	// most MAX; past it a new one is refused (counted).
	class Waiting
	{
	public:
		static constexpr std::size_t MAX = Protocol::MAX_PICKUPS;

		enum class State : std::uint8_t
		{
			kWaiting,
			kParked,
			kMade,
		};

		// a_loaded: whether its reference's cell is attached here now (SweepLoaded goes round those).
		bool Add(const Pickup& a_entry, State a_state = State::kWaiting, bool a_loaded = false)
		{
			auto it = slots.find(a_entry.ref);
			if (it == slots.end()) {
				if (slots.size() >= MAX) {
					++refused;
					return false;
				}
				it = slots.emplace(a_entry.ref, Slot{ .entry = a_entry, .state = a_state, .gen = ++gens }).first;
				order.emplace_back(a_entry.ref, it->second.gen);
			} else {
				it->second.entry = a_entry;
				it->second.state = a_state;
			}
			SetLoaded(it->second, a_entry.ref, a_loaded);
			return true;
		}

		[[nodiscard]] const Pickup* Find(std::uint32_t a_ref) const
		{
			const auto it = slots.find(a_ref);
			return it == slots.end() ? nullptr : &it->second.entry;
		}

		[[nodiscard]] std::optional<State> StateOf(std::uint32_t a_ref) const
		{
			const auto it = slots.find(a_ref);
			return it == slots.end() ? std::nullopt : std::optional<State>{ it->second.state };
		}

		void Remove(std::uint32_t a_ref)
		{
			const auto it = slots.find(a_ref);
			if (it == slots.end()) {
				return;
			}
			if (it->second.loaded) {
				--loadedCount;
			}
			slots.erase(it);
		}

		// The next a_count entries, round the list (each once per call at most).
		std::vector<Pickup> Sweep(std::size_t a_count) { return Round(order, a_count, false); }

		// The next a_count entries whose cell is loaded here, round those (each once per call at most).
		std::vector<Pickup> SweepLoaded(std::size_t a_count) { return Round(loadedOrder, a_count, true); }

		[[nodiscard]] std::size_t Size() const { return slots.size(); }
		[[nodiscard]] std::size_t Count(State a_state) const
		{
			return static_cast<std::size_t>(std::ranges::count_if(slots, [&](const auto& a_slot) { return a_slot.second.state == a_state; }));
		}
		[[nodiscard]] std::size_t   Loaded() const { return loadedCount; }
		[[nodiscard]] std::uint64_t Refused() const { return refused; }

		void Clear()
		{
			slots.clear();
			order.clear();
			loadedOrder.clear();
			loadedCount = 0;
		}

	private:
		struct Slot
		{
			Pickup        entry{};
			State         state = State::kWaiting;
			std::uint64_t gen = 0;        // the slot's own: a place in order left by an older slot of the reference is passed over
			bool          loaded = false;
			std::uint64_t loadedGen = 0;  // ... and in loadedOrder, left from an earlier time its cell was loaded
		};
		using Place = std::pair<std::uint32_t, std::uint64_t>;  // reference, gen

		void SetLoaded(Slot& a_slot, std::uint32_t a_ref, bool a_loaded)
		{
			if (a_slot.loaded == a_loaded) {
				return;
			}
			a_slot.loaded = a_loaded;
			if (a_loaded) {
				++loadedCount;
				a_slot.loadedGen = ++gens;
				loadedOrder.emplace_back(a_ref, a_slot.loadedGen);
			} else {
				--loadedCount;
			}
		}

		// Goes round a_order: a place whose slot is gone, or is another's since (a removal, then a new word), or (a_loaded)
		// no longer loaded, is dropped as it comes up; each reference comes out once per call at most.
		std::vector<Pickup> Round(std::deque<Place>& a_order, std::size_t a_count, bool a_loaded)
		{
			std::vector<Pickup> out;
			std::size_t         looked = 0;
			const auto          limit = a_order.size();
			while (out.size() < a_count && looked < limit && !a_order.empty()) {
				const auto place = a_order.front();
				a_order.pop_front();
				++looked;
				const auto it = slots.find(place.first);
				if (it == slots.end()) {
					continue;
				}
				const auto& slot = it->second;
				if (a_loaded ? (!slot.loaded || slot.loadedGen != place.second) : slot.gen != place.second) {
					continue;
				}
				out.push_back(slot.entry);
				a_order.push_back(place);
			}
			return out;
		}

		std::unordered_map<std::uint32_t, Slot> slots;
		std::deque<Place>                       order;        // each slot once (and stale places, passed over)
		std::deque<Place>                       loadedOrder;  // each loaded slot once (and stale places)
		std::size_t                             loadedCount = 0;
		std::uint64_t                           gens = 0;
		std::uint64_t                           refused = 0;
	};

	// What this game keeps of an entry after its verdict (Pickups.cpp's Try): the state it is watched under, or none to
	// forget it. a_ours: our player took it too (ours: never taken out of our world again); a_was: the state it was watched
	// under before (a reference this game took out stays kMade while it reads disabled, or its cell is not loaded).
	[[nodiscard]] inline std::optional<Waiting::State> KeepAfter(Verdict a_verdict, std::uint8_t a_kind, bool a_ours,
		std::optional<Waiting::State> a_was)
	{
		using State = Waiting::State;
		switch (a_verdict) {
		case Verdict::kApply:
			return a_kind == Pickup::kTaken ? std::optional<State>{ State::kMade } : std::nullopt;
		case Verdict::kParked:
			if (a_ours) {
				return std::nullopt;
			}
			return a_was == State::kMade ? State::kMade : State::kParked;
		case Verdict::kWait:
			return a_was ? *a_was : State::kWaiting;
		case Verdict::kAlready:
		case Verdict::kHeld:
		case Verdict::kSkip:
			break;
		}
		return std::nullopt;
	}

	// The plants this game has seen near our player, in three states (a census of the plants near our player, and each
	// plant as its cell attaches): growing; harvested since it was seen growing and not reported yet; done (first seen
	// harvested, or reported). A scan for a harvest no activation explains reports a plant that reads harvested now and was
	// growing when last seen or harvested since: a harvest since. A later reading of it harvested never erases that (the
	// census runs every second, the scan only after the 2 s wait for an activation). Our own report and the session's
	// harvest make a plant done; a reading of it growing (it grew back) makes it growing again. A plant first seen harvested
	// (in the save from before the session, our own harvest from before the last load) is never reported by a scan. Past
	// MAX all are forgotten (counted): a plant not seen again is never reported by a scan (the safe side: each world keeps
	// its own).
	class Plants
	{
	public:
		static constexpr std::size_t MAX = 16384;

		enum class State : std::uint8_t
		{
			kGrowing,
			kUnreported,  // harvested since it was seen growing, not reported yet
			kDone         // first seen harvested, or reported
		};

		void Saw(std::uint32_t a_ref, bool a_harvested)
		{
			if (a_ref == 0) {
				return;
			}
			if (const auto it = seen.find(a_ref); it != seen.end()) {
				if (!a_harvested) {
					it->second = State::kGrowing;
				} else if (it->second == State::kGrowing) {
					it->second = State::kUnreported;
				}
				return;
			}
			Add(a_ref, a_harvested ? State::kDone : State::kGrowing);
		}

		// Our own report of its harvest, or the session's harvest made here: a scan never reports it again.
		void Reported(std::uint32_t a_ref)
		{
			if (a_ref == 0) {
				return;
			}
			if (const auto it = seen.find(a_ref); it != seen.end()) {
				it->second = State::kDone;
				return;
			}
			Add(a_ref, State::kDone);
		}

		// Whether a plant was seen growing and not reported since (one that reads harvested now was harvested since).
		[[nodiscard]] bool SeenGrowing(std::uint32_t a_ref) const
		{
			const auto it = seen.find(a_ref);
			return it != seen.end() && it->second != State::kDone;
		}

		[[nodiscard]] std::optional<State> StateOf(std::uint32_t a_ref) const
		{
			const auto it = seen.find(a_ref);
			if (it == seen.end()) {
				return std::nullopt;
			}
			return it->second;
		}

		[[nodiscard]] std::size_t   Size() const { return seen.size(); }
		[[nodiscard]] std::uint64_t Forgot() const { return forgot; }

		void Clear() { seen.clear(); }

	private:
		void Add(std::uint32_t a_ref, State a_state)
		{
			if (seen.size() >= MAX) {
				seen.clear();
				++forgot;
			}
			seen.emplace(a_ref, a_state);
		}

		std::unordered_map<std::uint32_t, State> seen;
		std::uint64_t                            forgot = 0;
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
