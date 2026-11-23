#pragma once

#include "Protocol.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

// NPCs' health and deaths, as the server keeps them (docs/DESIGN.md, "NPC health and deaths"). The game that runs an NPC
// (common/NpcBook.h) is the truth of both; the server:
// - sends a player's hit on a copy to the NPC's runner alone, by its own book, not the hitter's (whose may be a round
//   trip behind), and answers it itself when it can't go anywhere: nobody runs it, the hitter runs it, the session has
//   it dead (F4Multiplayer's docs/HEALTH-SYNC.md, rule 3);
// - passes on only a runner's word of its NPCs' health and deaths, and keeps the last word of each NPC run now and every
//   one dead for whoever joins after (a joiner's copy of an NPC its own save has dead is raised by that word);
// - takes one death of an NPC, its runner's: a second report of it (another game's copy dying, or the same death
//   again) is dropped, so no game kills one twice. A runner that says a dead NPC lives (raised by a spell) brings it
//   back for the session.
// - takes the death from the game that ran it a moment ago too (DEATH_GRACE): one whose NPC died there before it heard
//   that it lost it (another game's fight claim, or its own release, reached the server first). Every game hears that
//   death, that game too (its body is a copy now), and the new runner kills the NPC it has alive. What the new runner
//   says of it alive in that moment it said before it heard, and is dropped.
// No game and no network here: tests/NpcCombatTests.cpp.
namespace NpcFates
{
	// Where a hit goes: to a_runner, or answered at once with a_refused.
	struct Route
	{
		std::uint32_t                       runner = 0;
		std::optional<Protocol::HitVerdict> refused;
	};

	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// A death from the game that ran an NPC is taken this long after it let go of it or lost it: a round trip and more.
	inline constexpr auto DEATH_GRACE = 3s;

	enum class Death
	{
		kNew,      // the session's first word of it, from its runner: everyone else is told
		kLate,     // the session's first word of it, from the game that ran it a moment ago: everyone is told, that game too
		kKnown,    // dead already: dropped
		kRefused,  // from a game that does not run it (and did not a moment ago): dropped
	};

	enum class Health
	{
		kTaken,    // passed on
		kRevived,  // the runner says an NPC the session had dead lives: passed on, and it is dead no more
		kStale,    // the runner says alive an NPC its last runner said died a moment ago: said before it heard, dropped
		kRefused,  // from a game that does not run it: dropped
	};

	class Book
	{
	public:
		// a_runner: who runs it, by the server's NpcBook.
		[[nodiscard]] Route RouteHit(std::uint32_t a_hitter, std::uint32_t a_runner, const Protocol::NpcHit& a_hit) const
		{
			if (dead.contains(a_hit.key)) {
				return { .refused = Protocol::HitVerdict::kDead };
			}
			if (a_runner == 0) {
				return { .refused = Protocol::HitVerdict::kNobody };
			}
			if (a_runner == a_hitter) {
				return { .refused = Protocol::HitVerdict::kYours };
			}
			return { .runner = a_runner };
		}

		Health Said(std::uint32_t a_sender, std::uint32_t a_runner, const Protocol::NpcHealth& a_health, Clock::time_point a_now)
		{
			if (a_sender == 0 || a_sender != a_runner) {
				return Health::kRefused;
			}
			const auto was = dead.find(a_health.key);
			if (was != dead.end() && was->second.late && a_now - was->second.at < DEATH_GRACE) {
				return Health::kStale;
			}
			const bool revived = was != dead.end();
			if (revived) {
				dead.erase(was);
			}
			if (said.size() < Protocol::MAX_NPCS || said.contains(a_health.key)) {
				said[a_health.key] = a_health.share;
			}
			return revived ? Health::kRevived : Health::kTaken;
		}

		Death Died(std::uint32_t a_sender, std::uint32_t a_runner, const Protocol::NpcDeath& a_death, Clock::time_point a_now)
		{
			if (a_sender == 0) {
				return Death::kRefused;
			}
			bool late = false;
			if (a_sender != a_runner) {
				const auto was = lastRunner.find(a_death.key);
				late = was != lastRunner.end() && was->second.player == a_sender && a_now - was->second.at <= DEATH_GRACE;
				if (!late) {
					return Death::kRefused;
				}
			}
			if (dead.contains(a_death.key)) {
				return Death::kKnown;
			}
			said.erase(a_death.key);
			if (dead.size() < Protocol::MAX_NPCS) {
				dead.emplace(a_death.key, Dead{ a_death, a_now, late });
			}
			return late ? Death::kLate : Death::kNew;
		}

		// a_was ran it and does no more (another game won it, or it let go of it): a death there is still taken for
		// DEATH_GRACE.
		void HandedOver(const Protocol::NpcKey& a_key, std::uint32_t a_was, Clock::time_point a_now)
		{
			std::erase_if(lastRunner, [&](const auto& a_entry) { return a_now - a_entry.second.at > DEATH_GRACE; });
			if (a_was != 0 && (lastRunner.size() < Protocol::MAX_NPCS || lastRunner.contains(a_key))) {
				lastRunner[a_key] = { a_was, a_now };
			}
		}

		// Nobody runs it now: its next runner says its own health (a dead one stays dead).
		void Released(const Protocol::NpcKey& a_key) { said.erase(a_key); }

		// A player left: their spawns are gone with them, dead or hurt.
		void Left(std::uint32_t a_player)
		{
			std::erase_if(said, [&](const auto& a_entry) { return a_entry.first.spawner == a_player; });
			std::erase_if(dead, [&](const auto& a_entry) { return a_entry.first.spawner == a_player; });
			std::erase_if(lastRunner, [&](const auto& a_entry) { return a_entry.second.player == a_player; });
		}

		[[nodiscard]] bool IsDead(const Protocol::NpcKey& a_key) const { return dead.contains(a_key); }

		// For a joiner: the last word of every NPC run now, and every one the session has dead.
		[[nodiscard]] std::vector<Protocol::NpcHealth> Healths() const
		{
			std::vector<Protocol::NpcHealth> out;
			out.reserve(said.size());
			for (const auto& [key, share] : said) {
				out.push_back({ key, share });
			}
			return out;
		}

		[[nodiscard]] std::vector<Protocol::NpcDeath> Dead() const
		{
			std::vector<Protocol::NpcDeath> out;
			out.reserve(dead.size());
			for (const auto& [key, death] : dead) {
				out.push_back(death.death);
			}
			return out;
		}

		void Clear()
		{
			said.clear();
			dead.clear();
			lastRunner.clear();
		}

	private:
		struct Dead
		{
			Protocol::NpcDeath death;
			Clock::time_point  at{};
			bool               late = false;  // taken from the game that ran it a moment before (Death::kLate)
		};
		struct Was
		{
			std::uint32_t     player = 0;
			Clock::time_point at{};
		};

		std::map<Protocol::NpcKey, float> said;        // each NPC's runner's last word, while it is run
		std::map<Protocol::NpcKey, Dead>  dead;
		std::map<Protocol::NpcKey, Was>   lastRunner;  // who ran each NPC that changed hands in the last DEATH_GRACE
	};
}
