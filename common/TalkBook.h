#pragma once

#include "FormIds.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Who talks with which NPC (docs/DESIGN.md, "Conversations"): the server's book. One player at a time talks with an NPC:
// the first to say so has it, and keeps it while their game says so again every few seconds; another player's game
// that says it talks with the same NPC is answered with who has it, and ends its own talk. A player talks with one NPC
// at a time (a new one ends the talk before the last; the last is kept until said off, for its goodbye), and a talk
// not said again within HOLD is over (a game that stopped saying it: it crashed, or its word was lost). A player's
// lines of an NPC's are passed on only while that player talks with it; an NPC the session cannot name (a runtime
// one, a courier the game made, or one of a plugin the session lacks) is in nobody's book, and its lines pass. Free
// of the game and of the transports: tests/TalkTests.cpp.
class TalkBook
{
public:
	using Clock = std::chrono::steady_clock;

	// A talk not said again within this is over (a game says it every 5 s: src/game/TalkRules.h, Claimer::REFRESH).
	static constexpr auto HOLD = std::chrono::seconds(20);
	// The most talks kept: two a player (one, and the one before while it lingers), with room to spare for players who
	// came back on a new connection.
	static constexpr std::size_t MAX = 64;

	struct Claimed
	{
		bool          granted = false;  // the claimer has it (theirs before, or free)
		std::uint32_t holder = 0;       // who has it now: the claimer when granted
	};

	// a_player says they talk with a_ref now. Granted when nobody else has it (theirs already, free, or held past
	// HOLD); then of their other NPCs only the one said last is kept (its goodbye), until said off. Refused (busy) when
	// another player has it. A runtime reference (no other game has it) or none is nobody's to share: granted, and not
	// kept.
	Claimed Claim(std::uint32_t a_player, std::uint32_t a_ref, Clock::time_point a_now)
	{
		if (a_player == 0 || a_ref == 0 || FormIds::IsRuntime(a_ref)) {
			return { true, a_player };
		}
		Expire(a_now);
		if (const auto it = talks.find(a_ref); it != talks.end() && it->second.player != a_player) {
			return { false, it->second.player };
		}
		// A player talks with one NPC at a time, and keeps the one before until their game says it off (LINGER after its
		// menu closed, so its goodbye still passes: src/game/TalkRules.h, Claimer): of their other talks the one said last
		// is kept, any older one is over (their game said it off already, or its word was lost).
		if (!talks.contains(a_ref)) {  // a new talk (one of theirs said again keeps the rest)
			const Talk* newest = nullptr;
			std::uint32_t newestRef = 0;
			for (const auto& [ref, talk] : talks) {
				if (talk.player == a_player && (!newest || talk.said > newest->said)) {
					newest = &talk;
					newestRef = ref;
				}
			}
			std::erase_if(talks, [&](const auto& a_entry) { return a_entry.second.player == a_player && a_entry.first != newestRef; });
		}
		if (talks.size() >= MAX && !talks.contains(a_ref)) {
			return { true, a_player };  // never: two a player; granted unkept rather than refusing a talk nobody has
		}
		talks[a_ref] = Talk{ a_player, a_now };
		return { true, a_player };
	}

	// a_player no longer talks with a_ref. False when they did not have it (another player's, or nobody's).
	bool Release(std::uint32_t a_player, std::uint32_t a_ref)
	{
		const auto it = talks.find(a_ref);
		if (it == talks.end() || it->second.player != a_player) {
			return false;
		}
		talks.erase(it);
		return true;
	}

	// a_player left the session: the NPCs they talked with, freed.
	std::vector<std::uint32_t> Left(std::uint32_t a_player)
	{
		std::vector<std::uint32_t> freed;
		for (auto it = talks.begin(); it != talks.end();) {
			if (it->second.player == a_player) {
				freed.push_back(it->first);
				it = talks.erase(it);
			} else {
				++it;
			}
		}
		return freed;
	}

	// Whether a_player talks with a_ref now (said within HOLD).
	[[nodiscard]] bool Holds(std::uint32_t a_player, std::uint32_t a_ref, Clock::time_point a_now) const
	{
		const auto it = talks.find(a_ref);
		return it != talks.end() && it->second.player == a_player && a_now - it->second.said < HOLD;
	}

	// Who talks with a_ref now; 0: nobody.
	[[nodiscard]] std::uint32_t HolderOf(std::uint32_t a_ref, Clock::time_point a_now) const
	{
		const auto it = talks.find(a_ref);
		return it != talks.end() && a_now - it->second.said < HOLD ? it->second.player : 0;
	}

	[[nodiscard]] std::size_t Size() const { return talks.size(); }
	void                      Clear() { talks.clear(); }

private:
	struct Talk
	{
		std::uint32_t     player = 0;
		Clock::time_point said{};
	};

	void Expire(Clock::time_point a_now)
	{
		std::erase_if(talks, [&](const auto& a_entry) { return a_now - a_entry.second.said >= HOLD; });
	}

	std::unordered_map<std::uint32_t, Talk> talks;  // by NPC
};
