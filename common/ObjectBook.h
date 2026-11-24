#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

// Loose objects, as the server keeps them (docs/DESIGN.md, "Loose objects"; the sister project's ObjectMovers, carried
// over). A move has one owner while it lasts: the game whose player holds the object, or threw it, or whose player,
// spell or NPC knocked it about. The server:
// - Holds: one holder at a time, the first grab wins. A hold of an object another player holds is refused and answered
//   with the holder's last hold, to that game alone; a holder who sends no hold for HOLD_TIMEOUT holds it no more.
// - Let-go: only from its holder (or from anyone while nobody holds it); the thrower then owns its flight for THROW_HOLD,
//   or until its rest.
// - Rests: the first rest of a move stands for SETTLED. A rest from another game within that is refused and answered
//   with the standing one, to that game alone, so its copy goes where the session has it. A rest of an object someone
//   holds, or someone else's throw still flies, is refused with no answer (their word is on its way).
// - Places: where each object came to rest last, for whoever joins or loads later: at most MAX_PLACES, the one moved
//   longest ago forgotten first; one player may add MAX_NEW_PLACES_PER_PLAYER new objects a run, so made-up ones can't
//   push out the host's. A kGone ends the object's place.
// - Owed: a let-go or a rest past a player's budget is kept (MAX_OWED a player, the oldest dropped and counted) and
//   passed on from the server's loop once the budget has room: an object's rest is never lost to a burst.
// - Moves: what is held or thrown now, at most MAX_MOVES; past that the ones whose hold and throw are over are
//   forgotten, and a new object past it is refused and counted (a move that never gets its rest is not kept for ever).
// No game and no network here: tests/ObjectTests.cpp.
namespace ObjectBook
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;
	using Key = Protocol::ObjectKey;

	inline constexpr auto        HOLD_TIMEOUT = 2s;
	inline constexpr auto        THROW_HOLD = 12s;
	inline constexpr auto        SETTLED = 3s;
	inline constexpr std::size_t MAX_PLACES = 2048;
	inline constexpr std::size_t MAX_NEW_PLACES_PER_PLAYER = 512;
	inline constexpr std::size_t MAX_OWED = 256;
	inline constexpr std::size_t MAX_MOVES = 1024;

	enum class Verdict
	{
		kPass,      // passed on to the others (a hold, a let-go, a rest, a gone)
		kHeld,      // another player holds it: refused; a hold is answered with theirs
		kThrown,    // another player's throw of it still flies: refused, their rest is coming
		kSettled,   // another player's rest of it stands: refused and answered with that rest
		kFull,      // a new object past the player's MAX_NEW_PLACES_PER_PLAYER: passed on, but no place is kept
		kRefused,   // a drop's key (not shared yet), a player of 0, or a message that does not hold
	};

	struct Answer
	{
		Verdict                             verdict = Verdict::kRefused;
		std::optional<Protocol::ObjectHold> hold;     // kHeld, to a hold: the holder's last hold
		std::uint32_t                       holder = 0;
		std::optional<Protocol::ObjectRest> rest;     // kSettled: the rest that stands (the session's word)

		[[nodiscard]] bool Passes() const { return verdict == Verdict::kPass || verdict == Verdict::kFull; }
	};

	class Book
	{
	public:
		Answer Hold(std::uint32_t a_player, const Protocol::ObjectHold& a_hold, Clock::time_point a_now)
		{
			if (a_player == 0 || a_hold.key.kind != Protocol::ObjectKind::kRef || !Protocol::ObjectHoldHolds(a_hold)) {
				return Refuse();
			}
			const auto found = MoveOf(a_hold.key, a_now);
			if (!found) {
				return Refuse();
			}
			auto& move = *found;
			if (HeldByOther(move, a_player, a_now)) {
				++counts.refusedHolds;
				return Answer{ .verdict = Verdict::kHeld, .hold = move.lastHold, .holder = move.holder };
			}
			move.holder = a_player;
			move.lastHold = a_hold;
			move.lastHoldAt = a_now;
			move.thrower = 0;
			++counts.holds;
			return Answer{ .verdict = Verdict::kPass };
		}

		Answer Rest(std::uint32_t a_player, const Protocol::ObjectRest& a_rest, Clock::time_point a_now)
		{
			if (a_player == 0 || a_rest.key.kind != Protocol::ObjectKind::kRef || !Protocol::ObjectRestHolds(a_rest)) {
				return Refuse();
			}
			if (a_rest.kind == Protocol::RestKind::kGone) {
				if (const auto it = moves.find(a_rest.key); it != moves.end() && HeldByOther(it->second, a_player, a_now)) {
					++counts.refusedRests;
					return Answer{ .verdict = Verdict::kHeld };
				}
				Forget(a_rest.key);
				moves.erase(a_rest.key);
				++counts.gone;
				return Answer{ .verdict = Verdict::kPass };
			}
			const auto found = MoveOf(a_rest.key, a_now);
			if (!found) {
				return Refuse();
			}
			auto& move = *found;
			if (HeldByOther(move, a_player, a_now)) {
				++counts.refusedRests;
				return Answer{ .verdict = Verdict::kHeld };
			}
			switch (a_rest.kind) {
			case Protocol::RestKind::kLetGo:
				move.holder = 0;
				move.thrower = a_player;
				move.thrownAt = a_now;
				++counts.letGos;
				return Answer{ .verdict = Verdict::kPass };
			case Protocol::RestKind::kGone:
			case Protocol::RestKind::kRest:
				break;
			}
			if (move.thrower != 0 && move.thrower != a_player && a_now - move.thrownAt < THROW_HOLD) {
				++counts.refusedRests;
				return Answer{ .verdict = Verdict::kThrown };
			}
			const auto place = places.find(a_rest.key);
			if (place != places.end() && place->second.by != a_player && a_now - place->second.at < SETTLED) {
				++counts.refusedRests;
				return Answer{ .verdict = Verdict::kSettled, .rest = place->second.rest };
			}
			moves.erase(a_rest.key);  // the move is over: nobody holds or throws it (its rest stands in places)
			++counts.rests;
			return Keep(a_player, a_rest, a_now);
		}

		// A player left: what they held or threw is nobody's now (the others' games end a hold that stops coming).
		void Leave(std::uint32_t a_player)
		{
			for (auto& [key, move] : moves) {
				if (move.holder == a_player) {
					move.holder = 0;
				}
				if (move.thrower == a_player) {
					move.thrower = 0;
				}
			}
			owed.erase(a_player);
		}

		// Every place kept, the one moved longest ago first, as the session's word (sender 0).
		[[nodiscard]] std::vector<Protocol::ObjectRest> All() const
		{
			std::vector<std::pair<Clock::time_point, Protocol::ObjectRest>> sorted;
			sorted.reserve(places.size());
			for (const auto& [key, place] : places) {
				sorted.emplace_back(place.at, place.rest);
			}
			std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a_a, const auto& a_b) { return a_a.first < a_b.first; });
			std::vector<Protocol::ObjectRest> out;
			out.reserve(sorted.size());
			for (auto& [at, rest] : sorted) {
				out.push_back(rest);
			}
			return out;
		}

		// A new session: nothing is kept (but the counts).
		void Clear()
		{
			moves.clear();
			places.clear();
			added.clear();
			owed.clear();
		}

		// Past a player's budget: kept for the loop (Owes, NextOwed), the oldest dropped past MAX_OWED.
		void Owe(std::uint32_t a_player, const Protocol::ObjectRest& a_rest)
		{
			auto& list = owed[a_player];
			if (list.size() >= MAX_OWED) {
				list.pop_front();
				++counts.owedDropped;
			}
			list.push_back(a_rest);
		}
		[[nodiscard]] bool Owes(std::uint32_t a_player) const
		{
			const auto it = owed.find(a_player);
			return it != owed.end() && !it->second.empty();
		}
		bool NextOwed(std::uint32_t a_player, Protocol::ObjectRest& a_out)
		{
			const auto it = owed.find(a_player);
			if (it == owed.end() || it->second.empty()) {
				return false;
			}
			a_out = it->second.front();
			it->second.pop_front();
			return true;
		}

		[[nodiscard]] std::size_t Places() const { return places.size(); }
		[[nodiscard]] std::size_t Moves() const { return moves.size(); }
		[[nodiscard]] std::optional<Protocol::ObjectRest> PlaceOf(const Key& a_key) const
		{
			const auto it = places.find(a_key);
			return it != places.end() ? std::optional{ it->second.rest } : std::nullopt;
		}
		// Who holds a_key now (0: nobody).
		[[nodiscard]] std::uint32_t HolderOf(const Key& a_key, Clock::time_point a_now) const
		{
			const auto it = moves.find(a_key);
			return it != moves.end() && it->second.holder != 0 && a_now - it->second.lastHoldAt < HOLD_TIMEOUT ? it->second.holder : 0;
		}

		struct Counts
		{
			std::uint64_t holds = 0, letGos = 0, rests = 0, gone = 0, refusedHolds = 0, refusedRests = 0, refused = 0;
			std::uint64_t forgotten = 0, notKept = 0, owedDropped = 0, movesForgotten = 0, movesFull = 0;
		};
		Counts counts;

	private:
		struct Move
		{
			std::uint32_t                       holder = 0;
			std::optional<Protocol::ObjectHold> lastHold;
			Clock::time_point                   lastHoldAt{};
			std::uint32_t                       thrower = 0;
			Clock::time_point                   thrownAt{};
		};
		struct Place
		{
			Protocol::ObjectRest rest;
			std::uint32_t        by = 0;
			Clock::time_point    at{};
		};

		Answer Refuse()
		{
			++counts.refused;
			return Answer{ .verdict = Verdict::kRefused };
		}

		// The move of a_key, made if new: past MAX_MOVES the moves that are over (no live hold, no throw in flight) are
		// forgotten first; none when it is still full.
		Move* MoveOf(const Key& a_key, Clock::time_point a_now)
		{
			if (const auto it = moves.find(a_key); it != moves.end()) {
				return &it->second;
			}
			if (moves.size() >= MAX_MOVES) {
				counts.movesForgotten += std::erase_if(moves, [&](const auto& a_entry) {
					const auto& move = a_entry.second;
					const bool held = move.holder != 0 && a_now - move.lastHoldAt < HOLD_TIMEOUT;
					const bool thrown = move.thrower != 0 && a_now - move.thrownAt < THROW_HOLD;
					return !held && !thrown;
				});
				if (moves.size() >= MAX_MOVES) {
					++counts.movesFull;
					return nullptr;
				}
			}
			return &moves[a_key];
		}

		static bool HeldByOther(const Move& a_move, std::uint32_t a_player, Clock::time_point a_now)
		{
			return a_move.holder != 0 && a_move.holder != a_player && a_now - a_move.lastHoldAt < HOLD_TIMEOUT;
		}

		Answer Keep(std::uint32_t a_player, const Protocol::ObjectRest& a_rest, Clock::time_point a_now)
		{
			auto it = places.find(a_rest.key);
			if (it == places.end()) {
				if (added[a_player] >= MAX_NEW_PLACES_PER_PLAYER) {
					++counts.notKept;
					return Answer{ .verdict = Verdict::kFull };
				}
				if (places.size() >= MAX_PLACES) {
					ForgetOldest();
				}
				++added[a_player];
				it = places.emplace(a_rest.key, Place{}).first;
			}
			it->second.rest = a_rest;
			it->second.rest.vx = it->second.rest.vy = it->second.rest.vz = 0.0f;
			it->second.by = a_player;
			it->second.at = a_now;
			return Answer{ .verdict = Verdict::kPass };
		}

		void Forget(const Key& a_key)
		{
			places.erase(a_key);
		}

		void ForgetOldest()
		{
			auto oldest = places.begin();
			for (auto it = places.begin(); it != places.end(); ++it) {
				if (it->second.at < oldest->second.at) {
					oldest = it;
				}
			}
			if (oldest != places.end()) {
				places.erase(oldest);
				++counts.forgotten;
			}
		}

		std::map<Key, Move>                                    moves;
		std::map<Key, Place>                                   places;
		std::map<std::uint32_t, std::size_t>                   added;  // new objects each player put in places this run
		std::map<std::uint32_t, std::deque<Protocol::ObjectRest>> owed;
	};
}
