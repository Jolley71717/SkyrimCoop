#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What each game decides about the NPCs near its player (docs/DESIGN.md, "NPCs"): which to ask to run and which to let
// go of, how often to send the states of the ones it runs, when a copy of another game's NPC follows its runner and
// when it is left to its own AI, which of the runner's graph events its copies play and when, and what gear a copy puts
// on. The server's side is common/NpcBook.h; the game's is src/game/NpcSync.cpp. No game here: tests/NpcTests.cpp.
namespace NpcRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// ---- who runs it ----

	// A game asks again for an NPC it was refused, or got no answer for, no sooner than this.
	inline constexpr auto CLAIM_EVERY = 1s;
	// An NPC nobody runs is asked for by the game whose player is nearest it (within CLAIM_MARGIN of the nearest friend),
	// so it starts in the game where it is most likely to be fought; after CLAIM_ANYWAY_AFTER by any game that has it
	// (the nearest friend's game may not have it loaded).
	inline constexpr float CLAIM_MARGIN = 500.0f;
	inline constexpr auto  CLAIM_ANYWAY_AFTER = 2s;
	// A game whose player fights an NPC claims it this often (the server keeps it with the first fighter; its own
	// fights keep it there).
	inline constexpr auto FIGHT_EVERY = 2s;
	// A fight with our player counts this long after its last blow, or after the NPC last targeted our player.
	inline constexpr auto FIGHT_HOLD = 10s;
	// The runner lets go of an NPC its player is this far from when a friend is HANDOFF_MARGIN nearer, so the NPC runs
	// where it is seen from close by: the friend's game asks for it (an NPC fought here or following our player stays).
	inline constexpr float HANDOFF_FAR = 6000.0f;
	inline constexpr float HANDOFF_MARGIN = 1000.0f;
	// An NPC that follows our player but is run by another game is asked for this often (it may follow theirs too).
	inline constexpr auto FOLLOWER_EVERY = 15s;

	enum class Owner
	{
		kNobody,
		kUs,
		kOther,
	};

	enum class Ask
	{
		kNone,
		kHave,
		kFight,
		kRelease,
		kFollower,
	};

	struct Facts
	{
		Owner                            owner = Owner::kNobody;
		std::optional<Clock::time_point> unownedSince;  // when this game first saw it with nobody running it
		std::optional<float>             ours;          // our player's distance from it; none in another space
		std::optional<float>             nearestFriend;  // the nearest friend's distance from it, in its space; none: no friend there
		bool                             fighting = false;    // our player fought or talked to it within FIGHT_HOLD
		bool                             followsUs = false;   // it follows our player (a follower, a summon, a mount)
		std::optional<Clock::time_point> lastClaim;     // our last claim of it, of any kind
	};

	[[nodiscard]] inline Ask Decide(const Facts& a_facts, Clock::time_point a_now)
	{
		const auto since = [&](Clock::duration a_every) { return !a_facts.lastClaim || a_now - *a_facts.lastClaim >= a_every; };
		switch (a_facts.owner) {
		case Owner::kUs:
			if (a_facts.fighting) {
				return since(FIGHT_EVERY) ? Ask::kFight : Ask::kNone;
			}
			if (a_facts.followsUs) {
				return Ask::kNone;
			}
			if (!a_facts.ours || (a_facts.nearestFriend && *a_facts.ours > HANDOFF_FAR && *a_facts.nearestFriend + HANDOFF_MARGIN < *a_facts.ours)) {
				// Another space than our player (it walked through a door our player didn't): the friend there takes it,
				// and with no friend near, nobody needs it run here more than anywhere.
				return a_facts.nearestFriend ? Ask::kRelease : Ask::kNone;
			}
			return Ask::kNone;
		case Owner::kOther:
			if (a_facts.fighting) {
				return since(FIGHT_EVERY) ? Ask::kFight : Ask::kNone;
			}
			if (a_facts.followsUs) {
				return since(FOLLOWER_EVERY) ? Ask::kFollower : Ask::kNone;
			}
			return Ask::kNone;
		case Owner::kNobody:
			break;
		}
		if (!since(CLAIM_EVERY)) {
			return Ask::kNone;
		}
		if (a_facts.followsUs) {
			return Ask::kFollower;
		}
		if (a_facts.fighting) {
			return Ask::kFight;
		}
		const bool nearest = !a_facts.nearestFriend || (a_facts.ours && *a_facts.ours <= *a_facts.nearestFriend + CLAIM_MARGIN);
		const bool waited = a_facts.unownedSince && a_now - *a_facts.unownedSince >= CLAIM_ANYWAY_AFTER;
		return nearest || waited ? Ask::kHave : Ask::kNone;
	}

	[[nodiscard]] constexpr Protocol::ClaimWhy WhyOf(Ask a_ask)
	{
		switch (a_ask) {
		case Ask::kFight:
			return Protocol::ClaimWhy::kFight;
		case Ask::kRelease:
			return Protocol::ClaimWhy::kRelease;
		case Ask::kFollower:
			return Protocol::ClaimWhy::kFollower;
		default:
			return Protocol::ClaimWhy::kHave;
		}
	}

	// ---- its states ----

	// The runner sends an NPC's state this often: 20 a second in a fight or near a player (a copy is drawn
	// StateBuffer's delay behind, which holds two of them), else 10.
	inline constexpr auto  FAST_EVERY = 50ms;
	inline constexpr auto  SLOW_EVERY = 100ms;
	inline constexpr float FAST_WITHIN = 3000.0f;

	[[nodiscard]] inline Clock::duration SendEvery(bool a_inCombat, std::optional<float> a_nearestPlayer)
	{
		return a_inCombat || (a_nearestPlayer && *a_nearestPlayer < FAST_WITHIN) ? Clock::duration{ FAST_EVERY } : Clock::duration{ SLOW_EVERY };
	}

	// ---- its copies ----

	// A copy follows its runner's states (its AI off, placed where the runner's NPC is, its graph told what that NPC's
	// does) while there is a recent state in the space our player is in, within reach of our loaded world. Else it is
	// left to its own AI and walks its own way until the states come back (it is put where they say then): a runner
	// whose game is on a loading screen, an NPC that went through a door ours did not, one moved far by a script.
	inline constexpr float MIRROR_WITHIN = 8000.0f;

	enum class Mode
	{
		kMirror,
		kSilent,     // no recent state
		kElsewhere,  // its runner has it in another space than our player's
		kFar,        // its runner has it further from our player than MIRROR_WITHIN
	};

	[[nodiscard]] inline Mode CopyMode(bool a_recentState, bool a_sameSpace, std::optional<float> a_fromOurPlayer)
	{
		if (!a_recentState) {
			return Mode::kSilent;
		}
		if (!a_sameSpace) {
			return Mode::kElsewhere;
		}
		if (!a_fromOurPlayer || *a_fromOurPlayer > MIRROR_WITHIN) {
			return Mode::kFar;
		}
		return Mode::kMirror;
	}

	[[nodiscard]] constexpr std::string_view ModeName(Mode a_mode)
	{
		switch (a_mode) {
		case Mode::kMirror:
			return "mirror";
		case Mode::kSilent:
			return "silent";
		case Mode::kElsewhere:
			return "elsewhere";
		case Mode::kFar:
			return "far";
		}
		return "?";
	}

	// The runner's graph events its copies play: all but the walk's own (Gait.h works those out on each copy from the
	// states, as it does for a stand-in, so they would come twice) and names no graph takes. A graph's event names are
	// compared without case, as the game's string cache keeps them.
	inline constexpr std::array<std::string_view, 12> LOCAL_EVENTS{
		"moveStart",
		"moveStop",
		"SneakStart",
		"SneakStop",
		"SprintStart",
		"SprintStop",
		"JumpStandingStart",
		"JumpDirectionalStart",
		"JumpLand",
		"turnStart",
		"turnStop",
		"IdleStop",
	};

	[[nodiscard]] inline bool SameName(std::string_view a_a, std::string_view a_b)
	{
		return std::ranges::equal(a_a, a_b, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
	}

	[[nodiscard]] inline bool Replayed(std::string_view a_event)
	{
		if (a_event.empty() || a_event.size() > Protocol::MAX_EVENT_NAME) {
			return false;
		}
		if (std::ranges::any_of(a_event, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; })) {
			return false;
		}
		return std::ranges::none_of(LOCAL_EVENTS, [&](std::string_view a_local) { return SameName(a_local, a_event); });
	}

	// A copy's events, each played when the copy is drawn at the moment its runner's graph took it: the copy is drawn a
	// delay behind its states (StateBuffer), so an attack starts where the runner's NPC started it, not ahead of its
	// walk. An event that says no time, or comes later than that point, plays at once; one that waited more than STALE
	// before the copy was drawn at it (a copy not drawn: its AI was its own meanwhile) is dropped.
	class ActionQueue
	{
	public:
		static constexpr std::size_t MAX = 32;
		static constexpr auto        STALE = 2s;

		void Push(std::string a_event, std::optional<Clock::time_point> a_at, Clock::time_point a_now)
		{
			const auto at = (std::min)(a_at.value_or(a_now), a_now + STALE);  // a clock read wrong never holds the queue up
			auto       pos = std::upper_bound(queue.begin(), queue.end(), at, [](Clock::time_point a_t, const Entry& a_e) { return a_t < a_e.at; });
			queue.insert(pos, Entry{ at, std::move(a_event) });
			while (queue.size() > MAX) {
				queue.pop_front();
				++dropped;
			}
		}

		// The events due when the copy is drawn at a_drawn (now less the delay), in their order.
		[[nodiscard]] std::vector<std::string> Due(Clock::time_point a_drawn)
		{
			std::vector<std::string> out;
			while (!queue.empty() && queue.front().at <= a_drawn) {
				if (a_drawn - queue.front().at > STALE) {
					++dropped;
				} else {
					out.push_back(std::move(queue.front().event));
				}
				queue.pop_front();
			}
			return out;
		}

		// Events no copy will play: the copy left the world, or its AI is its own.
		void Clear() { queue.clear(); }
		[[nodiscard]] std::size_t   Size() const { return queue.size(); }
		[[nodiscard]] std::uint64_t Dropped() const { return dropped; }

	private:
		struct Entry
		{
			Clock::time_point at;
			std::string       event;
		};
		std::deque<Entry> queue;
		std::uint64_t     dropped = 0;
	};

	// ---- its gear ----

	// What a copy puts on of its runner's gear. A copy of a load-order NPC is a real NPC of this game, which goes into its
	// saves: it equips only what it carries already, and nothing is ever added to it (a corpse holds only its own
	// game's roll). A spawn's copy is this game's own and never saved (src/game/RemotePlayers.cpp's save guard takes it
	// out): it is given what it lacks. Runtime forms (FF) name nothing here and are never looked at.
	struct GearPlan
	{
		std::vector<std::uint32_t> equip;  // in the order to equip: worn armor, then the hands and the ammunition
		std::vector<std::uint32_t> add;    // of those, the ones to add first (a spawn's copy only)
		std::size_t                missing = 0;  // the runner's items this copy lacks and may not be given
	};

	[[nodiscard]] inline GearPlan PlanGear(const Protocol::NpcGear& a_gear, const std::vector<std::uint32_t>& a_wornNow, const std::vector<std::uint32_t>& a_carried, bool a_mayAdd)
	{
		GearPlan                   plan;
		std::vector<std::uint32_t> wanted = a_gear.worn;
		for (const auto id : { a_gear.right, a_gear.left, a_gear.ammo }) {
			wanted.push_back(id);
		}
		std::vector<std::uint32_t> seen;
		for (const auto id : wanted) {
			if (id == 0 || FormIds::IsRuntime(id) || std::ranges::find(seen, id) != seen.end()) {
				continue;
			}
			seen.push_back(id);
			if (std::ranges::find(a_wornNow, id) != a_wornNow.end()) {
				continue;
			}
			if (std::ranges::find(a_carried, id) != a_carried.end()) {
				plan.equip.push_back(id);
			} else if (a_mayAdd) {
				plan.add.push_back(id);
				plan.equip.push_back(id);
			} else {
				++plan.missing;
			}
		}
		return plan;
	}

	// ---- what kind of NPC ----

	// Every kind is run and copied the same way; the kind is said in the developer channel so the rig can tell which
	// kinds it saw.
	enum class Kind : std::uint8_t
	{
		kHuman,     // a person of a playable race (and the elder, child and other people races)
		kCreature,  // an animal, a monster, the undead, an automaton
		kDragon,    // a creature of a race that flies
		kHorse,     // a creature that can be ridden
		kFollower,  // anyone following a player
		kSummon,    // anyone a player commands (a conjured atronach, a raised corpse, a thrall)
		kCount,
	};

	[[nodiscard]] constexpr std::string_view KindName(Kind a_kind)
	{
		constexpr std::array<std::string_view, static_cast<std::size_t>(Kind::kCount)> names{ "human", "creature", "dragon", "horse", "follower", "summon" };
		const auto                                                                     i = static_cast<std::size_t>(a_kind);
		return i < names.size() ? names[i] : "?";
	}

	// The kind from what the game says of it: the most telling first (a summoned dragon priest is a summon).
	[[nodiscard]] constexpr Kind KindOf(bool a_commandedByPlayer, bool a_follower, bool a_flies, bool a_mount, bool a_person)
	{
		if (a_commandedByPlayer) {
			return Kind::kSummon;
		}
		if (a_follower) {
			return Kind::kFollower;
		}
		if (a_flies) {
			return Kind::kDragon;
		}
		if (a_mount) {
			return Kind::kHorse;
		}
		return a_person ? Kind::kHuman : Kind::kCreature;
	}
}
