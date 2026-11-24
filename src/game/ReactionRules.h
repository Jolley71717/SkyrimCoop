#pragma once

#include "NpcRules.h"
#include "Protocol.h"
#include "TokenBucket.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Melee hit reactions in every game (docs/DESIGN.md, "Combat"): a stagger, a recoil and a blocked hit count where they
// happen and are shown in the others. The game's side is src/game/Reactions.cpp; no game here: tests/ReactionTests.cpp.
//
// - A stagger, a recoil or a blocked hit our player or an NPC we run takes is one of their graph events, sent as their
//   other actions are (StandInRules::Outbox, NpcSync), a stagger with its size and way (Protocol::Stagger), so the
//   stand-in or the copy that plays it again staggers as far and the same way.
// - Our player's blow lands on a copy here, and our game deals it (the hit goes to its runner, NpcSync); the copy's
//   engine also reacts here: it staggers from a power attack or a bash, recoils when our block stops its swing, plays
//   a blocked hit when our blow lands on its block. That reaction is the hit's too: it goes to the NPC's runner
//   (kNpcReactions), which plays it on its NPC, so the NPC whose swing our block stopped stops swinging in its own game
//   too, and the one our bash staggered is staggered there. The runner's NPC then sends it back as one of its actions,
//   which the copy that already played it does not play again (Echoes).
// - Only a copy's reactions our player caused are sent: one within REACH of a hit between our player and that copy
//   (either way). One our game played itself (the runner's event played on its copy) is never sent back; nor one with
//   no hit of our player's near it (another NPC's blow here: the game that runs the one hit deals it).
namespace ReactionRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// ---- what a reaction is ----

	enum class Kind : std::uint8_t
	{
		kNone,
		kStagger,   // staggered by a blow, a bash, a shout's push short of a fall (staggerStart, and a mod's that say so)
		kRecoil,    // a swing stopped by a block or a wall (recoilStart, recoilLargeStart)
		kBlockHit,  // a blow landed on the block (blockHitStart)
		kCount,
	};

	inline constexpr std::array<std::string_view, static_cast<std::size_t>(Kind::kCount)> KIND_NAMES{ "none", "stagger", "recoil", "blockhit" };

	[[nodiscard]] constexpr std::string_view KindName(Kind a_kind)
	{
		const auto i = static_cast<std::size_t>(a_kind);
		return i < KIND_NAMES.size() ? KIND_NAMES[i] : "?";
	}

	[[nodiscard]] inline bool Contains(std::string_view a_text, std::string_view a_part)
	{
		if (a_part.size() > a_text.size()) {
			return false;
		}
		for (std::size_t i = 0; i + a_part.size() <= a_text.size(); ++i) {
			if (NpcRules::SameName(a_text.substr(i, a_part.size()), a_part)) {
				return true;
			}
		}
		return false;
	}

	// The kind of a graph event, by the words Skyrim's behaviors use (any case, so a mod's events that follow them count):
	// "stagger", "recoil", "blockhit". The end of one (a name with "stop", "end" or "exit" in it) is no reaction: the
	// graph ends it itself.
	[[nodiscard]] inline Kind KindOf(std::string_view a_event)
	{
		if (a_event.empty() || Contains(a_event, "stop") || Contains(a_event, "end") || Contains(a_event, "exit")) {
			return Kind::kNone;
		}
		if (Contains(a_event, "stagger")) {
			return Kind::kStagger;
		}
		if (Contains(a_event, "recoil")) {
			return Kind::kRecoil;
		}
		if (Contains(a_event, "blockhit")) {
			return Kind::kBlockHit;
		}
		return Kind::kNone;
	}

	[[nodiscard]] inline bool IsReaction(std::string_view a_event) { return KindOf(a_event) != Kind::kNone; }

	// ---- a stagger's size and way ----

	// The graph's staggerMagnitude (0 to Protocol::STAGGER_MOST) and staggerDirection (a share of a turn, any value: it is
	// taken round to 0 to 1), each in a byte. Not a number reads 0.
	[[nodiscard]] inline Protocol::Stagger Pack(float a_magnitude, float a_direction)
	{
		Protocol::Stagger out;
		if (std::isfinite(a_magnitude)) {
			const auto share = std::clamp(a_magnitude, 0.0f, Protocol::STAGGER_MOST) / Protocol::STAGGER_MOST;
			out.magnitude = static_cast<std::uint8_t>(std::lround(share * 255.0f));
		}
		if (std::isfinite(a_direction)) {
			const auto turn = a_direction - std::floor(a_direction);  // 0 to 1
			out.direction = static_cast<std::uint8_t>(std::lround(turn * 256.0f) % 256);
		}
		return out;
	}

	struct Unpacked
	{
		float magnitude = 0.0f;
		float direction = 0.0f;
	};

	[[nodiscard]] inline Unpacked Unpack(const Protocol::Stagger& a_stagger)
	{
		return { static_cast<float>(a_stagger.magnitude) / 255.0f * Protocol::STAGGER_MOST, static_cast<float>(a_stagger.direction) / 256.0f };
	}

	// ---- a copy's reactions our player caused ----

	// A copy's reaction this close in time to a hit between our player and it (either way, before or after: the hit's
	// event and the graph's are not sent in a set order) is that hit's.
	inline constexpr auto REACH = 1s;
	// A reaction heard with no hit near it yet waits this long for one (the hit event comes later in the frame) before it
	// is judged not ours.
	inline constexpr auto GRACE = 300ms;
	// At most this many reactions wait to be judged (a burst past it is dropped and counted).
	inline constexpr std::size_t MAX_HEARD = 64;

	struct Heard
	{
		std::uint32_t                    id = 0;  // the copy's actor's form id here
		std::string                      event;
		std::optional<Protocol::Stagger> stagger;
		Clock::time_point                at;
	};

	// The reactions the copies' graphs took that our game did not play itself, and our player's hits with each actor,
	// both as the game's threads hear them; judged on the main thread.
	class Judge
	{
	public:
		void Heard(ReactionRules::Heard a_heard)
		{
			if (heard.size() >= MAX_HEARD) {
				++dropped;
				return;
			}
			heard.push_back(std::move(a_heard));
		}

		// A hit between our player and a_id (an actor), either way.
		void Touched(std::uint32_t a_id, Clock::time_point a_at) { touched[a_id] = (std::max)(touched[a_id], a_at); }

		// The reactions that are our player's hits': each with a hit within REACH of it. One with none waits GRACE for a
		// hit, then is counted not ours. Hits older than REACH are forgotten.
		[[nodiscard]] std::vector<ReactionRules::Heard> Ours(Clock::time_point a_now)
		{
			std::vector<ReactionRules::Heard> out;
			std::deque<ReactionRules::Heard>  waiting;
			for (auto& reaction : heard) {
				const auto hit = touched.find(reaction.id);
				const bool nearby = hit != touched.end() && (hit->second > reaction.at ? hit->second - reaction.at : reaction.at - hit->second) <= REACH;
				if (nearby) {
					++ours;
					out.push_back(std::move(reaction));
				} else if (a_now - reaction.at < GRACE) {
					waiting.push_back(std::move(reaction));
				} else {
					++notOurs;
				}
			}
			heard = std::move(waiting);
			std::erase_if(touched, [&](const auto& a_hit) { return a_now - a_hit.second > REACH + GRACE; });
			return out;
		}

		void Clear()
		{
			heard.clear();
			touched.clear();
		}

		[[nodiscard]] std::size_t Waiting() const { return heard.size(); }
		[[nodiscard]] std::size_t Touching() const { return touched.size(); }

		std::uint64_t ours = 0;
		std::uint64_t notOurs = 0;
		std::uint64_t dropped = 0;

	private:
		std::deque<ReactionRules::Heard>                 heard;
		std::map<std::uint32_t, Clock::time_point>       touched;
	};

	// ---- the runner's same reaction coming back ----

	// The runner's NPC sends a reaction we sent it back as one of its actions, a round trip later; the copy here already
	// played it, so the first of that kind on that copy within ECHO of ours is not played again.
	inline constexpr auto ECHO = 2s;

	class Echoes
	{
	public:
		void Ours(const Protocol::NpcKey& a_key, Kind a_kind, Clock::time_point a_now) { sent[a_key].push_back({ a_kind, a_now }); }

		// True: a_kind on a_key is our own coming back: not to be played (it is taken, so a second is played).
		[[nodiscard]] bool Echo(const Protocol::NpcKey& a_key, Kind a_kind, Clock::time_point a_now)
		{
			const auto it = sent.find(a_key);
			if (it == sent.end() || a_kind == Kind::kNone) {
				return false;
			}
			auto& list = it->second;
			std::erase_if(list, [&](const Entry& a_e) { return a_now - a_e.at > ECHO; });
			const auto match = std::ranges::find_if(list, [&](const Entry& a_e) { return a_e.kind == a_kind; });
			const bool echo = match != list.end();
			if (echo) {
				list.erase(match);
				++echoes;
			}
			if (list.empty()) {
				sent.erase(it);
			}
			return echo;
		}

		void Forget(Clock::time_point a_now)
		{
			std::erase_if(sent, [&](auto& a_pair) {
				std::erase_if(a_pair.second, [&](const Entry& a_e) { return a_now - a_e.at > ECHO; });
				return a_pair.second.empty();
			});
		}

		void Clear() { sent.clear(); }
		[[nodiscard]] std::size_t Size() const { return sent.size(); }

		std::uint64_t echoes = 0;

	private:
		struct Entry
		{
			Kind              kind = Kind::kNone;
			Clock::time_point at;
		};
		std::map<Protocol::NpcKey, std::vector<Entry>> sent;
	};

	// ---- the runner plays a friend's ----

	// A friend's reaction on an NPC we run is played no more often than this for each kind (a flurry of daggers from two
	// friends would stun-lock it: the engine's own stagger does not stack either).
	inline constexpr auto RUNNER_GAP = 300ms;

	enum class Play : std::uint8_t
	{
		kPlay,
		kNotReaction,  // a name that is no reaction: never played (a friend's game may only stagger, recoil or block-hit)
		kTooSoon,      // the same kind on that NPC within RUNNER_GAP
	};

	class Gate
	{
	public:
		[[nodiscard]] Play Judge(const Protocol::NpcKey& a_key, std::string_view a_event, Clock::time_point a_now)
		{
			const auto kind = KindOf(a_event);
			if (kind == Kind::kNone) {
				return Play::kNotReaction;
			}
			auto& at = last[{ a_key, kind }];
			if (at && a_now - *at < RUNNER_GAP) {
				return Play::kTooSoon;
			}
			at = a_now;
			return Play::kPlay;
		}

		void Forget(Clock::time_point a_now)
		{
			std::erase_if(last, [&](const auto& a_pair) { return !a_pair.second || a_now - *a_pair.second > RUNNER_GAP; });
		}

		void Clear() { last.clear(); }
		[[nodiscard]] std::size_t Size() const { return last.size(); }

	private:
		std::map<std::pair<Protocol::NpcKey, Kind>, std::optional<Clock::time_point>> last;
	};

	// ---- on their way out ----

	// Our player's reactions on copies, sent once a tick in messages of at most Protocol::MAX_NPC_REACTIONS. A burst past
	// the budget (a burst of 16, then 8 a second: a fight sends a few a second) is dropped and counted.
	class Outbox
	{
	public:
		static constexpr std::size_t MAX_WAITING = 32;

		bool Add(Protocol::NpcReaction a_reaction, Clock::time_point a_now)
		{
			if (queue.size() >= MAX_WAITING || !budget.Take(a_now)) {
				++dropped;
				return false;
			}
			queue.push_back(std::move(a_reaction));
			return true;
		}

		[[nodiscard]] std::vector<Protocol::NpcReaction> Take()
		{
			std::vector<Protocol::NpcReaction> out;
			while (!queue.empty() && out.size() < Protocol::MAX_NPC_REACTIONS) {
				out.push_back(std::move(queue.front()));
				queue.pop_front();
			}
			sent += out.size();
			return out;
		}

		void Clear() { queue.clear(); }
		[[nodiscard]] std::size_t Waiting() const { return queue.size(); }

		std::uint64_t sent = 0;
		std::uint64_t dropped = 0;

	private:
		std::deque<Protocol::NpcReaction> queue;
		TokenBucket                       budget{ 16.0, 8.0 };
	};
}
