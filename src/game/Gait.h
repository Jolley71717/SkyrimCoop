#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string_view>
#include <vector>

// What a stand-in's animation graph is told so its legs do what its friend's do: the graph's own variables (Speed,
// Direction) each frame, and its events when the friend starts or stops moving, sneaking, sprinting or jumping. The
// names are those of Skyrim's character behavior graph as the game's own movement sends them; whether each is taken is
// logged on the rig (RemotePlayers' `graph` probe). No game here, so it is unit-tested.
namespace Gait
{
	// Below this many units a second a friend stands: the graph is told to stop.
	constexpr float MOVING = 10.0f;

	// What the stand-in's graph was last told it does.
	struct Shown
	{
		bool moving = false;
		bool sneaking = false;
		bool sprinting = false;
		bool inAir = false;

		bool operator==(const Shown&) const = default;
	};

	struct Step
	{
		std::vector<std::string_view> events;  // to send, in this order
		float                         speed = 0.0f;
		float                         direction = 0.0f;  // fraction of a turn, as the graph's Direction holds it
		Shown                         shown;             // what it shows after them
	};

	// The events that take a graph showing a_was to what a_state says, and the variables to set. a_held: the friend's
	// states stopped coming and the stand-in stands (StateBuffer's held), so its legs stop too.
	[[nodiscard]] inline Step Next(const Shown& a_was, const Protocol::PlayerState& a_state, bool a_held)
	{
		Step step;
		const bool inAir = (a_state.flags & Protocol::kJumping) != 0;
		Shown      now{
				 .moving = !a_held && a_state.speed > MOVING,
				 .sneaking = (a_state.flags & Protocol::kSneaking) != 0,
				 .sprinting = !a_held && (a_state.flags & Protocol::kSprinting) != 0,
				 .inAir = inAir && !a_held,
		};
		// Sneaking first: it changes which walk the move events start.
		if (now.sneaking != a_was.sneaking) {
			step.events.push_back(now.sneaking ? "SneakStart" : "SneakStop");
		}
		if (now.inAir && !a_was.inAir) {
			step.events.push_back(now.moving ? "JumpDirectionalStart" : "JumpStandingStart");
		} else if (!now.inAir && a_was.inAir) {
			step.events.push_back("JumpLand");
		}
		if (now.moving != a_was.moving) {
			step.events.push_back(now.moving ? "moveStart" : "moveStop");
		}
		if (now.sprinting != a_was.sprinting) {
			step.events.push_back(now.sprinting ? "SprintStart" : "SprintStop");
		}
		step.speed = now.moving ? a_state.speed : 0.0f;
		step.direction = Protocol::UnpackDirection(a_state.direction);
		step.shown = now;
		return step;
	}
}
