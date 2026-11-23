#pragma once

#include "Protocol.h"

#include <compare>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

// Who rides what, as the server keeps it (docs/DESIGN.md, "Riding"). Each game says its own player's ride and the rides
// of the NPCs it runs (kRides) whenever one changes; the server passes on a player's only from that player's game and an
// NPC's only from its runner's, each change once (the same ride again from the same game is not passed on; from a new
// runner it is, that runner named), and gives a joiner every ride the session has. A player who leaves takes every ride their game said with them: the others hear each one end.
// Past a sender's budget a change is still kept and owed (Owe): the server's loop passes the newest of it on once the
// budget has room, so an end of a ride is never lost. No game and no network here: tests/RideTests.cpp.
namespace RideBook
{
	// Who rides: a player (npc none), or an NPC (player 0).
	struct Rider
	{
		std::uint32_t   player = 0;
		Protocol::NpcKey npc;
		auto operator<=>(const Rider&) const = default;
	};

	[[nodiscard]] inline Rider RiderOf(const Protocol::Ride& a_ride)
	{
		return a_ride.player != 0 ? Rider{ a_ride.player, {} } : Rider{ 0, a_ride.rider };
	}

	// The end of a rider's ride, as passed on.
	[[nodiscard]] inline Protocol::Ride Off(const Rider& a_rider)
	{
		return Protocol::Ride{ .player = a_rider.player, .rider = a_rider.npc, .mount = {}, .on = false };
	}

	class Book
	{
	public:
		// The rides a_sender's game says, of which those it may say (its own player's; an NPC's when a_runs(that NPC):
		// it runs it) and that change what the book holds are kept and returned, to pass on. A ride that ends one the book
		// does not hold changes nothing (nobody was told it began).
		template <class Runs>
		std::vector<Protocol::Ride> Take(std::uint32_t a_sender, const std::vector<Protocol::Ride>& a_rides, Runs&& a_runs)
		{
			std::vector<Protocol::Ride> out;
			for (const auto& ride : a_rides) {
				if (a_sender == 0 || !Protocol::RideHolds(ride) || (ride.player != 0 ? ride.player != a_sender : !a_runs(ride.rider))) {
					++refused;
					continue;
				}
				const auto rider = RiderOf(ride);
				const auto held = rides.find(rider);
				if (ride.on) {
					// The same ride from the same game is no change. From another game it is: the NPC was handed to that
					// game (its runner now), so the book holds it as that game's (a leave of the old runner does not end
					// it) and every other game, the old runner's too, hears it with its new sender.
					if (held != rides.end() && held->second.ride == ride && held->second.sender == a_sender) {
						continue;
					}
					rides[rider] = Held{ ride, a_sender };
					out.push_back(ride);
				} else {
					if (held == rides.end()) {
						continue;
					}
					rides.erase(held);
					out.push_back(Off(rider));
				}
			}
			return out;
		}

		// Every ride the session has now, for a joiner.
		[[nodiscard]] std::vector<Protocol::Ride> All() const
		{
			std::vector<Protocol::Ride> out;
			out.reserve(rides.size());
			for (const auto& [rider, held] : rides) {
				out.push_back(held.ride);
			}
			return out;
		}

		// A player left: every ride their game said ends (their own, and those of the NPCs it ran), returned to pass on.
		std::vector<Protocol::Ride> Leave(std::uint32_t a_player)
		{
			std::vector<Protocol::Ride> out;
			for (auto it = rides.begin(); it != rides.end();) {
				if (it->second.sender == a_player || it->first.player == a_player) {
					out.push_back(Off(it->first));
					it = rides.erase(it);
				} else {
					++it;
				}
			}
			owed.erase(a_player);
			return out;
		}

		// A change of a_sender's held back by its budget: its newest state goes later (Owed).
		void Owe(std::uint32_t a_sender, const Protocol::Ride& a_ride) { owed[a_sender].insert(RiderOf(a_ride)); }

		[[nodiscard]] bool Owes(std::uint32_t a_sender) const
		{
			const auto it = owed.find(a_sender);
			return it != owed.end() && !it->second.empty();
		}

		// The newest state of one ride a_sender owes (taken off what it owes): the ride held, or its end. None owed: false.
		bool NextOwed(std::uint32_t a_sender, Protocol::Ride& a_out)
		{
			const auto it = owed.find(a_sender);
			if (it == owed.end() || it->second.empty()) {
				return false;
			}
			const auto rider = *it->second.begin();
			it->second.erase(it->second.begin());
			const auto held = rides.find(rider);
			a_out = held != rides.end() ? held->second.ride : Off(rider);
			return true;
		}

		// A new session: nothing of the last one's rides.
		void Clear()
		{
			rides.clear();
			owed.clear();
		}

		[[nodiscard]] std::size_t Count() const { return rides.size(); }
		[[nodiscard]] std::uint64_t Refused() const { return refused; }

	private:
		struct Held
		{
			Protocol::Ride ride;
			std::uint32_t  sender = 0;
		};
		std::map<Rider, Held>                        rides;
		std::map<std::uint32_t, std::set<Rider>>     owed;
		std::uint64_t                                refused = 0;
	};
}
