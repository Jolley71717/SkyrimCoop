#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
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
	// An NPC that follows our player but is run by another game is asked for this often (it may follow theirs too). One
	// we run that follows our player is said to (kFollower) as it starts following and this often after, however we got
	// it, so the server's book (common/NpcBook.h, FOLLOWS_LAPSE) keeps it from a friend's stray blow or talk; once it
	// stops following, we say kHave once, and a friend who hired it can have it.
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
		bool                             spawn = false;       // made as this game ran (a summon): no other game has it to run
		// A mount ridden here (docs/DESIGN.md, "Riding"): by our player or an NPC we run (ridden: it goes with its rider
		// at once, not at the follower's pace), or by an NPC another game runs (riddenThere: that game runs it).
		bool                             ridden = false;
		bool                             riddenThere = false;
		std::optional<Clock::time_point> lastClaim;     // our last claim of it, of any kind
		// Our last kFollower claim of it, kept until we say kHave or kRelease of it: none, the book has not heard from us
		// that it follows our player (or has heard that it stopped).
		std::optional<Clock::time_point> saidFollows;
	};

	// A friend's game is the better place for it: our player is in another space than it (it walked through a door our
	// player didn't) or far from it, and a friend is in its space, HANDOFF_MARGIN nearer.
	[[nodiscard]] inline bool BetterThere(const Facts& a_facts)
	{
		if (!a_facts.nearestFriend) {
			return false;
		}
		return !a_facts.ours || (*a_facts.ours > HANDOFF_FAR && *a_facts.nearestFriend + HANDOFF_MARGIN < *a_facts.ours);
	}

	[[nodiscard]] inline Ask Decide(const Facts& a_facts, Clock::time_point a_now)
	{
		const auto since = [&](Clock::duration a_every) { return !a_facts.lastClaim || a_now - *a_facts.lastClaim >= a_every; };
		if (a_facts.spawn) {
			// Ours alone (common/NpcBook.h refuses it to anyone else): kept while it is loaded here, and asked for at once
			// when nobody runs it. Letting it go would only have it asked for again.
			return a_facts.owner == Owner::kNobody && since(CLAIM_EVERY) ? Ask::kHave : Ask::kNone;
		}
		switch (a_facts.owner) {
		case Owner::kUs:
			if (a_facts.riddenThere && !a_facts.ridden) {
				// A mount an NPC of another game's rides goes with its rider: that game asks for it (as its follower).
				return since(CLAIM_EVERY) ? Ask::kRelease : Ask::kNone;
			}
			if (a_facts.followsUs) {
				// The book hears it follows our player, whether we got it as a follower or hired it after.
				if (!a_facts.saidFollows || a_now - *a_facts.saidFollows >= FOLLOWER_EVERY) {
					return Ask::kFollower;
				}
			} else if (a_facts.saidFollows) {
				// It stopped following our player (dismissed): the book hears so.
				return Ask::kHave;
			}
			if (a_facts.fighting) {
				return since(FIGHT_EVERY) ? Ask::kFight : Ask::kNone;
			}
			if (a_facts.followsUs) {
				return Ask::kNone;
			}
			// With no friend near, nobody needs it run elsewhere more than here.
			return BetterThere(a_facts) ? Ask::kRelease : Ask::kNone;
		case Owner::kOther:
			if (a_facts.fighting) {
				return since(FIGHT_EVERY) ? Ask::kFight : Ask::kNone;
			}
			if (a_facts.followsUs) {
				// A mount our rider sits on is asked for at the claim rate: rider and mount move in one game.
				return since(a_facts.ridden ? CLAIM_EVERY : FOLLOWER_EVERY) ? Ask::kFollower : Ask::kNone;
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
		// After the wait, any game that has it asks (the nearest friend's game may not list it: disabled in that save, or
		// not among its loaded actors), but never one that would let go of it again at once (BetterThere): that would
		// trade it back and forth every few seconds. Nobody runs it then, and each game's own AI shows it.
		const bool waited = a_facts.unownedSince && a_now - *a_facts.unownedSince >= CLAIM_ANYWAY_AFTER && !BetterThere(a_facts);
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

	// The numbers of each NPC's states, for the whole session. A copy takes only a state newer than the newest it holds
	// (StateBuffer::Push), so a runner that let go of an NPC and has it again (a scan that missed it for a moment: a
	// script disabled it, its 3D was not loaded for a frame; a load; a door there and back) goes on from its last number
	// instead of starting at 1 again, which a copy would refuse until it passed the old count.
	class Sequences
	{
	public:
		std::uint32_t Next(const Protocol::NpcKey& a_key) { return ++numbers[a_key]; }
		void          Clear() { numbers.clear(); }

	private:
		std::map<Protocol::NpcKey, std::uint32_t> numbers;
	};

	// ---- its copies ----

	// Whose states a copy holds. They are dropped, to start over from the next one, when states come from another runner
	// than the one held, or from the same runner after the NPC changed hands (through nobody too): whatever the runner
	// numbered them, the copy never refuses its runner's new states for being older than ones it was sent before.
	struct Feed
	{
		std::uint32_t runner = 0;
		bool          handsChanged = false;

		// The server said someone else runs it now, or nobody.
		void HandsChanged() { handsChanged = true; }

		// States (or gear) of it from a_runner came: true when the copy's states and events must be dropped first.
		bool From(std::uint32_t a_runner)
		{
			const bool over = handsChanged || runner != a_runner;
			runner = a_runner;
			handsChanged = false;
			return over;
		}
	};

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
	inline constexpr std::array<std::string_view, 14> LOCAL_EVENTS{
		"moveStart",
		"moveStop",
		"SneakStart",
		"SneakStop",
		"SprintStart",
		"SprintStop",
		"JumpStandingStart",
		"JumpDirectionalStart",
		"JumpLand",
		"SwimStart",
		"SwimStop",
		"turnStart",
		"turnStop",
		"IdleStop",
	};

	[[nodiscard]] inline bool SameName(std::string_view a_a, std::string_view a_b)
	{
		return std::ranges::equal(a_a, a_b, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
	}

	// A graph event of getting on or off a mount, or of riding one (a rider's "HorseEnter", "HorseExit", its mounted
	// locomotion, a dragon rider's): each game's engine plays those itself as it seats a rider (docs/DESIGN.md, "Riding"),
	// so a friend's or a runner's are never played again on a stand-in or a copy, which may not be on its mount yet. Names
	// that start with "horse", or hold "mount" or "dragonrid", in any case.
	[[nodiscard]] inline bool MountEvent(std::string_view a_event)
	{
		const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
		std::string name(a_event.size(), '\0');
		std::ranges::transform(a_event, name.begin(), lower);
		return name.starts_with("horse") || name.find("mount") != std::string::npos || name.find("dragonrid") != std::string::npos;
	}

	[[nodiscard]] inline bool Replayed(std::string_view a_event)
	{
		if (a_event.empty() || a_event.size() > Protocol::MAX_EVENT_NAME) {
			return false;
		}
		if (std::ranges::any_of(a_event, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; })) {
			return false;
		}
		if (MountEvent(a_event)) {
			return false;
		}
		return std::ranges::none_of(LOCAL_EVENTS, [&](std::string_view a_local) { return SameName(a_local, a_event); });
	}

	// A graph event to play again on a stand-in or a copy, and a stagger's size and way when it is one (set on the graph
	// before it, so it staggers as far and the same way: game/ReactionRules.h).
	struct Event
	{
		std::string                      name;
		std::optional<Protocol::Stagger> stagger;
		bool operator==(const Event&) const = default;
	};

	// A copy's events, each played when the copy is drawn at the moment its runner's graph took it: the copy is drawn a
	// delay behind its states (StateBuffer), so an attack starts where the runner's NPC started it, not ahead of its
	// walk. An event that says no time, or comes later than that point, plays at once; one that waited more than STALE
	// before the copy was drawn at it (a copy not drawn: its AI was its own meanwhile) is dropped.
	class ActionQueue
	{
	public:
		static constexpr std::size_t MAX = 32;
		static constexpr auto        STALE = 2s;

		void Push(std::string a_event, std::optional<Clock::time_point> a_at, Clock::time_point a_now) { Push(Event{ std::move(a_event), std::nullopt }, a_at, a_now); }

		void Push(Event a_event, std::optional<Clock::time_point> a_at, Clock::time_point a_now)
		{
			const auto at = (std::min)(a_at.value_or(a_now), a_now + STALE);  // a clock read wrong never holds the queue up
			auto       pos = std::upper_bound(queue.begin(), queue.end(), at, [](Clock::time_point a_t, const Entry& a_e) { return a_t < a_e.at; });
			queue.insert(pos, Entry{ at, std::move(a_event) });
			while (queue.size() > MAX) {
				queue.pop_front();
				++dropped;
			}
		}

		// The events due when the copy is drawn at a_drawn (now less the delay), in their order: their names.
		[[nodiscard]] std::vector<std::string> Due(Clock::time_point a_drawn)
		{
			std::vector<std::string> out;
			for (auto& event : DueEvents(a_drawn)) {
				out.push_back(std::move(event.name));
			}
			return out;
		}

		// The same, each with its stagger.
		[[nodiscard]] std::vector<Event> DueEvents(Clock::time_point a_drawn)
		{
			std::vector<Event> out;
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
			Event             event;
		};
		std::deque<Entry> queue;
		std::uint64_t     dropped = 0;
	};

	// ---- its gear ----

	// What a copy takes off and puts on to wear and hold what its runner's NPC does. A copy of a load-order NPC is a real
	// NPC of this game, which goes into its saves: it equips only what it carries already, and nothing is ever added to it
	// (a corpse holds only its own game's roll). A spawn's copy is this game's own and never saved
	// (src/game/RemotePlayers.cpp's save guard takes it out): it is given what it lacks. Taking off is the same for both
	// (nothing leaves its inventory): what it wears or holds that its runner's NPC does not, so a torch put away, a helmet
	// taken off, a shield put on the back or a bow swapped for a sword is seen in every game. What the runner's game could
	// not name to this one (a form made as it ran, a mod's this game lacks) is never taken off: the copy keeps its own
	// there (Protocol::NpcGear's unnamed and unnamedSlots). Nor is what sits in a body slot or a hand where the runner's
	// NPC has an item this copy lacks and may not be given (a leveled outfit rolled differently in each game): it keeps
	// its own there rather than stand bare. Runtime forms (FF) name nothing here and are never looked at.
	enum class Hand : std::uint8_t
	{
		kNone,  // worn armor, the ammunition, or whatever hand the game picks (a torch, a shield)
		kRight,
		kLeft,
	};

	struct GearItem
	{
		std::uint32_t id = 0;
		Hand          hand = Hand::kNone;
		bool operator==(const GearItem&) const = default;
	};

	// What a copy wears and holds now: each worn armor as its own form, the form its runner's game names it by (its
	// plugin form, or the one a runtime armor was made from; 0: none) and its body slots; each hand's object and its
	// ammunition (0: none).
	struct Wearing
	{
		struct Armor
		{
			std::uint32_t id = 0;
			std::uint32_t shared = 0;
			std::uint32_t slots = 0;
		};
		std::vector<Armor> armor;
		std::uint32_t      right = 0;
		std::uint32_t      left = 0;
		std::uint32_t      ammo = 0;
	};

	struct Count
	{
		std::uint32_t id = 0;
		std::int32_t  count = 0;
		bool operator==(const Count&) const = default;
	};

	// The hands as one report each, for both what a runner's NPC holds (NpcSync's GearOf) and what its copy holds now
	// (Dress). The game may report a two-handed object (a greatsword, a battleaxe, a warhammer, a bow, a crossbow, a ritual
	// spell, a mod's weapon of any of these) in both hands; taken as it is, a copy would want a second one for its left
	// hand (a load-order copy counting one missing each time an archer draws, a spawn's copy given a second greatsword).
	// So a two-handed object is the right hand's alone and the left is empty, whichever way the game reports it. One-handed
	// weapons or spells in both hands (dual wielding, dual casting) stay in both.
	struct Hands
	{
		std::uint32_t right = 0;
		std::uint32_t left = 0;
		bool operator==(const Hands&) const = default;
	};

	[[nodiscard]] constexpr Hands OneReportEach(std::uint32_t a_right, std::uint32_t a_left, bool a_rightTwoHanded, bool a_leftTwoHanded)
	{
		if (a_left != 0 && a_leftTwoHanded && (a_right == 0 || a_right == a_left)) {
			return { a_left, 0 };  // reported in the left alone: it is still the right hand's
		}
		if (a_right != 0 && a_rightTwoHanded && a_left == a_right) {
			return { a_right, 0 };
		}
		return { a_right, a_left };
	}

	struct GearPlan
	{
		std::vector<GearItem> unequip;  // first, so a hand or a slot is free for what comes
		std::vector<Count>    add;      // then given (a spawn's copy only): each form and how many
		std::vector<GearItem> equip;    // then put on: worn armor, then the right hand, the left, the ammunition
		std::size_t           missing = 0;  // the runner's items this copy lacks and may not be given
	};

	[[nodiscard]] inline GearPlan PlanGear(const Protocol::NpcGear& a_gear, const Wearing& a_now, const std::vector<Count>& a_carried, bool a_mayAdd)
	{
		GearPlan   plan;
		const auto named = [](std::uint32_t a_id) { return a_id != 0 && !FormIds::IsRuntime(a_id); };
		const auto inGear = [&](std::uint32_t a_id) { return std::ranges::any_of(a_gear.worn, [&](const Protocol::WornItem& a_w) { return a_w.id == a_id; }); };
		const auto wearsAsArmor = [&](std::uint32_t a_id) { return std::ranges::any_of(a_now.armor, [&](const Wearing::Armor& a_a) { return a_a.id == a_id || a_a.shared == a_id; }); };
		const auto takeOff = [&](std::uint32_t a_id, Hand a_hand) {
			if (std::ranges::none_of(plan.unequip, [&](const GearItem& a_i) { return a_i.id == a_id && a_i.hand == a_hand; })) {
				plan.unequip.push_back({ a_id, a_hand });
			}
		};
		const auto has = [&](std::uint32_t a_id) {
			std::int32_t n = 0;
			for (const auto& c : a_carried) {
				n += c.id == a_id ? c.count : 0;
			}
			for (const auto& c : plan.add) {
				n += c.id == a_id ? c.count : 0;
			}
			return n;
		};
		// What the runner's NPC wears or holds that this copy can't put on (it lacks it and may not be given it): the copy
		// keeps its own there, as it does where the runner's game could not name an item. An outfit rolled from a leveled
		// list differs in each game (a bandit in hide armor in one, in fur in the other); taken off with nothing to put
		// on, the copy would stand bare in that slot, or fight barehanded.
		const auto lacks = [&](std::uint32_t a_id, std::int32_t a_need) { return named(a_id) && !a_mayAdd && has(a_id) < a_need; };
		std::uint32_t lackedSlots = 0;
		for (const auto& item : a_gear.worn) {
			if (std::ranges::none_of(a_now.armor, [&](const Wearing::Armor& a_a) { return a_a.shared == item.id; }) && lacks(item.id, 1)) {
				lackedSlots |= item.slots;
			}
		}
		// ---- off ----
		for (const auto& armor : a_now.armor) {
			// Only what the runner's game named, and not over a slot it could not name to us or one the copy can't fill.
			if (named(armor.shared) && !inGear(armor.shared) && (armor.slots & (a_gear.unnamedSlots | lackedSlots)) == 0) {
				takeOff(armor.id, Hand::kNone);
			}
		}
		const auto hand = [&](std::uint32_t a_now, std::uint32_t a_wanted, std::int32_t a_need, std::uint8_t a_unnamedBit, Hand a_hand) {
			// A shield is worn armor too: the armor's own rule takes it off. A hand the copy can't fill keeps its own.
			if (a_now != 0 && a_now != a_wanted && (a_gear.unnamed & a_unnamedBit) == 0 && !wearsAsArmor(a_now) && !lacks(a_wanted, a_need)) {
				takeOff(a_now, a_hand);
			}
		};
		hand(a_now.right, a_gear.right, 1, Protocol::kRightUnnamed, Hand::kRight);
		hand(a_now.left, a_gear.left, a_gear.left == a_gear.right ? 2 : 1, Protocol::kLeftUnnamed, Hand::kLeft);
		hand(a_now.ammo, a_gear.ammo, 1, Protocol::kAmmoUnnamed, Hand::kNone);
		// ---- on ----
		const auto putOn = [&](std::uint32_t a_id, Hand a_hand, std::int32_t a_need) {
			const auto have = has(a_id);
			if (have >= a_need) {
				plan.equip.push_back({ a_id, a_hand });
			} else if (a_mayAdd) {
				plan.add.push_back({ a_id, a_need - have });
				plan.equip.push_back({ a_id, a_hand });
			} else {
				++plan.missing;
			}
		};
		std::vector<std::uint32_t> seen;
		for (const auto& item : a_gear.worn) {
			if (!named(item.id) || std::ranges::find(seen, item.id) != seen.end()) {
				continue;
			}
			seen.push_back(item.id);
			if (std::ranges::none_of(a_now.armor, [&](const Wearing::Armor& a_a) { return a_a.shared == item.id; })) {
				putOn(item.id, Hand::kNone, 1);
			}
		}
		// The hands: a shield the worn list has put on already. One weapon in both hands needs two of it.
		if (named(a_gear.right) && a_gear.right != a_now.right && !inGear(a_gear.right)) {
			putOn(a_gear.right, Hand::kRight, 1);
		}
		if (named(a_gear.left) && a_gear.left != a_now.left && !inGear(a_gear.left)) {
			putOn(a_gear.left, Hand::kLeft, a_gear.left == a_gear.right ? 2 : 1);
		}
		if (named(a_gear.ammo) && a_gear.ammo != a_now.ammo) {
			putOn(a_gear.ammo, Hand::kNone, 1);
		}
		return plan;
	}

	// How a plan's hands are done when a spell is in one. The engine takes a spell out of a hand only by deselecting it
	// (Actor::DeselectSpell), which takes no hand: the spell leaves both. So, for every actor dressed (a copy of any NPC,
	// a stand-in), a spell in a hand the plan fills again is not taken off at all (equipping into that hand's slot
	// replaces it: a dual-casting mage drawing a sword in its right keeps its spell in its left, one raising a ward in its
	// left keeps its spell in its right); a spell taken off for an empty hand is deselected once; and a spell the plan
	// keeps in the other hand (it held the same one there) is put back in that hand after.
	struct HandSteps
	{
		std::vector<GearItem> unequip;   // the plan's take-offs that are no spell in a hand (UnequipObject), in its order
		std::vector<GearItem> deselect;  // each spell taken out of every hand (DeselectSpell), once
		std::vector<GearItem> reselect;  // after the deselects: a spell put back in the hand the plan keeps it in
	};

	// a_isSpell(id): whether that id is a spell (held in a hand, in no inventory).
	template <class IsSpell>
	[[nodiscard]] HandSteps StepsOf(const GearPlan& a_plan, const Wearing& a_now, const Protocol::NpcGear& a_gear, IsSpell&& a_isSpell)
	{
		HandSteps  steps;
		const auto heldIn = [&](Hand a_hand) { return a_hand == Hand::kLeft ? a_now.left : a_now.right; };
		const auto wantedIn = [&](Hand a_hand) { return a_hand == Hand::kLeft ? a_gear.left : a_gear.right; };
		// Something goes into that hand: equipped into it, or a shield (put on with the worn armor) that its hand wants.
		const auto filled = [&](Hand a_hand) {
			return std::ranges::any_of(a_plan.equip, [&](const GearItem& a_e) {
				return a_e.hand == a_hand || (a_e.hand == Hand::kNone && a_e.id != 0 && a_e.id == wantedIn(a_hand));
			});
		};
		const auto takenFrom = [&](std::uint32_t a_id, Hand a_hand) {
			return std::ranges::any_of(a_plan.unequip, [&](const GearItem& a_u) { return a_u.id == a_id && a_u.hand == a_hand; });
		};
		for (const auto& item : a_plan.unequip) {
			if (item.hand == Hand::kNone || !a_isSpell(item.id)) {
				steps.unequip.push_back(item);
			} else if (!filled(item.hand) && std::ranges::none_of(steps.deselect, [&](const GearItem& a_d) { return a_d.id == item.id; })) {
				steps.deselect.push_back(item);
			}
		}
		for (const auto& gone : steps.deselect) {
			for (const auto hand : { Hand::kRight, Hand::kLeft }) {
				if (heldIn(hand) == gone.id && !takenFrom(gone.id, hand) && !filled(hand)) {
					steps.reselect.push_back({ gone.id, hand });
				}
			}
		}
		return steps;
	}

	// What a copy wears and holds, read back from it a while after it was dressed, against its runner's gear: the equip
	// and unequip calls say nothing of what the game did (an equip it refused, a bow left on the back by an unequip of
	// the wrong slot). Judged as PlanGear plans: what the runner's game could not name to this one (an unnamed hand,
	// unnamed slots, a runtime form) is not looked at.
	struct GearShown
	{
		std::size_t named = 0;    // the runner's NPC's worn items this game can name
		std::size_t matched = 0;  // of those, worn by the copy
		std::size_t extra = 0;    // worn by the copy, named by the runner's game, and not worn by the runner's NPC: left on
		bool        right = true;  // the right hand holds what the runner's NPC's does
		bool        left = true;
		bool        ammo = true;

		// What is not on that should be.
		[[nodiscard]] std::size_t Short() const { return (named - matched) + (right ? 0 : 1) + (left ? 0 : 1) + (ammo ? 0 : 1); }
		// All of it on, apart from what the copy lacks (a_missing: PlanGear's count when it was dressed), and nothing left
		// on that its runner's NPC took off.
		[[nodiscard]] bool Complete(std::size_t a_missing) const { return extra == 0 && Short() <= a_missing; }
	};

	[[nodiscard]] inline GearShown CheckGear(const Protocol::NpcGear& a_gear, const Wearing& a_now)
	{
		GearShown  out;
		const auto named = [](std::uint32_t a_id) { return a_id != 0 && !FormIds::IsRuntime(a_id); };
		const auto inGear = [&](std::uint32_t a_id) { return std::ranges::any_of(a_gear.worn, [&](const Protocol::WornItem& a_w) { return a_w.id == a_id; }); };
		const auto wears = [&](std::uint32_t a_id) { return std::ranges::any_of(a_now.armor, [&](const Wearing::Armor& a_a) { return a_a.shared == a_id; }); };
		std::vector<std::uint32_t> seen;
		std::uint32_t              notOnSlots = 0;  // the slots of the runner's items the copy doesn't wear (lacked, or refused)
		for (const auto& item : a_gear.worn) {
			if (!named(item.id) || std::ranges::find(seen, item.id) != seen.end()) {
				continue;
			}
			seen.push_back(item.id);
			++out.named;
			if (wears(item.id)) {
				++out.matched;
			} else {
				notOnSlots |= item.slots;
			}
		}
		// The copy's own armor where the runner's item is not on is kept there (PlanGear keeps it where the copy lacks the
		// item): not left on. That item is short already, so a slot the copy could have filled still fails the check.
		for (const auto& armor : a_now.armor) {
			if (named(armor.shared) && !inGear(armor.shared) && (armor.slots & (a_gear.unnamedSlots | notOnSlots)) == 0) {
				++out.extra;
			}
		}
		const auto same = [&](std::uint32_t a_held, std::uint32_t a_wanted, std::uint8_t a_unnamedBit) {
			if ((a_gear.unnamed & a_unnamedBit) != 0 || (a_wanted != 0 && !named(a_wanted))) {
				return true;
			}
			// A shield is worn armor too: the worn items count it, on or lacking; the hand need only hold nothing else,
			// unless the shield is not on (then the hand keeps its own, and the shield is short once already).
			return a_held == a_wanted || (inGear(a_wanted) && (a_held == 0 || !wears(a_wanted)));
		};
		out.right = same(a_now.right, a_gear.right, Protocol::kRightUnnamed);
		out.left = same(a_now.left, a_gear.left, Protocol::kLeftUnnamed);
		out.ammo = same(a_now.ammo, a_gear.ammo, Protocol::kAmmoUnnamed);
		return out;
	}

	// ---- which actors made as a game ran are shared ----

	// Who commands an actor this game made as it ran (a runtime reference, FF).
	enum class Commander
	{
		kNobody,
		kOurPlayer,
		kOurNpc,  // an NPC this game runs (a load-order one, or a spawn of ours)
		kOther,   // a friend's stand-in, an NPC another game runs, one nobody runs, or one not shared
	};

	// Such an actor is shared as this game's spawn when our player or an NPC this game runs commands it: every summon,
	// raised corpse and thrall, whoever cast it (a conjurer the host's game runs, a follower, our player). Any other (a
	// quest's or an encounter's, which every game makes its own of; one a copy conjured while its own AI ran) is not.
	[[nodiscard]] constexpr bool SharedAsSpawn(Commander a_commander)
	{
		return a_commander == Commander::kOurPlayer || a_commander == Commander::kOurNpc;
	}

	// The same, and a mount of ours (docs/DESIGN.md, "Riding"): one our player or an NPC this game runs rides, or our
	// player's last ridden horse (a horse bought or placed as the game ran is a runtime reference), so the others have a
	// copy of it to seat that rider on.
	[[nodiscard]] constexpr bool SharedRuntime(Commander a_commander, bool a_ourMount)
	{
		return SharedAsSpawn(a_commander) || a_ourMount;
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
		kSummon,    // anyone a player or an NPC commands (a conjured atronach, a raised corpse, a thrall)
		kCount,
	};

	[[nodiscard]] constexpr std::string_view KindName(Kind a_kind)
	{
		constexpr std::array<std::string_view, static_cast<std::size_t>(Kind::kCount)> names{ "human", "creature", "dragon", "horse", "follower", "summon" };
		const auto                                                                     i = static_cast<std::size_t>(a_kind);
		return i < names.size() ? names[i] : "?";
	}

	// The kind from what the game says of it: the most telling first (a summoned dragon priest is a summon).
	[[nodiscard]] constexpr Kind KindOf(bool a_commanded, bool a_follower, bool a_flies, bool a_mount, bool a_person)
	{
		if (a_commanded) {
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
