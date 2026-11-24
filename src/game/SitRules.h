#pragma once

#include "Protocol.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

// Furniture, the rules (docs/DESIGN.md, "Furniture"; the game's side is src/game/Furniture.cpp, the server's
// common/SitBook.h). A game says who of its own is in what furniture: its player, and each NPC it runs, in a chair, a
// bench, a bed, a bedroll, at a railing, at a crafting station or a work spot, a dragon on its perch: every kind of
// furniture, every kind of actor, one way. Every other game puts the actor that shows them (a friend's stand-in, a copy
// of the NPC) in its own copy of that furniture through the engine's own way in, so the engine plays the sitting down,
// the idle and the getting up, and the furniture's marker places it:
// - Tell: what our player or an NPC we run is in, from its sit/sleep state (in it or getting in; getting up is out of it
//   already, so the others' copy rises as it starts to) and the furniture its AI process names: the reference when it
//   is of the load order, its base and where it stands always (a runtime one, a bedroll put down, is found by those).
// - Pick: the furniture here: the reference named when this game has it (and it is of that base), else the nearest of
//   that base within MATCH_RADIUS of where it stands in the sender's world.
// - Seat: rounds of ways in (the furniture activated by the actor, the console's activate 2 s later), each waiting for
//   the actor's and the furniture's 3D; while the engine gets it in, its own update runs for the actor (Assist: a
//   stand-in's or a copy's AI never runs otherwise, and the engine's sit procedure is the AI's), at most ENTRY_MAX; one
//   that stalls getting in is taken out (quick) and the rounds go on; 3 rounds 5 s apart, then given up. Out of it (its
//   friend got up, or it is in other furniture): first the engine's own getting up (want-to-stand or want-to-wake, its
//   update assisting), then, if it is not up after STAND_WAIT, at once (the engine's quick stop).
// - Held: an actor in furniture, getting in or out, or near (HOLD_RADIUS) the furniture its friend is in, is not placed
//   by its friend's states (they would stand it on the chair), nor told its walk: it stays where it stood as they sat
//   down when no way in takes. Which actor is held or assisted is kept per sit (Marks), so none stays so once it goes.
// - Out: the engine's getting up is started by the actor's sit state, written in its own update (StandState), never
//   from the tick: the word it is in holds the actor's life, knock and fly states, which its update writes too.
// - Events: a graph event an actor's graph takes while the engine has it in furniture, getting in or out (and not on a
//   mount, whose sit state is the same) is the engine's own: it is never sent, as every game plays it itself.
// No game here: tests/SitTests.cpp.
namespace SitRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// The engine's sit and sleep states (RE::SIT_SLEEP_STATE).
	enum class SitState : std::uint32_t
	{
		kNormal = 0,
		kWantToSit = 1,
		kWaitingForSitAnim = 2,
		kIsSitting = 3,  // a rider on a mount too
		kWantToStand = 4,
		kWantToSleep = 5,
		kWaitingForSleepAnim = 6,
		kIsSleeping = 7,
		kWantToWake = 8,
	};
	inline constexpr std::uint32_t SIT_STATE_COUNT = 9;

	[[nodiscard]] constexpr bool GettingIn(SitState a_state)
	{
		using enum SitState;
		return a_state == kWantToSit || a_state == kWaitingForSitAnim || a_state == kWantToSleep || a_state == kWaitingForSleepAnim;
	}
	[[nodiscard]] constexpr bool Settled(SitState a_state) { return a_state == SitState::kIsSitting || a_state == SitState::kIsSleeping; }
	[[nodiscard]] constexpr bool GettingUp(SitState a_state) { return a_state == SitState::kWantToStand || a_state == SitState::kWantToWake; }
	// In furniture as the others are told: in it, or on the way in.
	[[nodiscard]] constexpr bool InFurniture(SitState a_state) { return GettingIn(a_state) || Settled(a_state); }
	[[nodiscard]] constexpr bool Sleeping(SitState a_state)
	{
		using enum SitState;
		return a_state == kWantToSleep || a_state == kWaitingForSleepAnim || a_state == kIsSleeping || a_state == kWantToWake;
	}
	[[nodiscard]] constexpr SitState StateOf(std::uint32_t a_raw) { return a_raw < SIT_STATE_COUNT ? static_cast<SitState>(a_raw) : SitState::kNormal; }

	// Whether a graph event this actor's graph takes now is the engine's own furniture play (never sent: every game plays
	// it as it puts its own copy in): in furniture, getting in or out, and not riding (a rider's sit state is kIsSitting
	// too; its mounted actions are sent, its mount events never are: NpcRules::MountEvent).
	[[nodiscard]] constexpr bool EngineOwnsEvents(SitState a_state, bool a_onMount, bool a_gettingOnOrOffMount)
	{
		return a_state != SitState::kNormal && !a_onMount && !a_gettingOnOrOffMount;
	}

	// The furniture record's flags the kind is read from (RE::TESFurniture::ActiveMarker).
	inline constexpr std::uint32_t kIsPerch = 1u << 26;
	inline constexpr std::uint32_t kCanLean = 1u << 29;
	inline constexpr std::uint32_t kCanSit = 1u << 30;
	inline constexpr std::uint32_t kCanSleep = 1u << 31;

	// What it is used for: a sleep state is a bed's; then a perch, a crafting station (its bench type), a seat, a bed, a
	// railing; anything else is a work spot (a chopping block, an ore vein).
	[[nodiscard]] constexpr Protocol::SitKind KindOf(SitState a_state, std::uint32_t a_flags, std::uint8_t a_benchType)
	{
		using K = Protocol::SitKind;
		if (Sleeping(a_state)) {
			return K::kSleep;
		}
		if ((a_flags & kIsPerch) != 0) {
			return K::kPerch;
		}
		if (a_benchType != 0) {
			return K::kWork;
		}
		if ((a_flags & kCanSit) != 0) {
			return K::kSit;
		}
		if ((a_flags & kCanSleep) != 0) {
			return K::kSleep;
		}
		if ((a_flags & kCanLean) != 0) {
			return K::kLean;
		}
		return K::kWork;
	}

	[[nodiscard]] constexpr std::string_view KindName(Protocol::SitKind a_kind)
	{
		switch (a_kind) {
		case Protocol::SitKind::kSit:
			return "sit";
		case Protocol::SitKind::kSleep:
			return "sleep";
		case Protocol::SitKind::kLean:
			return "lean";
		case Protocol::SitKind::kWork:
			return "work";
		case Protocol::SitKind::kPerch:
			return "perch";
		}
		return "?";
	}

	// Our player, or an NPC we run, as the game reads it.
	struct Own
	{
		SitState      state = SitState::kNormal;
		bool          onMount = false;    // on a mount (its sit state is the saddle's): riding, never furniture
		bool          found = false;      // its AI process names furniture it occupies
		bool          furniture = false;  // that reference's base is furniture (not an actor, not anything else)
		std::uint32_t ref = 0;
		bool          runtimeRef = false;  // made as the game ran: no other game has that reference
		std::uint32_t base = 0;
		float         x = 0, y = 0, z = 0;  // where the furniture stands
		std::uint32_t marker = Protocol::NO_MARKER;  // the marker it uses
		std::uint32_t flags = 0;                     // the furniture record's
		std::uint8_t  benchType = 0;
	};

	// What to say of it: none when it is in no furniture (or getting up, or riding); the caller names the player or NPC.
	[[nodiscard]] inline std::optional<Protocol::Sit> Tell(const Own& a_own)
	{
		if (a_own.onMount || !InFurniture(a_own.state) || !a_own.found || !a_own.furniture || a_own.base == 0) {
			return std::nullopt;
		}
		if (!Protocol::InWorld(a_own.x) || !Protocol::InWorld(a_own.y) || !Protocol::InWorld(a_own.z)) {
			return std::nullopt;
		}
		Protocol::Sit sit;
		sit.furniture = a_own.runtimeRef ? 0 : a_own.ref;
		sit.base = a_own.base;
		sit.x = std::round(a_own.x);
		sit.y = std::round(a_own.y);
		sit.z = std::round(a_own.z);
		sit.marker = a_own.marker < Protocol::NO_MARKER ? static_cast<std::uint8_t>(a_own.marker) : Protocol::NO_MARKER;
		sit.kind = KindOf(a_own.state, a_own.flags, a_own.benchType);
		sit.on = true;
		return sit;
	}

	// Furniture made in their game (a bedroll put down, a mod's at run time), or one numbered otherwise here, is found by
	// its base within this of where they said it stands (a shared drop lies where it was dropped, to the unit).
	inline constexpr float MATCH_RADIUS = 48.0f;

	// A reference here that may be the one.
	struct Candidate
	{
		std::uint32_t id = 0;
		std::uint32_t base = 0;
		float         x = 0, y = 0, z = 0;
		bool          usable = false;  // furniture, enabled, not deleted
	};

	// Which candidate is the furniture: the reference they named when this game has it (of that base, when the base is
	// known here), else the nearest of that base within MATCH_RADIUS of where they said it stands. None: shown standing.
	[[nodiscard]] inline std::optional<std::uint32_t> Pick(const Protocol::Sit& a_sit, std::span<const Candidate> a_candidates)
	{
		if (a_sit.furniture != 0) {
			for (const auto& candidate : a_candidates) {
				if (candidate.id == a_sit.furniture && candidate.usable && (a_sit.base == 0 || candidate.base == a_sit.base)) {
					return candidate.id;
				}
			}
		}
		if (a_sit.base == 0) {
			return std::nullopt;
		}
		std::optional<std::uint32_t> best;
		float                        bestDistance = MATCH_RADIUS;
		for (const auto& candidate : a_candidates) {
			if (candidate.base != a_sit.base || !candidate.usable) {
				continue;
			}
			const float distance = std::hypot(candidate.x - a_sit.x, candidate.y - a_sit.y, candidate.z - a_sit.z);
			if (distance <= bestDistance) {
				best = candidate.id;
				bestDistance = distance;
			}
		}
		return best;
	}

	// A furniture our own player is in, at the marker they said (or one we can't tell apart: an unknown marker, or a
	// furniture with one): theirs is not put in it here (the engine would refuse, or put it on our lap); it is held beside.
	[[nodiscard]] constexpr bool TakenByUs(bool a_oursIsIt, std::uint8_t a_saidMarker, std::uint32_t a_ourMarker, std::uint32_t a_markers)
	{
		return a_oursIsIt && (a_saidMarker == Protocol::NO_MARKER || a_ourMarker == a_saidMarker || a_markers <= 1);
	}

	// The actor is held (not placed by its friend's or runner's states) within this of the furniture they are in. A long
	// bench's or a bed's far end is some way off its reference. (Not NEAR: windows.h makes NEAR, near, FAR and far empty.)
	inline constexpr float HOLD_RADIUS = 256.0f;

	// Furniture of that record near where they said is looked for (a walk of the cell's references) at most this often
	// for one actor: the reference they named, or the one found before while it is usable there, needs no walk.
	inline constexpr auto RESCAN_EVERY = 1s;
	[[nodiscard]] inline bool ScanDue(const std::optional<Clock::time_point>& a_last, Clock::time_point a_now)
	{
		return !a_last || a_now - *a_last >= RESCAN_EVERY;
	}

	// A crafting station: furniture with a bench type, which opens a crafting menu (a forge, a grindstone, a workbench, an
	// enchanter, an alchemy lab, a cooking pot, a tanning rack, a smelter). A work spot with none (a chopping block, an ore
	// vein) needs a tool a player may not carry: the rig's player legs take stations only (`sit find station`).
	[[nodiscard]] constexpr bool IsStation(std::uint8_t a_benchType) { return a_benchType != 0; }

	// The sit state the engine's own getting up is started with, from in it: want-to-wake from a bed, want-to-stand from
	// the rest; none when it is not settled in it (getting in, getting up already, or up). Written in the actor's own
	// update, on the thread that updates it (the same word holds its life, knock and fly states).
	[[nodiscard]] constexpr std::optional<SitState> StandState(SitState a_state)
	{
		if (a_state == SitState::kIsSleeping) {
			return SitState::kWantToWake;
		}
		if (a_state == SitState::kIsSitting) {
			return SitState::kWantToStand;
		}
		return std::nullopt;
	}

	// Which actor a sit has marked held or assisted in the update hooks' sets, so none stays marked: an actor that
	// changes (a stand-in made again, a copy now another's) or goes (its friend left, its runner went, the NPC is ours)
	// is unmarked as it does, and a sit with no actor here holds and assists none (so an ended one is let go of).
	class Marks
	{
	public:
		struct Change
		{
			std::uint32_t id = 0;
			bool          held = false;
			bool          assisted = false;
			bool          dropped = false;  // no longer this sit's actor: anything else waiting for it (a stand) goes too
			auto operator<=>(const Change&) const = default;
		};

		// This tick: the actor that shows it here (0: none) and whether it is held and assisted. Returns what the sets are
		// told: the old actor's unmarking first, then the actor's own marks.
		std::vector<Change> Set(std::uint32_t a_id, bool a_held, bool a_assisted)
		{
			std::vector<Change> out;
			if (id != 0 && id != a_id) {
				out.push_back({ .id = id, .dropped = true });
			}
			id = a_id;
			holding = a_id != 0 && a_held;
			assisting = a_id != 0 && a_assisted;
			if (a_id != 0) {
				out.push_back({ .id = a_id, .held = holding, .assisted = assisting });
			}
			return out;
		}
		// The sit is forgotten (it ended, or its NPC is ours now): its actor unmarked.
		std::optional<Change> Drop()
		{
			if (id == 0) {
				return std::nullopt;
			}
			const Change out{ .id = id, .dropped = true };
			*this = {};
			return out;
		}

		[[nodiscard]] std::uint32_t Id() const { return id; }
		[[nodiscard]] bool          Holding() const { return holding; }
		[[nodiscard]] bool          Assisting() const { return assisting; }

	private:
		std::uint32_t id = 0;
		bool          holding = false;
		bool          assisting = false;
	};

	// Not placed by its states, nor told its walk: in furniture or getting in or out as the engine has it, or near the
	// furniture its friend is said to be in (waiting for a way in, or none took: it stays where it stood as they sat).
	[[nodiscard]] constexpr bool Held(bool a_saidIn, bool a_near, SitState a_state, bool a_occupies)
	{
		return a_state != SitState::kNormal || a_occupies || (a_saidIn && a_near);
	}

	// A round starts once the actor and the furniture have had their 3D this long (a way in on a skeleton still loading
	// does not hold).
	inline constexpr auto READY_AFTER = 1s;
	// In a round: the console's activate this long after the first try, and the round over this long after it began.
	inline constexpr auto CONSOLE_AFTER = 2s;
	inline constexpr auto ROUND_LENGTH = 8s;
	inline constexpr auto ROUND_GAP = 5s;
	inline constexpr int  MAX_ROUNDS = 3;
	// The engine's own update runs for an actor getting in at most this long; one still getting in after it is stalled.
	inline constexpr auto ENTRY_MAX = 6s;
	// The engine's own getting up (its update running) is given this long before the actor is taken out at once.
	inline constexpr auto STAND_WAIT = 3s;
	// An actor is taken out at once no more often than this.
	inline constexpr auto QUICK_EVERY = 1s;

	// What the engine says of one actor this game shows (a stand-in, a copy).
	struct SeatFacts
	{
		bool     wanted = false;    // its friend or runner is in furniture, and it is found here and free for it (Pick)
		bool     ready = false;     // actor and furniture are here with their 3D for READY_AFTER, and the actor near it
		SitState state = SitState::kNormal;  // its sit/sleep state
		bool     inWanted = false;  // its AI process names that furniture
		bool     inOther = false;   // its AI process names another (or any, while none is wanted)
	};

	enum class Act : std::uint8_t
	{
		kNone,
		kActivate,  // the furniture activated by the actor (TESObjectREFR::ActivateRef)
		kConsole,   // the console's `<furniture>.activate <actor>`
		kStand,     // the engine's own getting up: its sit state set to want-to-stand (or want-to-wake)
		kQuick,     // out at once (Actor::StopInteractingQuick)
	};

	[[nodiscard]] constexpr std::string_view ActName(Act a_act)
	{
		switch (a_act) {
		case Act::kNone:
			return "none";
		case Act::kActivate:
			return "activate";
		case Act::kConsole:
			return "console";
		case Act::kStand:
			return "stand";
		case Act::kQuick:
			return "quick";
		}
		return "?";
	}

	// One actor's seat: what to do each tick to have it where its friend or runner is.
	class Seat
	{
	public:
		Act Step(const SeatFacts& a_facts, Clock::time_point a_now)
		{
			assist = false;
			const bool occupies = a_facts.inWanted || a_facts.inOther;
			seated = a_facts.wanted && a_facts.inWanted && Settled(a_facts.state);
			// Out: it is in other furniture, or in any while none is wanted (its friend got up).
			if (a_facts.inOther || (!a_facts.wanted && (occupies || a_facts.state != SitState::kNormal))) {
				ResetRounds();
				enterSince.reset();
				return Out(a_facts.state, a_now);
			}
			outSince.reset();
			standTried = false;
			if (!a_facts.wanted) {
				ResetRounds();
				enterSince.reset();
				givenUp = false;
				rounds = 0;
				return Act::kNone;
			}
			if (seated) {
				if (!seatedSince) {
					seatedSince = a_now;
					seatedBy = tried;
					++seats;
				}
				ResetRounds();
				enterSince.reset();
				rounds = 0;
				givenUp = false;
				return Act::kNone;
			}
			seatedSince.reset();
			// The engine is getting it in: its own update runs for it (Assist), for at most ENTRY_MAX; past that it is
			// stalled, taken out, and the rounds go on.
			if (a_facts.inWanted && GettingIn(a_facts.state)) {
				enterSince = enterSince.value_or(a_now);
				if (a_now - *enterSince < ENTRY_MAX) {
					assist = true;
					return Act::kNone;
				}
				enterSince.reset();
				++stalls;
				return Quick(a_now);
			}
			enterSince.reset();
			// Getting up by itself while it should stay: wait for it to be up, then the rounds put it back.
			if (GettingUp(a_facts.state)) {
				return Act::kNone;
			}
			if (!a_facts.ready || givenUp) {
				return Act::kNone;
			}
			if (!roundStart) {
				if (nextRound && a_now < *nextRound) {
					return Act::kNone;
				}
				roundStart = a_now;
				step = 1;
				return Tried(Act::kActivate);
			}
			const auto in = a_now - *roundStart;
			if (step == 1 && in >= CONSOLE_AFTER) {
				step = 2;
				return Tried(Act::kConsole);
			}
			if (in >= ROUND_LENGTH) {
				roundStart.reset();
				step = 0;
				++rounds;
				if (rounds >= MAX_ROUNDS) {
					givenUp = true;
				} else {
					nextRound = a_now + ROUND_GAP;
				}
			}
			return Act::kNone;
		}

		[[nodiscard]] bool Seated() const { return seated; }
		// Whether the engine's own update runs for the actor this tick (getting in or getting up).
		[[nodiscard]] bool Assist() const { return assist; }
		[[nodiscard]] bool GivenUp() const { return givenUp; }
		[[nodiscard]] int  Rounds() const { return rounds; }
		[[nodiscard]] Act  LastTried() const { return tried; }
		[[nodiscard]] Act  SeatedBy() const { return seatedBy; }  // the last way tried before it was seen in (none: it was in already)
		[[nodiscard]] std::uint32_t Seats() const { return seats; }
		[[nodiscard]] std::uint32_t Stands() const { return stands; }
		[[nodiscard]] std::uint32_t Quicks() const { return quicks; }
		[[nodiscard]] std::uint32_t Stalls() const { return stalls; }

	private:
		// Out of furniture: the engine's own getting up first (from in it), its update running while it gets up, for
		// STAND_WAIT; then at once.
		Act Out(SitState a_state, Clock::time_point a_now)
		{
			outSince = outSince.value_or(a_now);
			if (!standTried && (Settled(a_state) || GettingUp(a_state))) {
				standTried = true;
				outSince = a_now;
				if (GettingUp(a_state)) {
					assist = true;  // up already on its way: the engine's getting up goes on
					return Act::kNone;
				}
				++stands;
				assist = true;  // the stand is written in its own update (the caller's), which the engine runs in full
				return Act::kStand;
			}
			if (standTried && a_now - *outSince < STAND_WAIT && (GettingUp(a_state) || Settled(a_state))) {
				assist = GettingUp(a_state);
				return Act::kNone;
			}
			return Quick(a_now);
		}

		Act Quick(Clock::time_point a_now)
		{
			if (lastQuick && a_now - *lastQuick < QUICK_EVERY) {
				return Act::kNone;
			}
			lastQuick = a_now;
			++quicks;
			return Act::kQuick;
		}

		Act Tried(Act a_act)
		{
			tried = a_act;
			return a_act;
		}
		void ResetRounds()
		{
			roundStart.reset();
			nextRound.reset();
			step = 0;
		}

		bool                             seated = false;
		bool                             assist = false;
		bool                             givenUp = false;
		bool                             standTried = false;
		int                              rounds = 0;
		int                              step = 0;
		std::optional<Clock::time_point> roundStart;
		std::optional<Clock::time_point> nextRound;
		std::optional<Clock::time_point> enterSince;
		std::optional<Clock::time_point> outSince;
		std::optional<Clock::time_point> lastQuick;
		std::optional<Clock::time_point> seatedSince;
		Act                              tried = Act::kNone;
		Act                              seatedBy = Act::kNone;
		std::uint32_t                    seats = 0, stands = 0, quicks = 0, stalls = 0;
	};

	// Who of ours is in what, as this game said it last: each change is said once (an end too, of one it said).
	class Said
	{
	public:
		struct Rider
		{
			std::uint32_t    player = 0;
			Protocol::NpcKey npc;
			auto operator<=>(const Rider&) const = default;
		};

		// a_now: every actor of ours in furniture now, and what it is in (Tell's; the player and rider fields are set
		// here). Returns the sits to send: each that began or changed, and the end of each said before and not in now.
		std::vector<Protocol::Sit> Changes(const std::map<Rider, Protocol::Sit>& a_now)
		{
			std::vector<Protocol::Sit> out;
			for (const auto& [rider, now] : a_now) {
				auto sit = now;
				sit.player = rider.player;
				sit.rider = rider.player != 0 ? Protocol::NpcKey{} : rider.npc;
				sit.on = true;
				const auto said = sits.find(rider);
				if (said != sits.end() && said->second == sit) {
					continue;
				}
				sits[rider] = sit;
				out.push_back(sit);
			}
			// Handed to us in furniture another game said it was in, and out of it here: its end, said once.
			for (const auto& rider : handed) {
				if (!sits.contains(rider)) {
					out.push_back(End(rider));
				}
			}
			handed.clear();
			for (auto it = sits.begin(); it != sits.end();) {
				if (!a_now.contains(it->first)) {
					out.push_back(End(it->first));
					it = sits.erase(it);
				} else {
					++it;
				}
			}
			return out;
		}

		// An NPC handed to this game that another game said is in furniture: the next Changes says what it is in here,
		// even nothing (its end), so the session hears it from its new runner.
		// (What this game said of it before, when it ran it once already, is forgotten: said again, from here.)
		void HandedOver(const Rider& a_rider)
		{
			sits.erase(a_rider);
			handed.insert(a_rider);
		}

		void Clear()
		{
			sits.clear();
			handed.clear();
		}
		[[nodiscard]] std::size_t Count() const { return sits.size(); }
		[[nodiscard]] const std::map<Rider, Protocol::Sit>& Sits() const { return sits; }

	private:
		static Protocol::Sit End(const Rider& a_rider)
		{
			Protocol::Sit end;
			end.player = a_rider.player;
			end.rider = a_rider.player != 0 ? Protocol::NpcKey{} : a_rider.npc;
			end.on = false;
			return end;
		}

		std::map<Rider, Protocol::Sit> sits;
		std::set<Rider>                handed;
	};
}
