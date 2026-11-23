#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

// Doors, locks and mechanisms as one class (docs/DESIGN.md, "Doors, locks and mechanisms"): which of a reference's
// states a game shares, when it reports a change, what it does with the session's word, and when a friend's use of a
// mechanism is made here by their stand-in. Kept free of the game so every rule is tested (tests/RefRulesTests.cpp);
// src/game/WorldObjects.cpp reads the game and applies what these decide.
//
// Three things are shared, each by what the engine itself keeps, never by a script's name, so a mod's door, gate or
// lever is shared as the base game's are and the load order does not matter:
// - the open state of a door that opens and stays open (a plain door: not a load door, which takes the player to
//   another cell, nor an automatic one, which each game opens for whoever comes near);
// - the lock of everything that has one: every door (load doors too: a locked house), every chest; its level as well,
//   so a lock a script raises or a key-only lock is the same in each game;
// - the use of a mechanism: an activator the player can use (it has a name the game shows: "Pull Lever", "Pull Chain",
//   "Rotate Pillar", "Push Button", a claw door's keyhole). The game whose player used it says so; every other game has
//   that player's stand-in use it there, so the mechanism's own script runs once in each game, as it ran in the user's:
//   a gate, a portcullis, a drawbridge, a hidden door, a Dwemer mechanism or a puzzle pillar follows in every world.
//   What the mechanism then opens is shared as a door's state as well where it is one.
namespace RefRules
{
	using RefState = Protocol::RefState;
	using Clock = std::chrono::steady_clock;

	// What the game knows of a reference, read by WorldObjects.
	enum class Base : std::uint8_t
	{
		kOther,      // an actor, an item, furniture, a light, a talking activator, flora...: none of this class
		kDoor,       // TESObjectDOOR
		kActivator,  // TESObjectACTI (not a talking activator, not flora)
		kContainer,  // TESObjectCONT
	};

	struct Facts
	{
		Base base = Base::kOther;
		bool loadDoor = false;   // it takes whoever uses it to another cell (ExtraTeleport)
		bool automatic = false;  // a door that opens by itself as someone comes near (TESObjectDOOR::Flag::kAutomatic)
		bool named = false;      // its base has a name the game shows (an activator the player can use)
		bool marker = false;     // an editor marker: nothing anyone sees
	};

	// Which of a reference's states are shared.
	struct Shares
	{
		bool open = false;  // its open state
		bool lock = false;  // its lock and the lock's level
		bool use = false;   // a player's use of it is made in the other games by their stand-in
		[[nodiscard]] bool Any() const { return open || lock || use; }
		bool operator==(const Shares&) const = default;
	};

	constexpr Shares Of(const Facts& a_facts)
	{
		if (a_facts.marker) {
			return {};
		}
		switch (a_facts.base) {
		case Base::kDoor:
			return { .open = !a_facts.loadDoor && !a_facts.automatic, .lock = true };
		case Base::kContainer:
			return { .lock = true };
		case Base::kActivator:
			return { .use = a_facts.named };
		case Base::kOther:
			break;
		}
		return {};
	}

	// The kind's name, for the developer channel and the log.
	constexpr const char* KindName(const Facts& a_facts)
	{
		if (a_facts.marker) {
			return "other";
		}
		switch (a_facts.base) {
		case Base::kDoor:
			return a_facts.loadDoor ? "loaddoor" : a_facts.automatic ? "autodoor" : "door";
		case Base::kContainer:
			return "container";
		case Base::kActivator:
			return a_facts.named ? "mechanism" : "activator";
		case Base::kOther:
			break;
		}
		return "other";
	}

	// The game's open state (RE::BGSOpenCloseForm::OPEN_STATE: 1 open, 2 opening, 3 closed, 4 closing, 0 none) as the
	// wire has it: a door on its way is taken as where it is going.
	constexpr std::uint8_t OpenOf(int a_gameState)
	{
		switch (a_gameState) {
		case 1:
		case 2:
			return RefState::kYes;
		case 3:
		case 4:
			return RefState::kNo;
		default:
			return RefState::kUnknown;
		}
	}

	// The game's lock level (RE::LOCK_LEVEL: -1 unlocked, 0 very easy to 4 very hard, 5 needs a key) as the wire has it.
	constexpr std::uint8_t LevelOf(int a_gameLevel)
	{
		return a_gameLevel >= 0 && a_gameLevel <= RefState::LEVEL_MAX ? static_cast<std::uint8_t>(a_gameLevel) : RefState::LEVEL_UNKNOWN;
	}

	// A lock's own number (RE::REFR_LOCK::baseLevel, the record's value: 1 novice, 25 apprentice, 50 adept, 75 expert,
	// 100 master, 255 needs a key, which the signed byte reads as -1) as the wire's level. The game reads the level of a
	// lock that is locked itself (REFR_LOCK::GetLockLevel, which also scales a leveled lock); an unlocked one reads none
	// there, so its number is read here. A number between two levels is the lower one, as the game's table has it.
	constexpr std::uint8_t LevelOfBase(int a_base)
	{
		if (a_base == -1) {
			return 5;
		}
		if (a_base < 0) {
			return RefState::LEVEL_UNKNOWN;
		}
		return a_base >= 100 ? 4 : a_base >= 75 ? 3 : a_base >= 50 ? 2 : a_base >= 25 ? 1 : 0;
	}

	// The wire's level as a lock's own number, written with the lock's "leveled" flag cleared so every game reads the same
	// level whatever its player's level. LevelOfBase(BaseOfLevel(l)) == l for every level.
	constexpr std::int8_t BaseOfLevel(std::uint8_t a_level)
	{
		constexpr std::int8_t bases[] = { 1, 25, 50, 75, 100, -1 };
		return a_level <= RefState::LEVEL_MAX ? bases[a_level] : 1;
	}

	// The part of a state read here that is shared (Shares): the rest is not said.
	constexpr RefState Shared(RefState a_read, const Shares& a_shares)
	{
		if (!a_shares.open) {
			a_read.open = RefState::kUnknown;
		}
		if (!a_shares.lock) {
			a_read.locked = RefState::kUnknown;
			a_read.lockLevel = RefState::LEVEL_UNKNOWN;
		}
		return a_read;
	}

	// A change read here is reported when our player made it (their activation opened the door, they picked the lock)
	// or is within REPORT_REACH of the reference: a script, a trigger or an NPC near them did, and the friend near them
	// sees the same. Further off, a change is each game's own business (the friend is not there; their game runs its
	// own NPCs and scripts until those are shared too), and a report from there would only fight theirs.
	inline constexpr float REPORT_REACH = 2000.0f;

	constexpr bool Reports(bool a_byOurPlayer, float a_playerDistance)
	{
		return a_byOurPlayer || (a_playerDistance >= 0.0f && a_playerDistance <= REPORT_REACH);
	}

	// How far a friend's stand-in may stand from a mechanism to use it here. A player uses one from about 150 units; the
	// stand-in is drawn about 100 ms late (src/game/StateBuffer.h) and the friend may have walked on, so more is allowed,
	// but not a stand-in in another room (its use would be refused by the friend's world anyway).
	inline constexpr float USE_REACH = 1000.0f;

	enum class UseVerdict
	{
		kUse,           // the stand-in uses it here
		kNoStandIn,     // that player has no stand-in in this world (elsewhere, loading, not placed yet)
		kTooFar,        // their stand-in is further than USE_REACH from it
		kNotLoaded,     // this game holds no loaded reference of that id (another cell, or a plugin's this game lacks)
		kNotMechanism,  // it is no mechanism here (Of(...).use is false: a door, a chest, an unnamed activator)
	};

	struct UseFacts
	{
		bool  loaded = false;
		bool  mechanism = false;
		bool  standIn = false;
		float standInDistance = -1.0f;
	};

	constexpr UseVerdict JudgeUse(const UseFacts& a_facts)
	{
		if (!a_facts.loaded) {
			return UseVerdict::kNotLoaded;
		}
		if (!a_facts.mechanism) {
			return UseVerdict::kNotMechanism;
		}
		if (!a_facts.standIn) {
			return UseVerdict::kNoStandIn;
		}
		if (a_facts.standInDistance < 0.0f || a_facts.standInDistance > USE_REACH) {
			return UseVerdict::kTooFar;
		}
		return UseVerdict::kUse;
	}

	constexpr const char* UseVerdictName(UseVerdict a_verdict)
	{
		switch (a_verdict) {
		case UseVerdict::kUse:
			return "used";
		case UseVerdict::kNoStandIn:
			return "no stand-in";
		case UseVerdict::kTooFar:
			return "stand-in too far";
		case UseVerdict::kNotLoaded:
			return "not loaded";
		case UseVerdict::kNotMechanism:
			return "not a mechanism";
		}
		return "?";
	}

	// What to change on a reference to bring it to the session's word: only what the word says and the reference
	// reads otherwise (a part it reads none of, kUnknown, it lacks: a door with no lock is given none).
	struct Change
	{
		std::optional<bool>         open;
		std::optional<bool>         locked;
		std::optional<std::uint8_t> level;
		[[nodiscard]] bool Any() const { return open || locked || level; }
	};

	constexpr Change Plan(const RefState& a_want, const RefState& a_read)
	{
		Change out;
		if (a_want.open != RefState::kUnknown && a_read.open != RefState::kUnknown && a_want.open != a_read.open) {
			out.open = a_want.open == RefState::kYes;
		}
		if (a_want.locked != RefState::kUnknown && a_read.locked != RefState::kUnknown && a_want.locked != a_read.locked) {
			out.locked = a_want.locked == RefState::kYes;
		}
		if (a_want.lockLevel != RefState::LEVEL_UNKNOWN && a_read.lockLevel != RefState::LEVEL_UNKNOWN && a_want.lockLevel != a_read.lockLevel) {
			out.level = a_want.lockLevel;
		}
		return out;
	}

	// What this game knows the session says of each reference: what it reported and what it heard. Every word the
	// server passes on comes to every game, the reporter's own included (common/RefBook.h), so all of them end on the
	// last word the server heard. A game applies each word where its reference reads otherwise, except its own word
	// while it has a later report of the same reference still out (the word is old: applying it would undo the later
	// one for a moment). A change read here is news only where it differs from what the session says: the open and the
	// lock our own game set when it applied a friend's word read as they were said, and go nowhere.
	class Book
	{
	public:
		// We reported a_state: the session's word on it, until the server answers.
		void Sent(const RefState& a_state)
		{
			Merge(a_state);
			++outstanding[a_state.refId];
		}

		// The session's word came (a_ourId: our player id). Whether to bring the reference to it.
		bool Heard(const RefState& a_state, std::uint32_t a_ourId)
		{
			Merge(a_state);
			if (a_state.playerId == 0 || a_state.playerId != a_ourId) {
				return true;
			}
			const auto it = outstanding.find(a_state.refId);
			if (it == outstanding.end()) {
				return true;
			}
			if (--it->second == 0) {
				outstanding.erase(it);
				return true;
			}
			return false;
		}

		// Whether a state read here says something the session does not: a part it says that differs from the word
		// known, or a reference the session has said nothing of.
		[[nodiscard]] bool News(const RefState& a_read) const
		{
			if (!a_read.Says()) {
				return false;
			}
			const auto it = known.find(a_read.refId);
			if (it == known.end()) {
				return true;
			}
			const auto& k = it->second;
			return (a_read.open != RefState::kUnknown && a_read.open != k.open) || (a_read.locked != RefState::kUnknown && a_read.locked != k.locked) ||
			       (a_read.lockLevel != RefState::LEVEL_UNKNOWN && a_read.lockLevel != k.lockLevel);
		}

		[[nodiscard]] std::optional<RefState> Known(std::uint32_t a_refId) const
		{
			const auto it = known.find(a_refId);
			return it == known.end() ? std::nullopt : std::optional{ it->second };
		}

		[[nodiscard]] std::size_t Size() const { return known.size(); }
		[[nodiscard]] std::size_t Outstanding() const { return outstanding.size(); }

		void Clear()
		{
			known.clear();
			outstanding.clear();
		}

	private:
		void Merge(const RefState& a_state)
		{
			auto [it, added] = known.try_emplace(a_state.refId, a_state);
			if (!added) {
				it->second = Protocol::MergeRefState(it->second, a_state);
			}
		}

		std::unordered_map<std::uint32_t, RefState>      known;
		std::unordered_map<std::uint32_t, std::uint32_t> outstanding;  // our reports the server has not answered yet
	};

	// How often one reference is reported at most: BURST at once, then one every EVERY. A door a script flips over and
	// over (or two games' scripts undoing each other) would otherwise be reported each time it moves. A change that may
	// not go yet is held, and the latest state of it goes when it may (the sister project's DoorShare::Pace).
	inline constexpr std::uint32_t BURST = 8;
	inline constexpr auto          EVERY = std::chrono::seconds(2);

	class Pace
	{
	public:
		[[nodiscard]] bool May(Clock::time_point a_now) const { return due <= a_now + (BURST - 1) * EVERY; }
		void               Spend(Clock::time_point a_now) { due = (std::max)(due, a_now) + EVERY; }
		// Spent nothing lately: forgetting it changes nothing.
		[[nodiscard]] bool Rested(Clock::time_point a_now) const { return due <= a_now; }

	private:
		Clock::time_point due{};  // when every report spent would have come back
	};

	// The reports a game makes: sent now, or held while their reference's pace is spent, the newest read of each kept.
	class Outbox
	{
	public:
		// A change read now (news to the session): true to send it now (its pace spent), false: held for Due.
		bool Offer(const RefState& a_state, Clock::time_point a_now)
		{
			auto& pace = paces[a_state.refId];
			if (pace.May(a_now)) {
				pace.Spend(a_now);
				held.erase(a_state.refId);
				return true;
			}
			auto [it, added] = held.try_emplace(a_state.refId, a_state);
			if (!added) {
				it->second = Protocol::MergeRefState(it->second, a_state);
			}
			return false;
		}

		// The held reports whose pace has room now, each spent; references fully rested are forgotten.
		std::vector<RefState> Due(Clock::time_point a_now)
		{
			std::vector<RefState> out;
			for (auto it = held.begin(); it != held.end();) {
				auto& pace = paces[it->first];
				if (pace.May(a_now)) {
					pace.Spend(a_now);
					out.push_back(it->second);
					it = held.erase(it);
				} else {
					++it;
				}
			}
			std::erase_if(paces, [&](const auto& a_entry) { return a_entry.second.Rested(a_now) && !held.contains(a_entry.first); });
			return out;
		}

		[[nodiscard]] std::size_t Held() const { return held.size(); }

		void Clear()
		{
			held.clear();
			paces.clear();
		}

	private:
		std::unordered_map<std::uint32_t, Pace>     paces;
		std::unordered_map<std::uint32_t, RefState> held;
	};

	// The session's words this game could not apply yet: the reference is not loaded here (another cell, an interior we
	// are not in). Tried again while our player is in the world; the newest word of each kept, at most MAX_PENDING
	// references (past it a new one is dropped and counted: the session still holds it, and a load asks for it again).
	inline constexpr std::size_t MAX_PENDING = 4096;
	inline constexpr auto        RETRY_EVERY = std::chrono::milliseconds(500);

	class Pending
	{
	public:
		// False: dropped (full).
		bool Add(const RefState& a_state)
		{
			if (const auto it = waiting.find(a_state.refId); it != waiting.end()) {
				it->second = Protocol::MergeRefState(it->second, a_state);
				return true;
			}
			if (waiting.size() >= MAX_PENDING) {
				++dropped;
				return false;
			}
			waiting.emplace(a_state.refId, a_state);
			return true;
		}

		void Remove(std::uint32_t a_refId) { waiting.erase(a_refId); }

		[[nodiscard]] std::vector<RefState> All() const
		{
			std::vector<RefState> out;
			out.reserve(waiting.size());
			for (const auto& [id, state] : waiting) {
				out.push_back(state);
			}
			return out;
		}

		[[nodiscard]] std::size_t   Size() const { return waiting.size(); }
		[[nodiscard]] std::uint64_t Dropped() const { return dropped; }

		void Clear() { waiting.clear(); }

	private:
		std::unordered_map<std::uint32_t, RefState> waiting;
		std::uint64_t                               dropped = 0;
	};

	// Whether a reference can be shared at all: a placed one of a plugin (a runtime one, FF, no other game has).
	constexpr bool Shareable(std::uint32_t a_refId)
	{
		return a_refId != 0 && !FormIds::IsRuntime(a_refId);
	}
}
