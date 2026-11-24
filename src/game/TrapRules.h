#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

// World traps, as one class (docs/DESIGN.md, "Traps"): the rules, free of the game, so every one is tested
// (tests/TrapTests.cpp); src/game/Traps.cpp reads the game and makes what these decide.
//
// A trap is told by the Papyrus scripts bound to its reference, by script ancestry: a mod's trap whose script extends one
// of these is the same kind. What each kind's script does was read from the game's own compiled scripts:
// - Tripwire (and TrapTripwire...): armed in no state or "Inactive"; snapped it goes "Active" (activates what it is linked
//   to, plays its snap) and ends in "DoNothing" (its broken look).
// - TrapTriggerHinge (a trapped chest's or door's trigger): armed in "Active"; "Triggered" when it went off (sets off its
//   linked trap), "Disarmed" when a player disarmed it.
// - TrapRiggedRockfall: armed in "Idle"; falls once ("DoNothing" and its "break" animation).
// - TrapBreakAwayBase (a breakaway floor or bridge): armed in "UnBroken"; "Destroyed" once broken.
// - TrapBear: a toggle, "Open" (armed) and "Closed" (snapped, or disarmed by a player), "busy" between; a player sets a
//   closed one open again.
// - trapSoulGemController (a magic trap's soul gem): "waiting" or "inmagictrap" until taken, then "disarmed".
// Traps that set themselves again (blades, swinging walls, darts, flamethrowers, pressure plates, spring plates) stay each
// game's own: a friend's stand-in walking onto one sets it off in that game as a player does.
namespace TrapRules
{
	using Clock = std::chrono::steady_clock;
	using Kind = Protocol::TrapKind;
	using Trap = Protocol::Trap;

	// How long a trap must read one way before a change of it is news: a cell just loaded runs its scripts' OnLoad and
	// OnCellAttach, which put a trap in the state its save holds (a spent trap of the save reads spent, never news).
	inline constexpr auto SETTLE = std::chrono::seconds(2);
	// A trap this game made from the session's word is not told back for this long (its script runs on after the word).
	inline constexpr auto APPLY_QUIET = std::chrono::seconds(5);
	// How long after it is heard a word is set off the trap's own way (its sound, its fall, its snap): later (a catch-up,
	// one that waited for its cell) it is made spent quietly.
	inline constexpr auto FRESH = std::chrono::seconds(5);
	// Our player within this of a toggle (a bear trap) is the one who changed it: a change with nobody of ours near is a
	// friend's stand-in's or an NPC's, told by the game where it happened.
	inline constexpr float TOGGLE_REACH = 1024.0f;
	// Our player within this of a trap the session says went off just now sees and hears it go off here.
	inline constexpr float LIVE_REACH = 4000.0f;
	// How many times a word is made on a trap that still reads armed after APPLY_QUIET, before it is given up (counted).
	inline constexpr std::uint8_t MAX_ATTEMPTS = 3;

	// ---- telling a trap ----

	[[nodiscard]] constexpr char Lower(char a_c) { return a_c >= 'A' && a_c <= 'Z' ? static_cast<char>(a_c - 'A' + 'a') : a_c; }

	[[nodiscard]] constexpr bool Same(std::string_view a_a, std::string_view a_b)
	{
		if (a_a.size() != a_b.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a_a.size(); ++i) {
			if (Lower(a_a[i]) != Lower(a_b[i])) {
				return false;
			}
		}
		return true;
	}

	// The kind a script of this name makes a reference (none: not a trap script). Papyrus names are not case-sensitive.
	[[nodiscard]] constexpr Kind KindOfScript(std::string_view a_name)
	{
		if (Same(a_name, "Tripwire")) {
			return Kind::kTripwire;
		}
		if (Same(a_name, "TrapTriggerHinge")) {
			return Kind::kHinge;
		}
		if (Same(a_name, "TrapRiggedRockfall")) {
			return Kind::kRockfall;
		}
		if (Same(a_name, "TrapBreakAwayBase")) {
			return Kind::kBreakaway;
		}
		if (Same(a_name, "TrapBear")) {
			return Kind::kBearTrap;
		}
		if (Same(a_name, "trapSoulGemController")) {
			return Kind::kSoulGem;
		}
		return Kind::kNone;
	}

	// A script's chain, itself first, then each parent it extends: the first link that is a trap's script names the kind.
	[[nodiscard]] constexpr Kind Classify(const std::vector<std::string_view>& a_chain)
	{
		for (const auto name : a_chain) {
			if (const auto kind = KindOfScript(name); kind != Kind::kNone) {
				return kind;
			}
		}
		return Kind::kNone;
	}

	// ---- reading one ----

	// What a trap's script state says.
	enum class Look : std::uint8_t
	{
		kUnknown,   // a state its kind does not name (a mod's own, or a script not started): nothing read
		kArmed,     // set, waiting for a victim
		kGoing,     // going off now (a tripwire's "Active"): spent
		kFired,     // went off (a bear trap: closed)
		kDisarmed,  // a player disarmed it
		kBusy,      // between two looks (a bear trap's "busy"): read again
	};
	inline constexpr std::size_t LOOK_COUNT = 6;

	[[nodiscard]] constexpr const char* LookName(Look a_look)
	{
		switch (a_look) {
		case Look::kUnknown:
			return "unknown";
		case Look::kArmed:
			return "armed";
		case Look::kGoing:
			return "going";
		case Look::kFired:
			return "fired";
		case Look::kDisarmed:
			return "disarmed";
		case Look::kBusy:
			return "busy";
		}
		return "?";
	}

	[[nodiscard]] constexpr Look LookOf(Kind a_kind, std::string_view a_state)
	{
		switch (a_kind) {
		case Kind::kTripwire:
			if (a_state.empty() || Same(a_state, "Inactive")) {
				return Look::kArmed;
			}
			if (Same(a_state, "Active")) {
				return Look::kGoing;
			}
			if (Same(a_state, "DoNothing")) {
				return Look::kFired;
			}
			return Look::kUnknown;
		case Kind::kHinge:
			if (a_state.empty() || Same(a_state, "Active")) {
				return Look::kArmed;
			}
			if (Same(a_state, "Triggered")) {
				return Look::kFired;
			}
			if (Same(a_state, "Disarmed")) {
				return Look::kDisarmed;
			}
			return Look::kUnknown;
		case Kind::kRockfall:
			if (a_state.empty() || Same(a_state, "Idle")) {
				return Look::kArmed;
			}
			if (Same(a_state, "DoNothing")) {
				return Look::kFired;
			}
			return Look::kUnknown;
		case Kind::kBreakaway:
			if (a_state.empty() || Same(a_state, "UnBroken")) {
				return Look::kArmed;
			}
			if (Same(a_state, "Destroyed")) {
				return Look::kFired;
			}
			return Look::kUnknown;
		case Kind::kBearTrap:
			if (Same(a_state, "Open")) {
				return Look::kArmed;
			}
			if (Same(a_state, "Closed")) {
				return Look::kFired;
			}
			if (Same(a_state, "busy")) {
				return Look::kBusy;
			}
			return Look::kUnknown;  // no state yet: its OnLoad picks open or closed
		case Kind::kSoulGem:
			if (a_state.empty() || Same(a_state, "waiting") || Same(a_state, "inmagictrap")) {
				return Look::kArmed;
			}
			if (Same(a_state, "disarmed")) {
				return Look::kDisarmed;
			}
			return Look::kUnknown;
		case Kind::kNone:
			break;
		}
		return Look::kUnknown;
	}

	// Which side of a trap a look is: set (true), spent (false), or neither (none: read again).
	[[nodiscard]] constexpr std::optional<bool> Armed(Look a_look)
	{
		switch (a_look) {
		case Look::kArmed:
			return true;
		case Look::kGoing:
		case Look::kFired:
		case Look::kDisarmed:
			return false;
		case Look::kUnknown:
		case Look::kBusy:
			break;
		}
		return std::nullopt;
	}

	// The word a look says: kArmed, kFired (going or gone off) or kDisarmed. Only for a look with a side.
	[[nodiscard]] constexpr std::uint8_t PhaseOf(Look a_look)
	{
		switch (a_look) {
		case Look::kArmed:
			return Trap::kArmed;
		case Look::kDisarmed:
			return Trap::kDisarmed;
		default:
			return Trap::kFired;
		}
	}

	// ---- telling the session ----

	// The traps this game watches, each as it read (Traps.cpp reads them a few times a second while loaded near our
	// player). A change is told when the trap read the other way for SETTLE first: a one-shot trap set and then spent, a
	// bear trap snapped or set again with our player near. Never one this game made from a word within APPLY_QUIET.
	class Watch
	{
	public:
		static constexpr std::size_t MAX = 4096;

		// A read of a_ref, of a_kind, as a_look now, our player a_playerDistance from it (below 0: not in its cell or space).
		// The word to tell, if the read is news.
		std::optional<std::uint8_t> Read(std::uint32_t a_ref, Kind a_kind, Look a_look, float a_playerDistance, Clock::time_point a_now)
		{
			const auto side = Armed(a_look);
			auto       it = seen.find(a_ref);
			if (it == seen.end()) {
				if (seen.size() >= MAX) {
					++refused;
					return std::nullopt;
				}
				it = seen.emplace(a_ref, Seen{ .kind = a_kind, .firstAt = a_now, .since = a_now }).first;
			}
			auto& s = it->second;
			if (s.kind != a_kind) {  // read another way now (a script swapped): started again
				s = Seen{ .kind = a_kind, .firstAt = a_now, .since = a_now };
			}
			if (!side) {
				return std::nullopt;  // busy or unknown: read again
			}
			if (!s.side) {
				s.side = side;
				s.since = a_now;
				return std::nullopt;
			}
			if (*s.side == *side) {
				if (!s.settled || *s.settled != *side) {
					if (a_now - s.since >= SETTLE) {
						s.settled = side;
					}
				}
				return std::nullopt;
			}
			// It changed sides.
			const auto was = a_now - s.since >= SETTLE ? s.side : s.settled;
			s.side = side;
			s.since = a_now;
			s.settled.reset();
			if (!was || *was == *side) {
				return std::nullopt;  // never seen the other way long enough: a load's flicker
			}
			if (s.appliedAt && a_now - *s.appliedAt < APPLY_QUIET) {
				++quieted;
				return std::nullopt;  // ours, made from the session's word
			}
			if (Protocol::TrapToggles(a_kind)) {
				if (a_playerDistance < 0.0f || a_playerDistance > TOGGLE_REACH) {
					++farToggles;
					return std::nullopt;
				}
			} else if (*side) {
				return std::nullopt;  // a one-shot trap set again (a cell reset): each game's own
			}
			return PhaseOf(a_look);
		}

		// This game made the session's word on a_ref just now.
		void Applied(std::uint32_t a_ref, Clock::time_point a_now)
		{
			if (const auto it = seen.find(a_ref); it != seen.end()) {
				it->second.appliedAt = a_now;
			}
		}

		// Whether a_ref has been read for SETTLE (its cell's scripts have run): a word is made only on a settled trap.
		[[nodiscard]] bool Settled(std::uint32_t a_ref, Clock::time_point a_now) const
		{
			const auto it = seen.find(a_ref);
			return it != seen.end() && a_now - it->second.firstAt >= SETTLE;
		}

		// Whether a word was made on a_ref within APPLY_QUIET.
		[[nodiscard]] bool RecentlyApplied(std::uint32_t a_ref, Clock::time_point a_now) const
		{
			const auto it = seen.find(a_ref);
			return it != seen.end() && it->second.appliedAt && a_now - *it->second.appliedAt < APPLY_QUIET;
		}

		[[nodiscard]] bool Has(std::uint32_t a_ref) const { return seen.contains(a_ref); }

		// a_ref is no longer loaded here: read afresh when it is again.
		void Forget(std::uint32_t a_ref) { seen.erase(a_ref); }

		[[nodiscard]] std::size_t   Size() const { return seen.size(); }
		[[nodiscard]] std::uint64_t Refused() const { return refused; }
		[[nodiscard]] std::uint64_t Quieted() const { return quieted; }
		[[nodiscard]] std::uint64_t FarToggles() const { return farToggles; }

		void Clear() { seen.clear(); }

	private:
		struct Seen
		{
			Kind                             kind = Kind::kNone;
			Clock::time_point                firstAt{};
			std::optional<bool>              side;     // armed (true) or spent, as last read
			Clock::time_point                since{};  // ... since then
			std::optional<bool>              settled;  // the side it held for SETTLE, until it changes
			std::optional<Clock::time_point> appliedAt;
		};
		std::unordered_map<std::uint32_t, Seen> seen;
		std::uint64_t                           refused = 0;
		std::uint64_t                           quieted = 0;
		std::uint64_t                           farToggles = 0;
	};

	// Our words, paced: a batch at most every PACE. A newer word on a queued trap replaces it.
	class Outbox
	{
	public:
		static constexpr auto        PACE = std::chrono::milliseconds(250);
		static constexpr std::size_t MAX = 4096;

		void Add(const Trap& a_entry)
		{
			Trap out{ .ref = a_entry.ref, .base = a_entry.base, .kind = a_entry.kind, .phase = a_entry.phase };
			if (const auto it = std::ranges::find(queue, a_entry.ref, &Trap::ref); it != queue.end()) {
				*it = out;
				return;
			}
			if (queue.size() >= MAX) {
				queue.pop_front();
				++overflow;
			}
			queue.push_back(out);
		}

		std::optional<std::vector<Trap>> Due(Clock::time_point a_now)
		{
			if (queue.empty() || a_now < next) {
				return std::nullopt;
			}
			next = a_now + PACE;
			const auto        count = (std::min)(queue.size(), Protocol::MAX_TRAP_BATCH);
			std::vector<Trap> out(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(count));
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
		std::deque<Trap>  queue;
		Clock::time_point next{};
		std::uint64_t     overflow = 0;
	};

	// ---- making the session's word ----

	enum class Verdict : std::uint8_t
	{
		kNothing,    // it reads that way already (or a toggle's word it reads already): done
		kWait,       // not loaded, not settled, between looks, or just made: tried again later
		kSkip,       // another trap here (another base or kind), or a word its kind can't take: never made
		kFireLive,   // set off its own way: its snap, its fall, its sound (our player near, the word fresh)
		kFireQuiet,  // made spent with no show (a catch-up, a late word, our player far)
		kDisarm,     // made disarmed
		kRearm,      // a bear trap set open again
		kGiveUp,     // made MAX_ATTEMPTS times and still reads armed: left alone (counted)
	};
	inline constexpr std::size_t VERDICT_COUNT = 8;

	[[nodiscard]] constexpr const char* VerdictName(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kNothing:
			return "nothing";
		case Verdict::kWait:
			return "wait";
		case Verdict::kSkip:
			return "skip";
		case Verdict::kFireLive:
			return "live";
		case Verdict::kFireQuiet:
			return "quiet";
		case Verdict::kDisarm:
			return "disarm";
		case Verdict::kRearm:
			return "rearm";
		case Verdict::kGiveUp:
			return "giveup";
		}
		return "?";
	}

	// What this game knows of its own reference of a word.
	struct Facts
	{
		bool          loaded = false;    // in memory, its cell attached, its 3D loaded
		bool          settled = false;   // read for SETTLE (Watch::Settled)
		std::uint32_t base = 0;          // its base form
		Kind          kind = Kind::kNone;  // what its scripts make it
		Look          look = Look::kUnknown;
		bool          justMade = false;  // a word made on it within APPLY_QUIET (Watch::RecentlyApplied)
		std::uint8_t  attempts = 0;      // words made on it so far
		bool          catchUp = false;   // the word is a catch-up (kept by the session, not just said)
		bool          fresh = false;     // ... heard within FRESH
		float         playerDistance = -1.0f;  // our player from it (below 0: another cell or space)
	};

	[[nodiscard]] constexpr Verdict Judge(const Trap& a_word, const Facts& a_facts)
	{
		if (!Protocol::TrapWordFits(a_word.kind, a_word.phase)) {
			return Verdict::kSkip;
		}
		if (!a_facts.loaded) {
			return Verdict::kWait;
		}
		if (a_facts.base != a_word.base || a_facts.kind != a_word.kind) {
			return Verdict::kSkip;
		}
		if (!a_facts.settled || a_facts.justMade) {
			return Verdict::kWait;
		}
		const auto side = Armed(a_facts.look);
		if (!side) {
			return Verdict::kWait;
		}
		const bool wantArmed = a_word.phase == Trap::kArmed;
		if (*side == wantArmed) {
			return Verdict::kNothing;
		}
		if (a_facts.attempts >= MAX_ATTEMPTS) {
			return Verdict::kGiveUp;
		}
		if (wantArmed) {
			return Verdict::kRearm;
		}
		if (a_word.phase == Trap::kDisarmed) {
			return Verdict::kDisarm;
		}
		const bool inReach = a_facts.playerDistance >= 0.0f && a_facts.playerDistance <= LIVE_REACH;
		return !a_facts.catchUp && a_facts.fresh && inReach ? Verdict::kFireLive : Verdict::kFireQuiet;
	}

	// One step of making a word: a Papyrus call on the trap's own script (its state), an animation, the trap's own
	// OnTriggerEnter with itself (a breakaway's break), or a disable.
	struct Step
	{
		enum Op : std::uint8_t
		{
			kGoto,
			kAnim,
			kTriggerEnter,
			kDisable,
		};
		Op               op = kGoto;
		std::string_view arg;

		bool operator==(const Step&) const = default;
	};

	// The steps that make a_verdict on a trap of a_kind, in order (none: nothing to do, or a verdict that makes nothing).
	// Each kind's own script does the rest: a tripwire's "Active" activates what it is linked to, a hinge's "Triggered"
	// sets off its linked trap, a "DoNothing" or "Disarmed" sets the trap's broken look.
	[[nodiscard]] inline std::vector<Step> StepsFor(Kind a_kind, Verdict a_verdict)
	{
		const bool live = a_verdict == Verdict::kFireLive;
		const bool spend = a_verdict == Verdict::kFireLive || a_verdict == Verdict::kFireQuiet || a_verdict == Verdict::kDisarm;
		const bool rearm = a_verdict == Verdict::kRearm;
		if (!spend && !rearm) {
			return {};
		}
		switch (a_kind) {
		case Kind::kTripwire:
			if (spend) {
				return { live ? Step{ Step::kGoto, "Active" } : Step{ Step::kGoto, "DoNothing" } };
			}
			break;
		case Kind::kHinge:
			if (spend) {
				return { live ? Step{ Step::kGoto, "Triggered" } : Step{ Step::kGoto, "Disarmed" } };
			}
			break;
		case Kind::kRockfall:
			if (spend) {
				return { Step{ Step::kGoto, "DoNothing" }, Step{ Step::kAnim, "break" } };
			}
			break;
		case Kind::kBreakaway:
			if (spend) {
				if (live) {
					return { Step{ Step::kTriggerEnter, {} } };
				}
				return { Step{ Step::kGoto, "Destroyed" }, Step{ Step::kDisable, {} } };
			}
			break;
		case Kind::kBearTrap:
			if (spend) {
				return { Step{ Step::kAnim, "Trigger01" }, Step{ Step::kGoto, "Closed" } };
			}
			return { Step{ Step::kAnim, "Reset01" }, Step{ Step::kGoto, "Open" } };
		case Kind::kSoulGem:
			if (spend) {
				return { Step{ Step::kGoto, "disarmed" }, Step{ Step::kDisable, {} } };
			}
			break;
		case Kind::kNone:
			break;
		}
		return {};
	}

	// The session's words this game has not made yet (or made, and checks again after APPLY_QUIET), by reference: a newer
	// word on one replaces the older. Tried each time the trap is read here (loaded near our player); one never loaded
	// here waits for the session's run. At most MAX; past it a new one is refused (counted).
	class Waiting
	{
	public:
		static constexpr std::size_t MAX = Protocol::MAX_TRAPS;

		struct Entry
		{
			Trap              word;
			bool              catchUp = false;
			Clock::time_point heardAt{};
			std::uint8_t      attempts = 0;
		};

		bool Add(const Trap& a_word, bool a_catchUp, Clock::time_point a_heardAt)
		{
			const Entry entry{ .word = a_word, .catchUp = a_catchUp, .heardAt = a_heardAt };
			if (const auto it = entries.find(a_word.ref); it != entries.end()) {
				it->second = entry;  // a newer word: tried afresh
				return true;
			}
			if (entries.size() >= MAX) {
				++refused;
				return false;
			}
			entries.emplace(a_word.ref, entry);
			return true;
		}

		[[nodiscard]] Entry* Find(std::uint32_t a_ref)
		{
			const auto it = entries.find(a_ref);
			return it != entries.end() ? &it->second : nullptr;
		}

		void Remove(std::uint32_t a_ref) { entries.erase(a_ref); }

		[[nodiscard]] std::size_t   Size() const { return entries.size(); }
		[[nodiscard]] std::uint64_t Refused() const { return refused; }

		void Clear() { entries.clear(); }

	private:
		std::unordered_map<std::uint32_t, Entry> entries;
		std::uint64_t                            refused = 0;
	};
}
