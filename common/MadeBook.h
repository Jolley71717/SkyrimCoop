#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// The NPCs the games made as they ran, as the server keeps them (docs/DESIGN.md, "Run-time NPCs"). A quest's created
// alias, a world encounter, a script's PlaceAtMe: each game makes its own, under its own run-time id, of what is one NPC
// in the story (Lydia's escort dragon, a Thalmor patrol, the courier). The game that makes one says what made it
// (kNpcMades); the server answers:
// - the twin of another game's (kTwin): a game made the same one first, and it is still there: the same quest's alias,
//   or the same NPC (or leveled NPC) placed at the same spot, or, its leveled roll aside, a body of the same kind placed
//   at the very spot (Twins). The later game shows the first one's through its own NPC: nothing is made twice;
// - shared (kShared): the maker runs it, as its spawn (common/NpcBook.h), and the others place a copy of it;
// - full (kFull): past MAX_PER_PLAYER or MAX_SESSION; asked again later.
// The first is the one the server heard first, so two games making one at once agree which shows which. Two of one alias
// shared apart (made where each player was) that come near each other later are made one (Merges): the later one gives
// way. An entry goes when its NPC dies, its maker lets go of it (unloaded there) and when its maker leaves.
// No game and no network here: tests/MadeTests.cpp.
namespace MadeBook
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// An alias's NPC is the same one in two games while theirs stands this near ours (one run of a quest: two runs far
	// apart are each their own).
	inline constexpr float PAIR_RANGE = 4096.0f;
	// Without an alias: the same NPC (or leveled NPC) placed this near (a script placing at a marker in each game), or a
	// body of the same kind placed this near (one leveled list's two rolls at one marker).
	inline constexpr float SAME_SPOT = 512.0f;
	inline constexpr float BODY_SPOT = 128.0f;
	// Made NPCs a player's game shares at once, and the session (a fight's bandits, a dragon, a patrol or two).
	inline constexpr std::size_t MAX_PER_PLAYER = 32;
	inline constexpr std::size_t MAX_SESSION = 128;
	// Two shared ones of one alias are looked at for a merge this often.
	inline constexpr auto MERGE_EVERY = 1s;

	[[nodiscard]] inline bool Placed(const Protocol::MadeSource& a_s)
	{
		return a_s.cell != 0 || a_s.worldspace != 0;
	}

	[[nodiscard]] inline std::optional<float> Apart(const Protocol::MadeSource& a_a, const Protocol::MadeSource& a_b)
	{
		if (!Placed(a_a) || a_a.cell != a_b.cell || a_a.worldspace != a_b.worldspace) {
			return std::nullopt;
		}
		const float dx = a_a.x - a_b.x;
		const float dy = a_a.y - a_b.y;
		const float dz = a_a.z - a_b.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	// Whether the NPC a_new says is the one a_old says, made by another game: in one place, of one kind of body, and
	// either both by one quest's alias near each other, or both not by an alias and placed as the same thing near each
	// other, or at the very spot. An alias of a quest the reader lacks (quest 0) is no match: two such are any two.
	[[nodiscard]] inline bool Twins(const Protocol::MadeSource& a_old, const Protocol::MadeSource& a_new)
	{
		const auto apart = Apart(a_old, a_new);
		if (!apart || a_old.body == 0 || a_old.body != a_new.body || a_old.ByAlias() != a_new.ByAlias()) {
			return false;
		}
		if (a_old.ByAlias()) {
			return a_old.quest != 0 && a_old.quest == a_new.quest && a_old.alias == a_new.alias && *apart <= PAIR_RANGE;
		}
		const bool same = (a_old.made != 0 && a_old.made == a_new.made) || (a_old.pick != 0 && a_old.pick == a_new.pick);
		return *apart <= (same ? SAME_SPOT : BODY_SPOT);
	}

	struct Answer
	{
		Protocol::MadeVerdict verdict = Protocol::MadeVerdict::kRefused;
		Protocol::NpcKey      twin;
		bool operator==(const Answer&) const = default;
	};

	// A merge: later gives way to earlier (it shows earlier through its own NPC from now).
	struct Merge
	{
		Protocol::NpcKey later;
		Protocol::NpcKey earlier;
		bool operator==(const Merge&) const = default;
	};

	class Book
	{
	public:
		// A made NPC a_player's game says (a_made.key its spawn). Said again (its maker loaded it again, or moved it), it
		// stays as it was heard first. One said gone is no report (Gone).
		Answer Report(std::uint32_t a_player, const Protocol::NpcMade& a_made)
		{
			if (a_player == 0 || a_made.key.spawner != a_player || !Protocol::MadeHolds(a_made) || a_made.source.Gone()) {
				return {};
			}
			if (const auto it = entries.find(a_made.key); it != entries.end()) {
				it->second.source = a_made.source;
				return { Protocol::MadeVerdict::kShared, {} };
			}
			const Entry* first = nullptr;
			for (const auto& [key, entry] : entries) {
				if (key.spawner != a_player && Twins(entry.source, a_made.source) && (!first || entry.order < first->order)) {
					first = &entry;
				}
			}
			if (first) {
				return { Protocol::MadeVerdict::kTwin, first->key };
			}
			if (entries.size() >= MAX_SESSION || Of(a_player) >= MAX_PER_PLAYER) {
				return { Protocol::MadeVerdict::kFull, {} };
			}
			entries.emplace(a_made.key, Entry{ a_made.key, a_made.source, ++order });
			return { Protocol::MadeVerdict::kShared, {} };
		}

		// Where its maker's game has it now (its runner's state).
		void Moved(const Protocol::NpcKey& a_key, const Protocol::PlayerState& a_state)
		{
			const auto it = entries.find(a_key);
			if (it == entries.end() || (a_state.cell == 0 && a_state.worldspace == 0) || !a_state.IsFinite()) {
				return;
			}
			auto& s = it->second.source;
			s.cell = a_state.cell;
			s.worldspace = a_state.worldspace;
			s.x = a_state.x;
			s.y = a_state.y;
			s.z = a_state.z;
		}

		// Two of one quest's alias, shared by two games, that stand as twins now: the later of the two gives way (taken out
		// of the book; its maker is told it is the earlier's twin). One that gives way gives way once, to the earliest.
		[[nodiscard]] std::vector<Merge> Merges()
		{
			std::vector<Merge> out;
			for (const auto& [key, later] : entries) {
				if (!later.source.ByAlias()) {
					continue;
				}
				const Entry* first = nullptr;
				for (const auto& [otherKey, other] : entries) {
					if (otherKey.spawner != key.spawner && other.order < later.order && Twins(other.source, later.source) &&
						std::ranges::none_of(out, [&](const Merge& a_m) { return a_m.later == otherKey; }) && (!first || other.order < first->order)) {
						first = &other;
					}
				}
				if (first) {
					out.push_back({ key, first->key });
				}
			}
			for (const auto& merge : out) {
				entries.erase(merge.later);
			}
			return out;
		}

		// Its NPC died, or its maker let go of it.
		bool Erase(const Protocol::NpcKey& a_key) { return entries.erase(a_key) != 0; }

		// Its maker says it is gone from its world (a_made.source.Gone()): true when it was shared (the others hear it).
		bool Gone(std::uint32_t a_player, const Protocol::NpcMade& a_made)
		{
			return a_made.key.spawner == a_player && a_made.source.Gone() && entries.erase(a_made.key) != 0;
		}

		// A player left: every one their game made goes.
		std::vector<Protocol::NpcKey> Leave(std::uint32_t a_player)
		{
			std::vector<Protocol::NpcKey> out;
			for (auto it = entries.begin(); it != entries.end();) {
				if (it->first.spawner == a_player) {
					out.push_back(it->first);
					it = entries.erase(it);
				} else {
					++it;
				}
			}
			return out;
		}

		// Every one shared, in the order they were heard (for a joiner).
		[[nodiscard]] std::vector<Protocol::NpcMade> All() const
		{
			std::vector<const Entry*> sorted;
			sorted.reserve(entries.size());
			for (const auto& [key, entry] : entries) {
				sorted.push_back(&entry);
			}
			std::ranges::sort(sorted, [](const Entry* a_a, const Entry* a_b) { return a_a->order < a_b->order; });
			std::vector<Protocol::NpcMade> out;
			out.reserve(sorted.size());
			for (const auto* entry : sorted) {
				out.push_back({ entry->key, entry->source });
			}
			return out;
		}

		[[nodiscard]] std::optional<Protocol::MadeSource> Find(const Protocol::NpcKey& a_key) const
		{
			const auto it = entries.find(a_key);
			return it == entries.end() ? std::nullopt : std::optional{ it->second.source };
		}

		[[nodiscard]] std::size_t Of(std::uint32_t a_player) const
		{
			return static_cast<std::size_t>(std::ranges::count_if(entries, [&](const auto& a_entry) { return a_entry.first.spawner == a_player; }));
		}
		[[nodiscard]] std::size_t Size() const { return entries.size(); }
		void                      Clear() { entries.clear(); }

	private:
		struct Entry
		{
			Protocol::NpcKey     key;
			Protocol::MadeSource source;
			std::uint64_t        order = 0;  // the server heard it as the order-th
		};
		std::map<Protocol::NpcKey, Entry> entries;
		std::uint64_t                     order = 0;
	};
}
