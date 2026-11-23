#pragma once

#include "Protocol.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

// Riding, the rules (docs/DESIGN.md, "Riding"; the game's side is src/game/Riding.cpp, the server's common/RideBook.h).
// A game says who of its own rides what: its player, and each NPC it runs. Every other game seats the actor that shows
// that rider (a friend's stand-in, a copy of the NPC) on the actor that shows the mount (its copy of the horse) through
// the engine's own way onto a mount, so the engine carries the rider in the saddle and plays the getting on and off:
// - Seat: a rider that should be on a mount and is not is put on in rounds: the mount activated by the rider (what the
//   game does when a player presses the button on a horse), the console's activate a moment later if that did not take,
//   then the engine's quick put-on; each round waits for the rider's and the mount's 3D, and 3 rounds 5 s apart are
//   tried before it is given up (said in `ride`). One on another mount, or on one it should not be on, is got off.
// - Ours: what our player and the NPCs we run ride, said again only when it changes.
// No game here: tests/RideTests.cpp.
namespace RideRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// A round starts once rider and mount have had their 3D this long (a seat on a skeleton still loading does not hold).
	inline constexpr auto READY_AFTER = 1s;
	// In a round: the console's activate this long after the first try, the quick put-on this long after, and the round
	// over this long after it began.
	inline constexpr auto CONSOLE_AFTER = 2s;
	inline constexpr auto QUICK_AFTER = 4s;
	inline constexpr auto ROUND_LENGTH = 6s;
	inline constexpr auto ROUND_GAP = 5s;
	inline constexpr int  MAX_ROUNDS = 3;
	// A rider is got off no more often than this (the engine's getting off takes a moment to show).
	inline constexpr auto DISMOUNT_EVERY = 1s;
	// A seated rider this far from its mount (units, the two positions apart) for SADDLE_HOLD_AFTER is put back on the
	// saddle by the update hook: the engine carries a rider it seated, but not always one whose AI never runs.
	inline constexpr float SADDLE_FAR = 150.0f;
	inline constexpr auto  SADDLE_HOLD_AFTER = 1s;

	// What the engine says of one rider this game shows (a stand-in, a copy) and the mount it should be on.
	struct SeatFacts
	{
		bool wanted = false;     // its player or runner says it rides, on a mount this game shows
		bool ready = false;      // rider and mount are here with their 3D, for READY_AFTER
		bool onWanted = false;   // the engine has it on that mount
		bool onOther = false;    // the engine has it on another mount, or on one while it should be on foot
		bool busy = false;       // the engine is getting it on or off now: wait
	};

	enum class Act : std::uint8_t
	{
		kNone,
		kActivate,  // the mount activated by the rider (TESObjectREFR::ActivateRef)
		kConsole,   // the console's `<mount>.activate <rider>`
		kQuick,     // the engine's quick put-on (Actor::PutActorOnMountQuick)
		kDismount,  // off whatever it is on (Actor::StopInteractingQuick)
	};

	[[nodiscard]] constexpr std::string_view ActName(Act a_act)
	{
		switch (a_act) {
		case Act::kNone:
			return "none";
		case Act::kActivate:
			return "activate";
		case Act::kConsole:
			return "console";
		case Act::kQuick:
			return "quick";
		case Act::kDismount:
			return "dismount";
		}
		return "?";
	}

	// One rider's seat: what to do each tick to have it where its player or runner says it is.
	class Seat
	{
	public:
		Act Step(const SeatFacts& a_facts, Clock::time_point a_now)
		{
			seated = a_facts.wanted && a_facts.onWanted;
			if (a_facts.onOther || (!a_facts.wanted && a_facts.onWanted)) {
				Reset();
				if (a_facts.busy || (lastDismount && a_now - *lastDismount < DISMOUNT_EVERY)) {
					return Act::kNone;
				}
				lastDismount = a_now;
				++dismounts;
				return Act::kDismount;
			}
			if (!a_facts.wanted) {
				Reset();
				givenUp = false;
				rounds = 0;
				return Act::kNone;
			}
			if (seated) {
				if (!seatedSince) {
					seatedSince = a_now;
					seatedBy = tried;
				}
				Reset();
				rounds = 0;
				givenUp = false;
				return Act::kNone;
			}
			seatedSince.reset();
			if (!a_facts.ready || a_facts.busy || givenUp) {
				return Act::kNone;
			}
			if (!roundStart) {
				if (nextRound && a_now < *nextRound) {
					return Act::kNone;
				}
				roundStart = a_now;
				step = 1;
				return Tried(Act::kActivate);
			}
			const auto in = a_now - *roundStart;
			if (step == 1 && in >= CONSOLE_AFTER) {
				step = 2;
				return Tried(Act::kConsole);
			}
			if (step == 2 && in >= QUICK_AFTER) {
				step = 3;
				return Tried(Act::kQuick);
			}
			if (in >= ROUND_LENGTH) {
				roundStart.reset();
				step = 0;
				++rounds;
				if (rounds >= MAX_ROUNDS) {
					givenUp = true;
				} else {
					nextRound = a_now + ROUND_GAP;
				}
			}
			return Act::kNone;
		}

		[[nodiscard]] bool Seated() const { return seated; }
		[[nodiscard]] bool GivenUp() const { return givenUp; }
		[[nodiscard]] int  Rounds() const { return rounds; }
		[[nodiscard]] Act  LastTried() const { return tried; }
		[[nodiscard]] Act  SeatedBy() const { return seatedBy; }  // the last try before it was seen seated (none: it was on already)
		[[nodiscard]] std::uint32_t Dismounts() const { return dismounts; }

	private:
		Act Tried(Act a_act)
		{
			tried = a_act;
			return a_act;
		}
		void Reset()
		{
			roundStart.reset();
			nextRound.reset();
			step = 0;
		}

		bool                             seated = false;
		bool                             givenUp = false;
		int                              rounds = 0;
		int                              step = 0;
		std::optional<Clock::time_point> roundStart;
		std::optional<Clock::time_point> nextRound;
		std::optional<Clock::time_point> lastDismount;
		std::optional<Clock::time_point> seatedSince;
		Act                              tried = Act::kNone;
		Act                              seatedBy = Act::kNone;
		std::uint32_t                    dismounts = 0;
	};

	// A seated rider put back on its saddle: SADDLE_FAR from its mount for SADDLE_HOLD_AFTER.
	class SaddleWatch
	{
	public:
		bool Step(float a_gap, Clock::time_point a_now)
		{
			if (!(a_gap > SADDLE_FAR)) {
				farSince.reset();
				return false;
			}
			farSince = farSince.value_or(a_now);
			if (a_now - *farSince < SADDLE_HOLD_AFTER) {
				return false;
			}
			++holds;
			return true;
		}
		[[nodiscard]] std::uint64_t Holds() const { return holds; }

	private:
		std::optional<Clock::time_point> farSince;
		std::uint64_t                    holds = 0;
	};

	// Who of ours rides, as this game said it last: each change is said once (an end too, of a ride it said began).
	// Rider: Protocol::Ride's player (ours) or an NPC key.
	class Said
	{
	public:
		struct Rider
		{
			std::uint32_t    player = 0;
			Protocol::NpcKey npc;
			auto operator<=>(const Rider&) const = default;
		};

		// a_now: every rider of ours and what it rides now (mount id 0: on foot, or on a mount that is shared with no
		// game). Returns the rides to send: each that began or changed mount, and the end of each said before and not on
		// now (gone from the list, or on foot).
		std::vector<Protocol::Ride> Changes(const std::map<Rider, Protocol::NpcKey>& a_now)
		{
			std::vector<Protocol::Ride> out;
			for (const auto& [rider, mount] : a_now) {
				const auto said = mounts.find(rider);
				if (mount.id == 0) {
					continue;
				}
				if (said != mounts.end() && said->second == mount) {
					continue;
				}
				mounts[rider] = mount;
				out.push_back(Protocol::Ride{ .player = rider.player, .rider = rider.npc, .mount = mount, .on = true });
			}
			for (auto it = mounts.begin(); it != mounts.end();) {
				const auto now = a_now.find(it->first);
				if (now == a_now.end() || now->second.id == 0) {
					out.push_back(Protocol::Ride{ .player = it->first.player, .rider = it->first.npc, .mount = {}, .on = false });
					it = mounts.erase(it);
				} else {
					++it;
				}
			}
			return out;
		}

		// A new session, or a world gone: nothing said yet (an NPC handed to another game is said by that game).
		void Forget(const Rider& a_rider) { mounts.erase(a_rider); }
		void Clear() { mounts.clear(); }
		[[nodiscard]] std::size_t Count() const { return mounts.size(); }

	private:
		std::map<Rider, Protocol::NpcKey> mounts;
	};

	// A ride a friend's game says, as this game takes it: a player's only from that player, an NPC's only from the game
	// that runs it now (a_runner: who this game was told runs it; 0 nobody).
	[[nodiscard]] inline bool Believed(const Protocol::Ride& a_ride, std::uint32_t a_sender, std::uint32_t a_runner)
	{
		if (a_ride.player != 0) {
			return a_sender == 0 || a_ride.player == a_sender;
		}
		return a_sender == 0 || a_runner == a_sender;
	}
}
