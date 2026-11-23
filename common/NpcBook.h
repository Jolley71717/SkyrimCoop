#pragma once

#include "Protocol.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

// Who runs each NPC, as the server keeps it (docs/DESIGN.md, "NPCs"). Each NPC near a player is run by one game, its AI
// included, and shown as a copy in the others; a game asks for the ones it has loaded (kNpcClaims) and the server
// answers with who runs them (kNpcOwners). The rules are F4Multiplayer's (its docs/DESIGN.md, "Shared NPC AI"):
// - nobody runs it: the first game to ask has it;
// - a game whose player fights or talks to it takes it over, so that player's blows land on the real NPC with no wait,
//   unless it changed hands, or its runner's own player fought it, in the last TAKEOVER_COOLDOWN: two players fighting
//   one NPC leave it with the first instead of trading it, and its AI starting over, every few seconds;
// - an NPC that follows a player goes to that player's game, unless it follows the runner's player too (two players
//   who each hired the same follower: the first keeps it);
// - a spawn (an NPC one player's game made as it ran: a summon) is only ever its spawner's;
// - a game lets go of an NPC it no longer has loaded, or whose player went far; a player who leaves lets go of all of
//   theirs (their spawns are gone with them).
// No game and no network here: tests/NpcTests.cpp.
namespace NpcBook
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr auto TAKEOVER_COOLDOWN = 5s;

	enum class Outcome
	{
		kGranted,   // it is the claimant's now: everyone is told
		kKept,      // the claimant ran it already: nothing to tell
		kRefused,   // someone else runs it: the claimant is told who
		kReleased,  // nobody runs it now: everyone is told
		kIgnored,   // a release of an NPC the claimant didn't run, or a claim past MAX_NPCS: nothing to tell
	};

	class Book
	{
	public:
		Outcome Claim(std::uint32_t a_player, const Protocol::NpcClaim& a_claim, Clock::time_point a_now)
		{
			using Why = Protocol::ClaimWhy;
			const auto it = entries.find(a_claim.key);
			if (a_claim.why == Why::kRelease) {
				if (it == entries.end() || it->second.owner != a_player) {
					return Outcome::kIgnored;
				}
				entries.erase(it);
				return Outcome::kReleased;
			}
			// A spawn is its spawner's game's alone: no other game has the NPC to run.
			if (a_claim.key.IsSpawn() && a_claim.key.spawner != a_player) {
				return Outcome::kRefused;
			}
			if (it == entries.end()) {
				if (entries.size() >= Protocol::MAX_NPCS) {
					return Outcome::kIgnored;
				}
				entries.emplace(a_claim.key, Entry{
												 .owner = a_player,
												 .since = a_now,
												 .fought = a_claim.why == Why::kFight ? std::optional{ a_now } : std::nullopt,
												 .follows = a_claim.why == Why::kFollower,
											 });
				return Outcome::kGranted;
			}
			auto& entry = it->second;
			if (entry.owner == a_player) {
				if (a_claim.why == Why::kFight) {
					entry.fought = a_now;
				}
				if (a_claim.why == Why::kFollower) {
					entry.follows = true;
				}
				return Outcome::kKept;
			}
			if (a_claim.key.IsSpawn() || a_now - entry.since < TAKEOVER_COOLDOWN) {
				return Outcome::kRefused;
			}
			switch (a_claim.why) {
			case Why::kFight:
				if (entry.follows || (entry.fought && a_now - *entry.fought < TAKEOVER_COOLDOWN)) {
					return Outcome::kRefused;
				}
				break;
			case Why::kFollower:
				if (entry.follows) {
					return Outcome::kRefused;
				}
				break;
			default:
				return Outcome::kRefused;
			}
			entry = Entry{
				.owner = a_player,
				.since = a_now,
				.fought = a_claim.why == Why::kFight ? std::optional{ a_now } : std::nullopt,
				.follows = a_claim.why == Why::kFollower,
			};
			return Outcome::kGranted;
		}

		// A player left: every NPC they ran, now run by nobody (their spawns among them: gone).
		std::vector<Protocol::NpcKey> Leave(std::uint32_t a_player)
		{
			std::vector<Protocol::NpcKey> out;
			for (auto it = entries.begin(); it != entries.end();) {
				if (it->second.owner == a_player) {
					out.push_back(it->first);
					it = entries.erase(it);
				} else {
					++it;
				}
			}
			return out;
		}

		[[nodiscard]] std::uint32_t Owner(const Protocol::NpcKey& a_key) const
		{
			const auto it = entries.find(a_key);
			return it == entries.end() ? 0 : it->second.owner;
		}

		[[nodiscard]] bool Runs(std::uint32_t a_player, const Protocol::NpcKey& a_key) const { return a_player != 0 && Owner(a_key) == a_player; }

		// Every NPC someone runs, for a joiner.
		[[nodiscard]] std::vector<Protocol::NpcOwner> All() const
		{
			std::vector<Protocol::NpcOwner> out;
			out.reserve(entries.size());
			for (const auto& [key, entry] : entries) {
				out.push_back({ key, entry.owner });
			}
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return entries.size(); }
		void                      Clear() { entries.clear(); }

	private:
		struct Entry
		{
			std::uint32_t                    owner = 0;
			Clock::time_point                since{};   // when it came to this owner
			std::optional<Clock::time_point> fought;    // the owner's player last fought it (their game's kFight)
			bool                             follows = false;  // it follows the owner's player (their kFollower)
		};
		std::map<Protocol::NpcKey, Entry> entries;
	};
}
