#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

// The speed and direction each stand-in's legs go by, as its graph is to be given them.
//
// The engine does not keep what is set on an actor's graph for Speed and Direction: as the graph updates, it pulls both
// from the actor through one channel each (BSTAnimationGraphDataChannel<Actor, float, ActorSpeedChannel> and
// <..., ActorDirectionChannel>), from the movement speed and direction in the actor's process. Those are set each frame
// by the actor's movement job from its movement controller's desired movement, which is none for an actor whose AI
// never runs: so a stand-in's graph read 0 whatever was set on it, and its legs stood still while it slid along
// (docs/DESIGN.md, "The other players"). The channels' polls are hooked (RemotePlayers): the stand-in's own update
// writes its friend's values here, and the channel gives them to the graph in place of the engine's.
//
// The polls run as graphs update, on the game's job threads, so the table has its own lock, held only while a value is
// copied in or out: nothing of the game is called under it. No game here, so it is unit-tested.
namespace GaitDrive
{
	// What a stand-in's graph is given: Speed in the graph's own units (its friend's graph's, as sent), Direction as a
	// fraction of a turn from where it faces. A copy of a flyer in the air (its runner's state says it flies, or its body
	// crashes) also has its graph's flight values driven (Protocol::NpcGraph: the channels TurnDelta, PitchDelta and
	// TargetSpeed); with flight false those stay the engine's.
	struct Values
	{
		float speed = 0.0f;
		float direction = 0.0f;
		bool  flight = false;
		float turnDelta = 0.0f;
		float pitchDelta = 0.0f;
		float targetSpeed = 0.0f;

		bool operator==(const Values&) const = default;
	};

	// A stand-in whose update stopped writing its values (its update no longer runs: taken away, unloaded) goes back to
	// the engine's after this long.
	inline constexpr std::chrono::milliseconds FRESH{ 500 };

	template <class Key, class Clock = std::chrono::steady_clock>
	class Table
	{
	public:
		using time_point = typename Clock::time_point;

		// a_id: the actor's form id. A game reuses an actor's address for another actor; only the one it was set for
		// reads it.
		void Set(Key a_key, std::uint32_t a_id, Values a_values, time_point a_now)
		{
			std::scoped_lock l{ lock };
			auto&            entry = entries[a_key];
			flights += (a_values.flight ? 1 : 0) - (entry.values.flight ? 1 : 0);
			entry = Entry{ a_id, a_values, a_now };
		}

		// The values for this actor, or none: never set, set for another actor at that address, or not set for FRESH.
		[[nodiscard]] std::optional<Values> Get(Key a_key, std::uint32_t a_id, time_point a_now) const
		{
			std::scoped_lock l{ lock };
			const auto       it = entries.find(a_key);
			if (it == entries.end() || it->second.id != a_id || a_now - it->second.at > FRESH) {
				return std::nullopt;
			}
			return it->second.values;
		}

		// The values set at this address and the form id they were set for, without the actor's: for a caller that holds an
		// address it is not sure is an actor's (a channel whose actor's place is not known for sure), which may read it as an
		// actor only once the table holds it.
		[[nodiscard]] std::optional<std::pair<std::uint32_t, Values>> Held(Key a_key, time_point a_now) const
		{
			std::scoped_lock l{ lock };
			const auto       it = entries.find(a_key);
			if (it == entries.end() || a_now - it->second.at > FRESH) {
				return std::nullopt;
			}
			return std::pair{ it->second.id, it->second.values };
		}

		// Whether any entry has flight values: the flight channels' polls (every actor's graph's) skip the table while none
		// does. Read without the lock.
		[[nodiscard]] bool AnyFlight() const { return flights.load(std::memory_order_relaxed) > 0; }

		void Erase(Key a_key)
		{
			std::scoped_lock l{ lock };
			if (const auto it = entries.find(a_key); it != entries.end()) {
				flights -= it->second.values.flight ? 1 : 0;
				entries.erase(it);
			}
		}

		void Clear()
		{
			std::scoped_lock l{ lock };
			entries.clear();
			flights = 0;
		}

		[[nodiscard]] std::size_t Size() const
		{
			std::scoped_lock l{ lock };
			return entries.size();
		}

	private:
		struct Entry
		{
			std::uint32_t id = 0;
			Values        values;
			time_point    at{};
		};

		mutable std::mutex                 lock;
		std::unordered_map<Key, Entry>     entries;
		std::atomic<std::ptrdiff_t>        flights{ 0 };  // entries with flight values
	};
}
