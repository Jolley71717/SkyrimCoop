#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

// Kill moves and every other paired animation (docs/DESIGN.md, "Kill moves and paired animations"): the rules, free of
// the game (src/game/Pairs.h is the game's side; tests/PairTests.cpp).
//
// A paired animation is one idle the engine plays on two actors together, the second placed against the first and both
// halves in step: every kill move (each weapon's, a sneak kill, an unarmed one, a bow's bash, a creature's: a wolf's, a
// bear's, a sabre cat's, a troll's, a draugr's, a dragon's bite-and-throw; a werewolf's, a vampire lord's) and a vampire's
// feeding. Played as two actors' graph events it would come apart: the second actor's half is the engine's, chosen and
// placed by the pairing, and neither half's own events mean anything on one actor alone. So the class is said as one:
// - Said: the game that runs the actor that begins one (its player, an NPC it runs: a follower, a creature, a dragon, a
//   summon) says it once (Outbox), naming the other actor as the session names it, the idle by its record and its
//   animation event (a reader that lacks the record, a mod's kill move, finds it by the event: EventIndex), whether it
//   is a kill, and where it began. The game knows one began by the attacker's graph taking a paired idle's event
//   (RoleOf: the idles' events begin "pa_"; a kill move's hold "KillMove"); an actor its kill move flag says is in one,
//   whose event was never heard, is said by the idle its process played last, unless the other actor's names the same
//   idle (then neither can be told the attacker: Ambiguous).
// - Not played twice: an actor in a pair (its kill move flag, a paired event lately, or held by one here) sends none of
//   its graph's events to the others as actions while it lasts; each game plays the whole pair itself.
// - Played: at the sender's moment plus the stand-in delay (Inbox), on the actors that show both sides here, when Judge
//   says so: the attacker a friend's stand-in or a copy (never our player or an NPC this game runs: its own game says
//   those), the victim a stand-in, a copy, or an NPC this game runs only when the pair kills it (the hit that kills it
//   reaches this game too, as a friend's hit on an NPC it runs); never our player. Both alive, neither in a pair here,
//   near each other and near where it began.
// - Held: while the pair plays here its two actors are not placed by the update hooks and the engine's own update runs
//   for them (its pairing places them), from the play until both are out of it (Hold), at least MIN_HOLD and at most
//   MAX_HOLD; a pair that never took (neither in it by TAKE_WITHIN) is let go at once.
// Deaths are not this change's: the runner's word is the one death (a copy in a kill move is left to the move; one the
// move killed while its runner's NPC lives is raised, CopyHealth.h).
namespace PairRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr auto  STALE = 2s;           // a friend's pair that waited longer is dropped: it is over there
	inline constexpr auto  SAY_AGAIN = 3s;       // one attacker's pair on one victim is said once in this long
	inline constexpr auto  FLAG_GRACE = 300ms;   // an attacker in a kill move with no event heard is said by its idle after
	inline constexpr auto  EVENT_HOLD = 4s;      // an actor whose graph took a paired event keeps its events this long
	inline constexpr auto  MIN_HOLD = 500ms;     // a pair played here holds its actors at least this long
	inline constexpr auto  TAKE_WITHIN = 1500ms;  // ... and lets them go when neither is in it by then
	inline constexpr auto  MAX_HOLD = 8s;        // ... and at most this long (the longest kill moves take about five)
	inline constexpr auto  FEED_HOLD = 6s;       // a pair that kills no one has no flag to end by: held this long
	inline constexpr float REACH = 400.0f;       // u: the attacker shown here is this near where it began
	inline constexpr float PAIR_REACH = 300.0f;  // u: the victim shown here is this near the attacker
	inline constexpr std::size_t MAX_HELD = 16;  // actors held by pairs at once (eight pairs)

	// ---- which events are a pair's ----

	enum class Role : std::uint8_t
	{
		kNone,      // no pair's event
		kAttacker,  // the idle's own event, on the actor that begins it ("pa_KillMove1HMDecap", "KillMoveDualWieldXCross")
		kVictim,    // the event the engine sends the other actor ("PairedKillTarget")
		kEnd,       // the pair's end ("PairEnd", "PairedStop", "KillMoveEnd")
	};

	[[nodiscard]] inline std::string Lower(std::string_view a_text)
	{
		std::string out(a_text.size(), '\0');
		std::ranges::transform(a_text, out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
		return out;
	}

	// What part of a pair an event of an actor's graph is, by its name in any case: the paired idles' events begin "pa_"
	// (Skyrim's behavior names every paired idle so, and mods' follow it), a kill move's name holds "killmove", and the
	// engine's own pairing events begin "paired" or "pair".
	[[nodiscard]] inline Role RoleOf(std::string_view a_event)
	{
		if (a_event.empty() || a_event.size() > Protocol::MAX_EVENT_NAME) {
			return Role::kNone;
		}
		const auto name = Lower(a_event);
		if (name == "pairend" || name == "pairedstop" || name == "killmoveend" || name == "pairedend") {
			return Role::kEnd;
		}
		if (name == "pairedkilltarget" || name.starts_with("pairedtarget")) {
			return Role::kVictim;
		}
		if (name.starts_with("pa_") || name.find("killmove") != std::string::npos || name.starts_with("paired")) {
			return Role::kAttacker;
		}
		return Role::kNone;
	}

	[[nodiscard]] inline bool PairEvent(std::string_view a_event)
	{
		return RoleOf(a_event) != Role::kNone;
	}

	// Whether an actor's graph events stay in this game now: it is in a kill move (its flag), its graph took a pair's
	// event within EVENT_HOLD, or a pair played here holds it. Its game plays the whole pair (each game plays its own).
	[[nodiscard]] inline bool EventsStayHere(bool a_inKillMove, std::optional<Clock::time_point> a_lastPairEvent, bool a_heldHere, Clock::time_point a_now)
	{
		return a_inKillMove || a_heldHere || (a_lastPairEvent && a_now - *a_lastPairEvent <= EVENT_HOLD);
	}

	// What kind of pair it is, for the counts: a kill move, a sneak kill, or one that kills no one (a feeding).
	enum class Kind : std::uint8_t
	{
		kKill,
		kSneak,
		kOther,
		kCount,
	};
	inline constexpr std::array<std::string_view, static_cast<std::size_t>(Kind::kCount)> KIND_NAMES{ "kill", "sneak", "other" };

	[[nodiscard]] constexpr Kind KindOf(std::uint8_t a_flags)
	{
		if ((a_flags & Protocol::kPairKill) == 0) {
			return Kind::kOther;
		}
		return (a_flags & Protocol::kPairSneak) != 0 ? Kind::kSneak : Kind::kKill;
	}

	[[nodiscard]] constexpr std::string_view NameOf(Kind a_kind)
	{
		const auto i = static_cast<std::size_t>(a_kind);
		return i < KIND_NAMES.size() ? KIND_NAMES[i] : "?";
	}

	struct KindCounts
	{
		std::array<std::uint64_t, static_cast<std::size_t>(Kind::kCount)> n{};

		void Add(Kind a_kind) { ++n[static_cast<std::size_t>(a_kind)]; }
		[[nodiscard]] std::uint64_t Of(Kind a_kind) const { return n[static_cast<std::size_t>(a_kind)]; }
		[[nodiscard]] std::uint64_t Total() const { return n[0] + n[1] + n[2]; }
		// "kill:2,other:1"; "none" when there are none.
		[[nodiscard]] std::string Line() const
		{
			std::string out;
			for (std::size_t i = 0; i < n.size(); ++i) {
				if (n[i] != 0) {
					out += (out.empty() ? "" : ",") + std::string{ KIND_NAMES[i] } + ":" + std::to_string(n[i]);
				}
			}
			return out.empty() ? "none" : out;
		}
	};

	// ---- saying ours ----

	// A side as an ordered key (a map's).
	[[nodiscard]] inline auto SideKey(const Protocol::PairSide& a_side)
	{
		return std::tuple{ a_side.player, a_side.npc.id, a_side.npc.spawner };
	}

	// Each attacker's pair said once: the same attacker on the same victim again within SAY_AGAIN is the same pair (its
	// events and its flag both tell it, a frame apart); on another victim it is another pair.
	class Outbox
	{
	public:
		// True when a_pair is to be said (and it is kept to be sent); false when it was said already.
		bool Say(const Protocol::Pair& a_pair, Clock::time_point a_now)
		{
			const auto key = SideKey(a_pair.attacker);
			if (const auto it = last.find(key); it != last.end() && it->second.victim == a_pair.victim && a_now - it->second.at < SAY_AGAIN) {
				++coalesced;
				return false;
			}
			last[key] = { a_pair.victim, a_now };
			victims[SideKey(a_pair.victim)] = a_now;
			if (queue.size() >= MAX_QUEUED) {
				++dropped;
				return false;
			}
			queue.push_back(a_pair);
			++said;
			return true;
		}

		// Whether the attacker said a pair within SAY_AGAIN (its flag poll does not say it again by its idle).
		[[nodiscard]] bool SaidLately(const Protocol::PairSide& a_attacker, Clock::time_point a_now) const
		{
			const auto it = last.find(SideKey(a_attacker));
			return it != last.end() && a_now - it->second.at < SAY_AGAIN;
		}

		// Whether a_side was the victim of a pair said within SAY_AGAIN (its own kill move flag is the victim's, not a pair of
		// its own).
		[[nodiscard]] bool VictimLately(const Protocol::PairSide& a_side, Clock::time_point a_now) const
		{
			const auto it = victims.find(SideKey(a_side));
			return it != victims.end() && a_now - it->second < SAY_AGAIN;
		}

		// What is to be sent, in messages of at most MAX_PAIRS.
		[[nodiscard]] std::vector<Protocol::Pairs> Take()
		{
			std::vector<Protocol::Pairs> out;
			while (!queue.empty()) {
				Protocol::Pairs msg;
				while (!queue.empty() && msg.pairs.size() < Protocol::MAX_PAIRS) {
					msg.pairs.push_back(std::move(queue.front()));
					queue.pop_front();
				}
				out.push_back(std::move(msg));
			}
			return out;
		}

		// Forgets attackers said longer ago than SAY_AGAIN (a fight's NPCs come and go).
		void Prune(Clock::time_point a_now)
		{
			std::erase_if(last, [&](const auto& a_entry) { return a_now - a_entry.second.at >= SAY_AGAIN; });
			std::erase_if(victims, [&](const auto& a_entry) { return a_now - a_entry.second >= SAY_AGAIN; });
		}

		void Clear()
		{
			last.clear();
			victims.clear();
			queue.clear();
		}

		std::uint64_t said = 0;
		std::uint64_t coalesced = 0;
		std::uint64_t dropped = 0;

	private:
		static constexpr std::size_t MAX_QUEUED = 64;
		struct Said
		{
			Protocol::PairSide victim;
			Clock::time_point  at;
		};
		std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, Said> last;
		std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, Clock::time_point> victims;
		std::deque<Protocol::Pair>                                             queue;
	};

	// An attacker found by its kill move flag alone (no event of its heard): said by the idle its process played last when
	// that idle is a pair's and the other actor's last idle is not the same one (both naming it, neither can be told the
	// attacker: the engine may give the victim the idle too).
	enum class ByFlag : std::uint8_t
	{
		kSay,
		kNotPaired,  // its last idle is no pair's (it is the victim, or its idle is not known)
		kAmbiguous,  // both name the same idle
	};

	[[nodiscard]] inline ByFlag JudgeByFlag(std::uint32_t a_ownIdle, std::string_view a_ownEvent, std::uint32_t a_otherIdle)
	{
		if (a_ownIdle == 0 || RoleOf(a_ownEvent) != Role::kAttacker) {
			return ByFlag::kNotPaired;
		}
		return a_otherIdle == a_ownIdle ? ByFlag::kAmbiguous : ByFlag::kSay;
	}

	// ---- finding an idle by its event ----

	// The load order's paired idles by their animation event, in any case: a pair whose record this game lacks (a mod's,
	// or 0 on the wire) is played by the first idle (lowest id) with that event.
	class EventIndex
	{
	public:
		void Add(std::string_view a_event, std::uint32_t a_id)
		{
			if (a_id == 0 || a_event.empty()) {
				return;
			}
			auto [it, fresh] = ids.try_emplace(Lower(a_event), a_id);
			if (!fresh && a_id < it->second) {
				it->second = a_id;
			}
		}

		[[nodiscard]] std::optional<std::uint32_t> Find(std::string_view a_event) const
		{
			const auto it = ids.find(Lower(a_event));
			return it != ids.end() ? std::optional{ it->second } : std::nullopt;
		}

		[[nodiscard]] std::size_t Size() const { return ids.size(); }
		void Clear() { ids.clear(); }

	private:
		std::unordered_map<std::string, std::uint32_t> ids;
	};

	// ---- playing a friend's ----

	// A friend's pairs, each due when the actors that show it are drawn at the moment it began (the stand-ins' delay
	// behind); one that waited longer than STALE is dropped.
	class Inbox
	{
	public:
		static constexpr std::size_t MAX = 32;

		void Push(const Protocol::Pair& a_pair, std::uint32_t a_sender, std::optional<Clock::time_point> a_at, Clock::time_point a_now)
		{
			const auto at = (std::min)(a_at.value_or(a_now), a_now + STALE);  // a clock read wrong never holds the queue up
			const auto pos = std::upper_bound(queue.begin(), queue.end(), at, [](Clock::time_point a_t, const Entry& a_e) { return a_t < a_e.at; });
			queue.insert(pos, Entry{ at, a_sender, a_pair });
			while (queue.size() > MAX) {
				queue.pop_front();
				++dropped;
			}
		}

		struct Due
		{
			std::uint32_t  sender = 0;
			Protocol::Pair pair;
		};

		// The pairs due when their actors are drawn at a_drawn (now less the delay), in their order.
		[[nodiscard]] std::vector<Due> Take(Clock::time_point a_drawn)
		{
			std::vector<Due> out;
			while (!queue.empty() && queue.front().at <= a_drawn) {
				if (a_drawn - queue.front().at > STALE) {
					++late;
				} else {
					out.push_back({ queue.front().sender, std::move(queue.front().pair) });
				}
				queue.pop_front();
			}
			return out;
		}

		// A player left: their pairs go.
		void Forget(std::uint32_t a_sender)
		{
			std::erase_if(queue, [&](const Entry& a_e) { return a_e.sender == a_sender; });
		}

		void Clear() { queue.clear(); }
		[[nodiscard]] std::size_t Size() const { return queue.size(); }

		std::uint64_t late = 0;
		std::uint64_t dropped = 0;

	private:
		struct Entry
		{
			Clock::time_point at;
			std::uint32_t     sender = 0;
			Protocol::Pair    pair;
		};
		std::deque<Entry> queue;
	};

	// What shows a side of a pair here.
	enum class Shown : std::uint8_t
	{
		kNone,       // nothing here (not loaded, no stand-in, no copy)
		kOurPlayer,  // our player
		kStandIn,    // a friend's stand-in
		kCopy,       // a copy of an NPC another game runs
		kRunHere,    // an NPC this game runs
	};

	struct Facts
	{
		Shown attacker = Shown::kNone;
		Shown victim = Shown::kNone;
		bool  attackerAlive = true;
		bool  victimAlive = true;
		bool  attackerBusy = false;  // already in a pair (a kill move, or held by one here), or in furniture or a saddle
		bool  victimBusy = false;
		float apart = 0.0f;           // u: between the two actors here
		float drift = 0.0f;           // u: the attacker here from where the pair began
		bool  kill = false;           // the pair kills its victim (Protocol::kPairKill)
		bool  idleFound = false;      // the idle, by its record or its event
	};

	enum class Verdict : std::uint8_t
	{
		kPlay,
		kNoAttacker,
		kNoVictim,
		kAttackerOurs,    // our player or an NPC we run: our game says its own (the sender's book was behind)
		kVictimOurPlayer, // never: our player's fights are our game's
		kVictimOurs,      // an NPC we run, in a pair that does not kill it: its own AI is this game's
		kDead,
		kBusy,
		kFar,
		kNoIdle,
		kCount,
	};
	inline constexpr std::array<std::string_view, static_cast<std::size_t>(Verdict::kCount)> VERDICT_NAMES{ "play", "noAttacker", "noVictim", "attackerOurs",
		"victimOurPlayer", "victimOurs", "dead", "busy", "far", "noIdle" };

	[[nodiscard]] constexpr std::string_view NameOf(Verdict a_verdict)
	{
		const auto i = static_cast<std::size_t>(a_verdict);
		return i < VERDICT_NAMES.size() ? VERDICT_NAMES[i] : "?";
	}

	[[nodiscard]] inline Verdict Judge(const Facts& a_facts)
	{
		if (a_facts.attacker == Shown::kNone) {
			return Verdict::kNoAttacker;
		}
		if (a_facts.attacker == Shown::kOurPlayer || a_facts.attacker == Shown::kRunHere) {
			return Verdict::kAttackerOurs;
		}
		if (a_facts.victim == Shown::kNone) {
			return Verdict::kNoVictim;
		}
		if (a_facts.victim == Shown::kOurPlayer) {
			return Verdict::kVictimOurPlayer;
		}
		if (a_facts.victim == Shown::kRunHere && !a_facts.kill) {
			return Verdict::kVictimOurs;
		}
		if (!a_facts.attackerAlive || !a_facts.victimAlive) {
			return Verdict::kDead;
		}
		if (a_facts.attackerBusy || a_facts.victimBusy) {
			return Verdict::kBusy;
		}
		if (!(a_facts.apart <= PAIR_REACH) || !(a_facts.drift <= REACH)) {
			return Verdict::kFar;
		}
		if (!a_facts.idleFound) {
			return Verdict::kNoIdle;
		}
		return Verdict::kPlay;
	}

	struct VerdictCounts
	{
		std::array<std::uint64_t, static_cast<std::size_t>(Verdict::kCount)> n{};

		void Add(Verdict a_verdict) { ++n[static_cast<std::size_t>(a_verdict)]; }
		[[nodiscard]] std::uint64_t Of(Verdict a_verdict) const { return n[static_cast<std::size_t>(a_verdict)]; }
		// The refusals only ("far:1,dead:2"); "none" when there were none.
		[[nodiscard]] std::string Refused() const
		{
			std::string out;
			for (std::size_t i = 1; i < n.size(); ++i) {
				if (n[i] != 0) {
					out += (out.empty() ? "" : ",") + std::string{ VERDICT_NAMES[i] } + ":" + std::to_string(n[i]);
				}
			}
			return out.empty() ? "none" : out;
		}
	};

	// ---- holding a pair played here ----

	enum class Hold : std::uint8_t
	{
		kHold,    // still playing: not placed, the engine's update
		kEnded,   // both out of it: placed by their states again
		kNoTake,  // neither was ever in it: the play did not take
		kGaveUp,  // MAX_HOLD (or FEED_HOLD for a pair that kills no one) passed
	};

	// One pair played here, from its play: a_in, whether either actor is in it now (a kill move's flag; for a pair that
	// kills no one there is none, and it is held FEED_HOLD); a_took, whether either was ever seen in it.
	[[nodiscard]] inline Hold HoldStep(Clock::time_point a_since, Clock::time_point a_now, bool a_kill, bool a_in, bool a_took)
	{
		const auto age = a_now - a_since;
		if (!a_kill) {
			return age >= FEED_HOLD ? Hold::kEnded : Hold::kHold;
		}
		if (age >= MAX_HOLD) {
			return Hold::kGaveUp;
		}
		if (!a_took && !a_in) {
			return age >= TAKE_WITHIN ? Hold::kNoTake : Hold::kHold;
		}
		if (!a_in && age >= MIN_HOLD) {
			return Hold::kEnded;
		}
		return Hold::kHold;
	}
}
