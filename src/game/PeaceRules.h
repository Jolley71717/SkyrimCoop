#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

// The world leaves a friend's stand-in alone, and a stand-in leaves the world alone (docs/DESIGN.md, "Players"): the
// rules, free of the game (src/game/StandInPeace.h is the game's side; tests/PeaceTests.cpp).
//
// A stand-in is a plain NPC to the engine. Every actor here (a guard, a townsperson, a creature, a dragon, a horse, a
// follower, a summon: all Characters) could pick it as a combat target, start a conversation with it, gather to watch it
// or flee from it, and its harmless blows could start a fight or an assault alarm that its friend's game never had.
// None of that is its friend: their fights, talks and crimes are settled in their own game. So the engine's asking
// about a stand-in is turned away where it asks (Refused), an actor seen fighting one all the same is told to stop
// (CalmBook), and every effect anyone puts on a stand-in is refused (JudgeMagic): friends' weapons, spells and shouts
// pass through each other, as the feature plan's "friendly fire off" says, until hits on a stand-in are sent to its
// friend's game.
namespace PeaceRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// ---- who is a stand-in, for hooks on any thread ----

	// A set of form ids the engine's hooks read with no lock, on whatever thread the engine asks on (a combat job, an AI
	// job, the main thread). One writer at a time (the stand-ins' own lock). An id in the set before and after an Assign
	// is never missing from it in between, so a hook never lets a stand-in through while the set is written.
	template <std::size_t N>
	class IdSet
	{
	public:
		[[nodiscard]] bool Contains(std::uint32_t a_id) const noexcept
		{
			if (a_id == 0) {
				return false;
			}
			for (const auto& slot : slots) {
				if (slot.load(std::memory_order_acquire) == a_id) {
					return true;
				}
			}
			return false;
		}

		// The set becomes a_ids (0s and repeats ignored). False when more than N were given: the rest are left out.
		bool Assign(std::span<const std::uint32_t> a_ids) noexcept
		{
			const auto wanted = [&](std::uint32_t a_id) { return std::find(a_ids.begin(), a_ids.end(), a_id) != a_ids.end(); };
			for (auto& slot : slots) {
				const auto id = slot.load(std::memory_order_relaxed);
				if (id != 0 && !wanted(id)) {
					slot.store(0, std::memory_order_release);
				}
			}
			bool fits = true;
			for (const auto id : a_ids) {
				if (id == 0 || Contains(id)) {
					continue;
				}
				auto empty = std::find_if(slots.begin(), slots.end(), [](const auto& a_slot) { return a_slot.load(std::memory_order_relaxed) == 0; });
				if (empty == slots.end()) {
					fits = false;
					continue;
				}
				empty->store(id, std::memory_order_release);
			}
			return fits;
		}

		[[nodiscard]] std::size_t Count() const noexcept
		{
			return static_cast<std::size_t>(std::count_if(slots.begin(), slots.end(), [](const auto& a_slot) { return a_slot.load(std::memory_order_relaxed) != 0; }));
		}

	private:
		std::array<std::atomic<std::uint32_t>, N> slots{};
	};

	// Stand-ins here and those on their way out: one per friend, and a few leaving. Far above any session's.
	inline constexpr std::size_t MAX_WATCHED = 64;

	// ---- what the engine asks an actor's AI about another ----

	enum class Ask : std::uint8_t
	{
		kTarget,  // may it be a combat target (Actor::CheckValidTarget)
		kTalk,    // start a conversation with it (Actor::InitiateDialogue)
		kWatch,   // gather to watch it (Actor::InitiateSpectator: a brawl's crowd)
		kFlee,    // flee from it (Actor::InitiateFlee)
		kCount
	};

	[[nodiscard]] constexpr std::string_view Name(Ask a_ask)
	{
		switch (a_ask) {
		case Ask::kTarget:
			return "target";
		case Ask::kTalk:
			return "talk";
		case Ask::kWatch:
			return "watch";
		case Ask::kFlee:
			return "flee";
		default:
			return "?";
		}
	}

	// Whether the engine's asking a_asker's AI about a_other is turned away: a stand-in is no one's combat target, no
	// one's to talk to, watch or flee from, and asks none of that of anyone itself (its AI never runs; while the engine
	// gets it into or out of furniture its own update does run, a few seconds, and it must pick no fight then either).
	// Every other actor's asking about every other is the engine's, as it was.
	[[nodiscard]] constexpr bool Refused(bool a_askerIsStandIn, bool a_otherIsStandIn)
	{
		return a_askerIsStandIn || a_otherIsStandIn;
	}

	// ---- an actor seen fighting a stand-in all the same ----

	// The sweep looks at every actor in high process this often: one whose combat target is a stand-in (a console's
	// startcombat, an engine path the hooks do not see) is told to stop fighting (Actor::StopCombat). It then picks its
	// next target the engine's way, never the stand-in, so one that is hostile to our player goes on with that fight.
	inline constexpr auto SWEEP_EVERY = 500ms;
	// One actor is told at most once each CALM_COOL; after CALM_TRIES within CALM_WINDOW it is given up on (said once)
	// for CALM_REST, then tried again: an engine path that sets the target again each time is not fought every frame.
	inline constexpr auto        CALM_COOL = 2s;
	inline constexpr std::size_t CALM_TRIES = 4;
	inline constexpr auto        CALM_WINDOW = 30s;
	inline constexpr auto        CALM_REST = 60s;
	// Actors the book keeps at most (each forgotten CALM_WINDOW after its last stop, or at the end of its rest).
	inline constexpr std::size_t CALM_MAX = 1024;

	enum class Calm : std::uint8_t
	{
		kWait,    // told lately, or given up on and resting: nothing now
		kStop,    // tell it to stop fighting
		kGiveUp,  // told CALM_TRIES times in CALM_WINDOW: given up on for CALM_REST (said once)
	};

	class CalmBook
	{
	public:
		// a_actor was seen with a stand-in as its combat target at a_now.
		Calm Seen(std::uint32_t a_actor, Clock::time_point a_now)
		{
			auto it = entries.find(a_actor);
			if (it == entries.end()) {
				if (entries.size() >= CALM_MAX) {
					Age(a_now);
					if (entries.size() >= CALM_MAX) {
						return Calm::kWait;  // the book is full of actors told within the window: this one waits for room
					}
				}
				it = entries.emplace(a_actor, Entry{}).first;
			}
			auto& entry = it->second;
			if (entry.givenUp) {
				if (a_now < entry.restUntil) {
					return Calm::kWait;
				}
				entry = {};
			}
			if (!entry.stops.empty() && a_now - entry.stops.back() < CALM_COOL) {
				return Calm::kWait;
			}
			std::erase_if(entry.stops, [&](Clock::time_point a_stop) { return a_now - a_stop >= CALM_WINDOW; });
			if (entry.stops.size() >= CALM_TRIES) {
				entry.givenUp = true;
				entry.restUntil = a_now + CALM_REST;
				entry.stops.clear();
				return Calm::kGiveUp;
			}
			entry.stops.push_back(a_now);
			return Calm::kStop;
		}

		// Actors told nothing for CALM_WINDOW, and those whose rest is over, are forgotten.
		void Age(Clock::time_point a_now)
		{
			std::erase_if(entries, [&](const auto& a_entry) {
				const auto& entry = a_entry.second;
				return entry.givenUp ? a_now >= entry.restUntil : entry.stops.empty() || a_now - entry.stops.back() >= CALM_WINDOW;
			});
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] std::size_t Size() const { return entries.size(); }
		[[nodiscard]] std::size_t GivenUp() const
		{
			return static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(), [](const auto& a_entry) { return a_entry.second.givenUp; }));
		}

	private:
		struct Entry
		{
			std::vector<Clock::time_point> stops;  // within CALM_WINDOW
			bool                           givenUp = false;
			Clock::time_point              restUntil{};
		};
		std::unordered_map<std::uint32_t, Entry> entries;
	};

	// ---- what a spell passing through a stand-in may hit (the rig's `peace line`) ----

	// A place on the ground's plane (x, y: a cast line is judged from above).
	struct Flat
	{
		float x = 0.0f;
		float y = 0.0f;
	};

	// The rig casts at a stand-in, and weapons and spells pass through it (the ghost flag): whoever stands on the line
	// beyond it is hit instead, and in a town that is an assault on a person of this game. Whether a_at stands within
	// a_width of the line from a_from (the caster) through a_through (the stand-in), from a_width behind the caster out to
	// a_beyond past the stand-in. A stand-in on the caster's own spot (closer than 1 u): anyone within a_beyond of it.
	[[nodiscard]] inline bool OnLine(Flat a_from, Flat a_through, Flat a_at, float a_beyond, float a_width)
	{
		const float dx = a_through.x - a_from.x;
		const float dy = a_through.y - a_from.y;
		const float ax = a_at.x - a_from.x;
		const float ay = a_at.y - a_from.y;
		const float length = std::sqrt(dx * dx + dy * dy);
		if (length < 1.0f) {
			return ax * ax + ay * ay <= a_beyond * a_beyond;
		}
		const float along = (ax * dx + ay * dy) / length;  // how far along the line, from the caster
		const float aside = std::abs(ax * dy - ay * dx) / length;  // how far off it
		return along >= -a_width && along <= length + a_beyond && aside <= a_width;
	}

	// The rig's line: out to this past the stand-in (a Firebolt flies on far; a Flames cone reaches less) and this wide.
	inline constexpr float LINE_BEYOND = 3000.0f;
	inline constexpr float LINE_WIDTH = 200.0f;

	// ---- effects put on an actor ----

	enum class Magic : std::uint8_t
	{
		kLet,        // neither is a stand-in: the engine's, as it was
		kKept,       // a stand-in's own effect on itself, of a kind it shows (StandInRules::MagicKept)
		kRefused,    // a stand-in's effect on anyone else, or one on itself it does not show: its friend's game deals it
		kRefusedOn,  // anyone's effect on a stand-in (a spell, a shout, a trap's, a hazard's, our player's heal): its
		             // friend is not here, and their own game gives them what lands on them
	};

	// a_onItself: the caster is the target; a_keptKind: StandInRules::MagicKept's verdict on the effect's kind.
	[[nodiscard]] constexpr Magic JudgeMagic(bool a_casterIsStandIn, bool a_targetIsStandIn, bool a_onItself, bool a_keptKind)
	{
		if (a_casterIsStandIn && a_onItself) {
			return a_keptKind ? Magic::kKept : Magic::kRefused;
		}
		if (a_targetIsStandIn) {
			return Magic::kRefusedOn;
		}
		return a_casterIsStandIn ? Magic::kRefused : Magic::kLet;
	}
}
