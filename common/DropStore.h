#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

// The server's book of items players gave each other by dropping them (Protocol::DroppedItem, docs/DESIGN.md "Dropped
// items"): what lies in the session for anyone to take, and what was taken by whom. The server is what decides who gets
// an item: a take is granted to the first player who asks while it lies there, and to nobody after, so two players
// pressing Take at once can't both have it. Each game takes an item only once its grant comes (src/game/DropSync).
//
// A new drop is an offer at first, to its dropper's game alone: it lies there for the others only once that game says it
// has taken the item it dropped out of its world (Confirm), so nobody can take an item that is still in its dropper's
// world. An offer is never taken or listed, and goes when its dropper picked the item up again (Keep) or leaves (Left).
//
// What a game's save holds is the other half: a save from before a player dropped something has it again, and a save
// from before they took something lacks it. Each game says what its world holds after its welcome and after every load
// (Protocol::DropRecord), and Reconcile makes the book agree with it: what that world holds (an item it took, or its own
// drop it never took out of its world) is that player's, lying nowhere; what the book had them hold but their world
// lacks lies there again. The one case left is an item two worlds hold, as when someone else took a drop and its owner
// loads a save from before dropping it: both have it then (counted, dupRisk).
//
// A player who leaves takes their drops still lying with them (Left): each is withdrawn for everyone, and their game
// puts it back in its world as it leaves the session (the server has no file of its own to keep them in past the
// session; the host's game is the server, so they would be lost with it).
//
// Ported from the sister project's DropStore, without its session file and its characters' keys. Kept free of the
// server's loop so it can be tested.
class DropStore
{
public:
	struct Limits
	{
		std::size_t lying = 1024;                       // items lying in the session (and offered) at once
		std::size_t taken = Protocol::MAX_DROP_RECORD;  // takes remembered (for a taker's reload); the oldest go first
		std::size_t perOwner = 256;                     // items one player's game may have lying at once
	};

	enum class Outcome : std::uint8_t
	{
		kAdded,
		kRepeat,     // the same drop reported again (its nonce): the one kept is the answer
		kFull,       // the session has Limits::lying
		kOwnerFull,  // this player has Limits::perOwner lying
		kInvalid,    // no nonce, no base, nowhere
	};

	struct Added
	{
		Outcome                              outcome = Outcome::kInvalid;
		std::optional<Protocol::DroppedItem> drop;  // as kept: kAdded, and kRepeat of one offered or lying
		bool                                 lying = false;  // kRepeat: it lies there already (not an offer)
	};

	// What a game's record changed (Reconcile).
	struct Reconciled
	{
		std::vector<Protocol::DropGone>    withdrawn;  // lying no more: in this player's world (takerId 0: their own, back with them)
		std::vector<Protocol::DroppedItem> returned;   // not with their taker: lying there again
		std::vector<Protocol::DroppedItem> confirmed;  // their offers their world took out after all: lying there now
		std::vector<Protocol::DropGone>    unoffered;  // their offers their world still has: gone (for them alone to hear)
		std::size_t                        dupRisk = 0;  // their world holds one someone else holds too
	};

	DropStore() = default;
	explicit DropStore(const Limits& a_limits) :
		limits(a_limits)
	{}

	// A player's drop (a_item as their game reported it; its ids are set here): an offer to them until they Confirm it.
	Added Add(std::uint32_t a_owner, Protocol::DroppedItem a_item)
	{
		if (a_item.nonce == 0 || a_item.base == 0 || a_owner == 0 || (a_item.cell == 0 && a_item.worldspace == 0)) {
			return {};
		}
		for (const auto* book : { &offers, &lying }) {
			for (const auto& [id, drop] : *book) {
				if (drop.ownerId == a_owner && drop.nonce == a_item.nonce) {
					return { Outcome::kRepeat, drop, book == &lying };
				}
			}
		}
		for (const auto& [id, drop] : taken) {
			if (drop.ownerId == a_owner && drop.nonce == a_item.nonce) {
				return { Outcome::kRepeat, std::nullopt, false };  // taken already: nothing to answer
			}
		}
		if (lying.size() + offers.size() >= limits.lying) {
			return { Outcome::kFull, std::nullopt, false };
		}
		if (LyingOf(a_owner) >= limits.perOwner) {
			return { Outcome::kOwnerFull, std::nullopt, false };
		}
		a_item.dropId = nextId++;
		a_item.ownerId = a_owner;
		a_item.takerId = 0;
		offers.emplace(a_item.dropId, a_item);
		return { Outcome::kAdded, a_item, false };
	}

	// a_owner's game took the item it dropped out of its world: the offer lies there for anyone now (to be told to all).
	std::optional<Protocol::DroppedItem> Confirm(std::uint32_t a_owner, std::uint32_t a_dropId)
	{
		const auto it = offers.find(a_dropId);
		if (it == offers.end() || it->second.ownerId != a_owner) {
			return std::nullopt;
		}
		auto drop = std::move(it->second);
		offers.erase(it);
		lying[drop.dropId] = drop;
		return drop;
	}

	// a_owner's player picked the item up again before the offer came: it never lay there.
	bool Keep(std::uint32_t a_owner, std::uint32_t a_dropId)
	{
		const auto it = offers.find(a_dropId);
		if (it == offers.end() || it->second.ownerId != a_owner) {
			return false;
		}
		offers.erase(it);
		return true;
	}

	// a_player's game asks to take the item: granted while it lies there, to whoever asks first (never an offer).
	std::optional<Protocol::DropGone> Take(std::uint32_t a_player, std::uint32_t a_dropId)
	{
		const auto it = lying.find(a_dropId);
		if (it == lying.end() || a_player == 0) {
			return std::nullopt;
		}
		auto drop = std::move(it->second);
		lying.erase(it);
		drop.takerId = a_player;
		Remember(std::move(drop));
		return Protocol::DropGone{ a_dropId, a_player };
	}

	// a_player's game could not finish a take granted to it: the item lies there again.
	std::optional<Protocol::DroppedItem> GiveBack(std::uint32_t a_player, std::uint32_t a_dropId)
	{
		const auto it = taken.find(a_dropId);
		if (it == taken.end() || it->second.takerId != a_player) {
			return std::nullopt;
		}
		return Lay(it);
	}

	// a_owner left the session: their offers go (their game gives those items back to its world as it leaves), and so do
	// their drops still lying, each remembered as theirs (their game puts it back in its world as it leaves; a record of a
	// world that lacks it after all, a game that crashed first, lays it down again). The drops withdrawn, for everyone.
	std::vector<Protocol::DropGone> Left(std::uint32_t a_owner)
	{
		std::erase_if(offers, [&](const auto& a_entry) { return a_entry.second.ownerId == a_owner; });
		std::vector<Protocol::DropGone> out;
		for (auto it = lying.begin(); it != lying.end();) {
			if (it->second.ownerId != a_owner) {
				++it;
				continue;
			}
			auto drop = std::move(it->second);
			it = lying.erase(it);
			drop.takerId = a_owner;
			out.push_back({ drop.dropId, 0 });
			Remember(std::move(drop));
		}
		return out;
	}

	// What a_player's world holds (their game's DropRecord) against the book, in session a_session (see the class).
	Reconciled Reconcile(std::uint32_t a_player, const Protocol::DropRecord& a_record, std::uint64_t a_session)
	{
		Reconciled out;
		// A record of another session (or none) is of a save from before this one: it holds none of its transfers.
		const bool                              ours = a_record.sessionId == a_session && a_session != 0;
		const std::unordered_set<std::uint64_t> made = ours ? std::unordered_set<std::uint64_t>(a_record.made.begin(), a_record.made.end()) :
		                                                      std::unordered_set<std::uint64_t>{};
		const std::unordered_set<std::uint32_t> took = ours ? std::unordered_set<std::uint32_t>(a_record.taken.begin(), a_record.taken.end()) :
		                                                      std::unordered_set<std::uint32_t>{};
		// Whether the player's world holds the item: one they took, or their own that never left it.
		const auto holds = [&](const Protocol::DroppedItem& a_drop) {
			return took.contains(a_drop.dropId) || (a_drop.ownerId == a_player && !made.contains(a_drop.nonce));
		};
		// Their offers: one their world took out (a confirm lost with a load) lies there now; one it still has goes.
		for (auto it = offers.begin(); it != offers.end();) {
			if (it->second.ownerId != a_player) {
				++it;
				continue;
			}
			auto drop = std::move(it->second);
			it = offers.erase(it);
			if (made.contains(drop.nonce)) {
				lying[drop.dropId] = drop;
				out.confirmed.push_back(std::move(drop));
			} else {
				out.unoffered.push_back({ drop.dropId, 0 });
			}
		}
		for (auto it = lying.begin(); it != lying.end();) {
			if (!holds(it->second)) {
				++it;
				continue;
			}
			// In their world, so it lies nowhere: their own is back with them (gone for everyone, no taker), and one they
			// took is theirs (as everyone heard when they took it).
			auto       drop = std::move(it->second);
			const bool own = !took.contains(drop.dropId);
			it = lying.erase(it);
			drop.takerId = a_player;
			out.withdrawn.push_back({ drop.dropId, own ? 0 : a_player });
			Remember(std::move(drop));
		}
		for (auto it = taken.begin(); it != taken.end();) {
			const auto& drop = it->second;
			// Not in the world of the player the book says has it: it lies there again for anyone.
			if (drop.takerId == a_player && !holds(drop)) {
				out.returned.push_back(*Lay(it++));
				continue;
			}
			if (drop.takerId != a_player && holds(drop)) {
				++out.dupRisk;  // in their world and in the holder's too
			}
			++it;
		}
		return out;
	}

	// What lies in the session now, in the order it was dropped.
	[[nodiscard]] std::vector<Protocol::DroppedItem> Lying() const
	{
		std::vector<Protocol::DroppedItem> out;
		out.reserve(lying.size());
		for (const auto& [id, drop] : lying) {
			out.push_back(drop);
		}
		return out;
	}

	[[nodiscard]] const Protocol::DroppedItem* Find(std::uint32_t a_dropId) const
	{
		const auto it = lying.find(a_dropId);
		return it != lying.end() ? &it->second : nullptr;
	}

	// Who has it (0: nobody remembered as having it).
	[[nodiscard]] std::uint32_t TakerOf(std::uint32_t a_dropId) const
	{
		const auto it = taken.find(a_dropId);
		return it == taken.end() ? 0 : it->second.takerId;
	}

	[[nodiscard]] std::size_t LyingCount() const { return lying.size(); }
	[[nodiscard]] std::size_t TakenCount() const { return taken.size(); }
	[[nodiscard]] std::size_t OfferCount() const { return offers.size(); }

	// What a_owner has lying there or offered.
	[[nodiscard]] std::size_t LyingOf(std::uint32_t a_owner) const
	{
		const auto of = [&](const auto& a_entry) { return a_entry.second.ownerId == a_owner; };
		return static_cast<std::size_t>(std::ranges::count_if(lying, of) + std::ranges::count_if(offers, of));
	}

	// A new session (nobody was in): everything goes. The numbers go on from where they were, so a record of the session
	// before (the same server run, the same session id) names no drop of this one.
	void Clear()
	{
		offers.clear();
		lying.clear();
		taken.clear();
	}

private:
	using Book = std::map<std::uint32_t, Protocol::DroppedItem>;

	// A taken item lies there again (under its own id: every game's record names it so).
	std::optional<Protocol::DroppedItem> Lay(Book::iterator a_taken)
	{
		auto drop = std::move(a_taken->second);
		taken.erase(a_taken);
		drop.takerId = 0;
		lying[drop.dropId] = drop;
		return drop;
	}

	void Remember(Protocol::DroppedItem a_drop)
	{
		while (!taken.empty() && taken.size() >= limits.taken) {
			taken.erase(taken.begin());  // the oldest: its taker's reload no longer brings it back
		}
		const auto id = a_drop.dropId;
		taken[id] = std::move(a_drop);
	}

	Limits        limits;
	Book          offers;  // not confirmed by their dropper yet: lying nowhere
	Book          lying;
	Book          taken;
	std::uint32_t nextId = 1;
};
