#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// How a stand-in leaves the world: in two steps, never both in one frame (RemotePlayers' graveyard).
//
// First it is disabled, so the game takes its 3D, collision and movement out at a point of its own (the disable queues
// a task, run later in the game's task pool). Only once that is done (its 3D gone, a second and some ticks later) is it
// marked for deletion, which takes it out of its cell. Both at once crashed both games of the first two-player test
// (2026-11-22), at the same address: the disable's own task ran on an actor the deletion had already taken out of its
// cell, and read the cell it no longer had (a null read under MovementControllerNPC, in the task pool). On one game a
// save had taken its stand-in out, on the other a player had left. The sister project learned the same in Fallout 4 (its
// DeferredDelete.h, carried in idea).
//
// And a stand-in whose 3D is still being loaded is not disabled until it is in, or a long while has passed: the loader
// builds the graph of the actor it loads.
//
// No game here, so it is unit-tested.
namespace Retire
{
	using Duration = std::chrono::steady_clock::duration;

	// Longest wait for a stand-in's 3D to come in before it is disabled anyway (one whose cell is not loaded never has
	// any). Also how long a disabled one may keep its 3D before the log says so.
	inline constexpr auto LOAD_WAIT = std::chrono::seconds(15);
	// Marked for deletion no sooner than this after it was disabled, and only once this many ticks have run since: the
	// disable's task runs in the same task pool as the tick, so a tick that ran is one the pool ran.
	inline constexpr auto          DELETE_AFTER = std::chrono::seconds(1);
	inline constexpr std::uint64_t DELETE_AFTER_TICKS = 30;

	enum class Step
	{
		kWait,
		kDisable,
		kDelete,
	};

	struct Facts
	{
		bool                    has3D = false;
		Duration                sincePlaced{};
		std::optional<Duration> sinceDisabled;  // since it was disabled on its way out (none: not yet)
		std::uint64_t           ticksSinceDisabled = 0;
	};

	// What to do with a stand-in on its way out, now.
	[[nodiscard]] constexpr Step Next(const Facts& a_facts)
	{
		if (!a_facts.sinceDisabled) {
			return a_facts.has3D || a_facts.sincePlaced >= LOAD_WAIT ? Step::kDisable : Step::kWait;
		}
		const bool settled = !a_facts.has3D && *a_facts.sinceDisabled >= DELETE_AFTER && a_facts.ticksSinceDisabled >= DELETE_AFTER_TICKS;
		return settled ? Step::kDelete : Step::kWait;
	}

	// Disabled long since and its 3D still there: the game has not run the disable. It is left disabled (never deleted
	// with its 3D in); the log says so once.
	[[nodiscard]] constexpr bool Stuck(const Facts& a_facts)
	{
		return a_facts.sinceDisabled && a_facts.has3D && *a_facts.sinceDisabled >= LOAD_WAIT;
	}
}
