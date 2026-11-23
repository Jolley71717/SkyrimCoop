#pragma once

#include "Protocol.h"

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
// No game and no network here: tests/NpcCombatTests.cpp.
namespace NpcFates
{
	// Where a hit goes: to a_runner, or answered at once with a_refused.
	struct Route
	{
		std::uint32_t                       runner = 0;
		std::optional<Protocol::HitVerdict> refused;
	};

	enum class Death
	{
		kNew,      // the session's first word of it: everyone else is told
		kKnown,    // dead already: dropped
		kRefused,  // from a game that does not run it: dropped
	};

	enum class Health
	{
		kTaken,    // passed on
		kRevived,  // the runner says an NPC the session had dead lives: passed on, and it is dead no more
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

		Health Said(std::uint32_t a_sender, std::uint32_t a_runner, const Protocol::NpcHealth& a_health)
		{
			if (a_sender == 0 || a_sender != a_runner) {
				return Health::kRefused;
			}
			const bool revived = dead.erase(a_health.key) != 0;
			if (said.size() < Protocol::MAX_NPCS || said.contains(a_health.key)) {
				said[a_health.key] = a_health.share;
			}
			return revived ? Health::kRevived : Health::kTaken;
		}

		Death Died(std::uint32_t a_sender, std::uint32_t a_runner, const Protocol::NpcDeath& a_death)
		{
			if (a_sender == 0 || a_sender != a_runner) {
				return Death::kRefused;
			}
			if (dead.contains(a_death.key)) {
				return Death::kKnown;
			}
			said.erase(a_death.key);
			if (dead.size() < Protocol::MAX_NPCS) {
				dead.emplace(a_death.key, a_death);
			}
			return Death::kNew;
		}

		// Nobody runs it now: its next runner says its own health (a dead one stays dead).
		void Released(const Protocol::NpcKey& a_key) { said.erase(a_key); }

		// A player left: their spawns are gone with them, dead or hurt.
		void Left(std::uint32_t a_player)
		{
			std::erase_if(said, [&](const auto& a_entry) { return a_entry.first.spawner == a_player; });
			std::erase_if(dead, [&](const auto& a_entry) { return a_entry.first.spawner == a_player; });
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
				out.push_back(death);
			}
			return out;
		}

		void Clear()
		{
			said.clear();
			dead.clear();
		}

	private:
		std::map<Protocol::NpcKey, float>              said;  // each NPC's runner's last word, while it is run
		std::map<Protocol::NpcKey, Protocol::NpcDeath> dead;
	};
}
