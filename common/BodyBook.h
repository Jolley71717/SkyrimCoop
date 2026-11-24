#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

// Bodies of the dead, as the server keeps them (docs/DESIGN.md, "Bodies"). A body is its NPC's runner's word: a fall or
// a rest is passed on only from the game that runs the NPC (or from any game while nobody does: the runner let go of a
// body it left behind), and only while the session has the NPC dead. The last rest of each body is kept for whoever
// joins or loads later (at most MAX_KEPT, the one laid longest ago forgotten first); a body the session has alive again
// (raised, revived) is never sent, and forgotten. The game that said a body's fall may say its rest even if the NPC
// changed hands while it settled (FellBy: at most MAX_KEPT remembered, each until its rest). A body past a game's budget
// is owed (at most MAX_OWED a game, the oldest dropped and counted) and passed on once the budget has room: a big
// fight's rests are never lost to a burst. No game and no network here: tests/BodyTests.cpp.
namespace BodyBook
{
	using Clock = std::chrono::steady_clock;

	inline constexpr std::size_t MAX_KEPT = 512;
	inline constexpr std::size_t MAX_OWED = 256;

	enum class Verdict
	{
		kPass,     // passed on (a rest is kept too)
		kNotRun,   // another game runs the NPC: refused
		kAlive,    // the session does not have it dead: refused
		kRefused,  // a player of 0, or a body that does not hold
	};

	class Book
	{
	public:
		// a_mayTell: a_player's game runs the NPC, or nobody's does. a_dead: the session has it dead.
		Verdict Take(std::uint32_t a_player, const Protocol::Body& a_body, bool a_mayTell, bool a_dead, Clock::time_point a_now)
		{
			if (a_player == 0 || !Protocol::BodyHolds(a_body)) {
				++counts.refused;
				return Verdict::kRefused;
			}
			if (!a_mayTell) {
				++counts.notRun;
				return Verdict::kNotRun;
			}
			if (!a_dead) {
				++counts.alive;
				return Verdict::kAlive;
			}
			if (a_body.kind == Protocol::BodyKind::kFall) {
				if (fellBy.size() >= MAX_KEPT && !fellBy.contains(a_body.key)) {
					const auto oldest = std::min_element(fellBy.begin(), fellBy.end(), [](const auto& a_a, const auto& a_b) { return a_a.second.at < a_b.second.at; });
					fellBy.erase(oldest);
				}
				fellBy[a_body.key] = Faller{ a_player, a_now };
				++counts.falls;
				return Verdict::kPass;
			}
			fellBy.erase(a_body.key);
			auto it = kept.find(a_body.key);
			if (it == kept.end()) {
				if (kept.size() >= MAX_KEPT) {
					ForgetOldest();
				}
				it = kept.emplace(a_body.key, Kept{}).first;
			}
			it->second = Kept{ a_body, a_now };
			++counts.rests;
			return Verdict::kPass;
		}

		// Every body kept that the session still has dead (a_dead), the one laid longest ago first; the others are
		// forgotten.
		std::vector<Protocol::Body> All(const std::function<bool(const Protocol::NpcKey&)>& a_dead)
		{
			std::erase_if(kept, [&](const auto& a_entry) { return !a_dead(a_entry.first); });
			std::vector<const Kept*> sorted;
			sorted.reserve(kept.size());
			for (const auto& [key, body] : kept) {
				sorted.push_back(&body);
			}
			std::stable_sort(sorted.begin(), sorted.end(), [](const Kept* a_a, const Kept* a_b) { return a_a->at < a_b->at; });
			std::vector<Protocol::Body> out;
			out.reserve(sorted.size());
			for (const auto* body : sorted) {
				out.push_back(body->body);
			}
			return out;
		}

		// Whether a_player's game said a_key's fall (and not its rest yet): its rest is still its word, whoever runs the
		// NPC now.
		[[nodiscard]] bool FellBy(const Protocol::NpcKey& a_key, std::uint32_t a_player) const
		{
			const auto it = fellBy.find(a_key);
			return a_player != 0 && it != fellBy.end() && it->second.player == a_player;
		}

		// Past a player's budget: kept for the loop (Owes, NextOwed), the oldest dropped past MAX_OWED.
		void Owe(std::uint32_t a_player, const Protocol::Body& a_body)
		{
			auto& list = owed[a_player];
			if (list.size() >= MAX_OWED) {
				list.pop_front();
				++counts.owedDropped;
			}
			list.push_back(a_body);
		}
		[[nodiscard]] bool Owes(std::uint32_t a_player) const
		{
			const auto it = owed.find(a_player);
			return it != owed.end() && !it->second.empty();
		}
		bool NextOwed(std::uint32_t a_player, Protocol::Body& a_out)
		{
			const auto it = owed.find(a_player);
			if (it == owed.end() || it->second.empty()) {
				return false;
			}
			a_out = it->second.front();
			it->second.pop_front();
			return true;
		}
		// A player left: what they owed goes, and so does their word on the falls they said.
		void Leave(std::uint32_t a_player)
		{
			owed.erase(a_player);
			std::erase_if(fellBy, [&](const auto& a_entry) { return a_entry.second.player == a_player; });
		}

		void Forget(const Protocol::NpcKey& a_key) { kept.erase(a_key); }
		void Clear()
		{
			kept.clear();
			fellBy.clear();
			owed.clear();
		}
		[[nodiscard]] std::size_t Size() const { return kept.size(); }

		struct Counts
		{
			std::uint64_t falls = 0, rests = 0, notRun = 0, alive = 0, refused = 0, forgotten = 0, owedDropped = 0;
		};
		Counts counts;

	private:
		struct Kept
		{
			Protocol::Body    body;
			Clock::time_point at{};
		};

		void ForgetOldest()
		{
			const auto oldest = std::min_element(kept.begin(), kept.end(), [](const auto& a_a, const auto& a_b) { return a_a.second.at < a_b.second.at; });
			if (oldest != kept.end()) {
				kept.erase(oldest);
				++counts.forgotten;
			}
		}

		struct Faller
		{
			std::uint32_t     player = 0;
			Clock::time_point at{};
		};

		std::map<Protocol::NpcKey, Kept>                    kept;
		std::map<Protocol::NpcKey, Faller>                  fellBy;
		std::map<std::uint32_t, std::deque<Protocol::Body>> owed;
	};
}
