#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// What an actor this game moves for another game is shown doing with its legs and body: a friend's stand-in
// (RemotePlayers) and a copy of an NPC another game runs (NpcSync) alike. Its animation graph is told the graph's own
// variables (Speed, Direction) each frame, given to it through the engine's own channels for them (GaitDrive.h), and
// its events when the one it shows starts or stops moving, sneaking, sprinting, jumping or swimming; its actor state's
// movement bits are set to match, and in water it is pitched as its friend dives. The names are those of Skyrim's
// character behavior graph as the game's own movement sends them; whether each is taken is counted (RemotePlayers'
// `standins`, `swim`, NpcSync's `npcs`) for the rig. No game here, so it is unit-tested.
namespace Gait
{
	// Below this many units a second a friend stands: the graph is told to stop.
	constexpr float MOVING = 10.0f;
	// Above this many units a second a friend on foot runs (the actor state's running bit), at or below it walks.
	constexpr float RUN_ABOVE = 200.0f;

	// The graph's events, each the one the game's own movement sends.
	inline constexpr std::string_view MOVE_START = "moveStart";
	inline constexpr std::string_view MOVE_STOP = "moveStop";
	inline constexpr std::string_view SNEAK_START = "SneakStart";
	inline constexpr std::string_view SNEAK_STOP = "SneakStop";
	inline constexpr std::string_view SPRINT_START = "SprintStart";
	inline constexpr std::string_view SPRINT_STOP = "SprintStop";
	inline constexpr std::string_view JUMP_MOVING = "JumpDirectionalStart";
	inline constexpr std::string_view JUMP_STANDING = "JumpStandingStart";
	inline constexpr std::string_view JUMP_LAND = "JumpLand";
	inline constexpr std::string_view SWIM_START = "SwimStart";
	inline constexpr std::string_view SWIM_STOP = "SwimStop";

	// A graph that turns a swim event away this many times in a row for one change is not asked again until the next
	// change (its race's graph may have no swim state: the walk goes on as it was). Every other event is asked again
	// each frame until it is taken, as before: a graph that is busy for a moment (an equip) takes it a frame later.
	constexpr std::uint8_t SWIM_TRIES = 3;

	// What the graph was last told it does.
	struct Shown
	{
		bool         moving = false;
		bool         sneaking = false;
		bool         sprinting = false;
		bool         inAir = false;
		bool         swimming = false;
		std::uint8_t swimRefused = 0;  // swim events turned away in a row for the change under way (SWIM_TRIES)
		bool         swimGivenUp = false;  // ... and that change given up on: the graph was left as it was

		bool operator==(const Shown&) const = default;
	};

	struct Step
	{
		std::vector<std::string_view> events;  // to send, in this order
		float                         speed = 0.0f;
		float                         direction = 0.0f;  // fraction of a turn, as the graph's Direction holds it
		Shown                         shown;             // what it shows after them, if each is taken (Settle)
	};

	// The events that take a graph showing a_was to what a_state says, and the variables to set. a_held: the friend's
	// states stopped coming and the stand-in stands (StateBuffer's held), so its legs stop too (in water it stays in the
	// water). a_swimEvents: the actor's race walks, so swimming is a state its graph goes into and out of; a race that
	// does not (a fish) is always in water, and its graph is told nothing of it.
	[[nodiscard]] inline Step Next(const Shown& a_was, const Protocol::PlayerState& a_state, bool a_held, bool a_swimEvents = true)
	{
		Step       step;
		const bool swimming = (a_state.flags & Protocol::kSwimming) != 0;
		const bool inAir = (a_state.flags & Protocol::kJumping) != 0;
		Shown      now{
				 .moving = !a_held && a_state.speed > MOVING,
				 .sneaking = (a_state.flags & Protocol::kSneaking) != 0,
				 .sprinting = !a_held && (a_state.flags & Protocol::kSprinting) != 0,
				 .inAir = inAir && !a_held && !swimming,
				 .swimming = swimming,
		};
		// What the rest is compared with: the graph as it was, but for a swim change, which takes it into another mode:
		// the walk and the sprint are started again in it (the water's, or the land's on the way out).
		Shown from = a_was;
		if (a_swimEvents && swimming != a_was.swimming) {
			step.events.push_back(swimming ? SWIM_START : SWIM_STOP);
			from.moving = false;
			from.sprinting = false;
			now.swimRefused = a_was.swimRefused;  // the change under way (its tries so far)
		}
		// Sneaking first (after the water): it changes which walk the move events start.
		if (now.sneaking != from.sneaking) {
			step.events.push_back(now.sneaking ? SNEAK_START : SNEAK_STOP);
		}
		if (now.inAir && !from.inAir) {
			step.events.push_back(now.moving ? JUMP_MOVING : JUMP_STANDING);
		} else if (!now.inAir && from.inAir) {
			step.events.push_back(JUMP_LAND);
		}
		if (now.moving != from.moving) {
			step.events.push_back(now.moving ? MOVE_START : MOVE_STOP);
		}
		if (now.sprinting != from.sprinting) {
			step.events.push_back(now.sprinting ? SPRINT_START : SPRINT_STOP);
		}
		step.speed = now.moving ? a_state.speed : 0.0f;
		step.direction = Protocol::UnpackDirection(a_state.direction);
		step.shown = now;
		return step;
	}

	// What the graph shows after a step's events, each taken (true) or turned away (false), in the order sent.
	// a_retry (a stand-in's): the part of it a refused walk event was to change stays as it was, so that event alone is
	// sent again on the next step (the rest are not: a refused moveStart no longer sends the sneak, the jump and the
	// sprint again, nor restarts the walk). Without it (a copy of an NPC, whose creature graph may have no sneak or
	// sprint at all) a refused walk event is counted and not asked again. A swim event turned away SWIM_TRIES times in a
	// row for one change is given up on either way (the graph is taken to show the water's state, and nothing more is
	// asked until the next change).
	[[nodiscard]] inline Shown Settle(Shown a_shown, std::span<const std::pair<std::string_view, bool>> a_results, bool a_retry = true)
	{
		for (const auto& [event, taken] : a_results) {
			if (event == SWIM_START || event == SWIM_STOP) {
				if (taken) {
					a_shown.swimRefused = 0;
					a_shown.swimGivenUp = false;
				} else if (++a_shown.swimRefused >= SWIM_TRIES) {
					a_shown.swimGivenUp = true;  // left as it is, swimming as the friend does: not asked again
				} else {
					a_shown.swimming = event != SWIM_START;
				}
				continue;
			}
			if (taken || !a_retry) {
				continue;
			}
			if (event == MOVE_START || event == MOVE_STOP) {
				a_shown.moving = event != MOVE_START;
			} else if (event == SNEAK_START || event == SNEAK_STOP) {
				a_shown.sneaking = event != SNEAK_START;
			} else if (event == SPRINT_START || event == SPRINT_STOP) {
				a_shown.sprinting = event != SPRINT_START;
			} else if (event == JUMP_MOVING || event == JUMP_STANDING) {
				a_shown.inAir = false;
			} else if (event == JUMP_LAND) {
				a_shown.inAir = true;
			}
		}
		// A change done (or given up) leaves no tries behind for the next one.
		if (a_shown.swimGivenUp || a_shown.swimRefused == 0) {
			a_shown.swimRefused = 0;
		}
		return a_shown;
	}

	// The actor state's movement bits (ActorState::actorState1) of an actor shown doing what a_state says, as a_step
	// shows it: what the engine's own code reads of it (its sounds, its footsteps, what NPCs make of it).
	struct MoveBits
	{
		bool sneaking = false;
		bool sprinting = false;
		bool running = false;
		bool walking = false;
		bool forward = false;
		bool swimming = false;

		bool operator==(const MoveBits&) const = default;
	};

	[[nodiscard]] inline MoveBits Bits(const Step& a_step, const Protocol::PlayerState& a_state)
	{
		const auto& shown = a_step.shown;
		return {
			.sneaking = shown.sneaking,
			.sprinting = shown.sprinting,
			.running = shown.moving && !shown.sprinting && a_state.speed > RUN_ABOVE,
			.walking = shown.moving && a_state.speed <= RUN_ABOVE,
			.forward = shown.moving,
			.swimming = (a_state.flags & Protocol::kSwimming) != 0,  // what it does, whatever its graph took
		};
	}

	// ---- in water ----

	// The pitch a swimmer under water dives or rises at, as far as it is shown (a friend looking straight down swims down
	// steeply, but no body is drawn standing on its head).
	inline constexpr float MAX_DIVE_DEGREES = 75.0f;

	// The pitch an actor shown swimming under water is turned to, in the game's radians (down positive, as an actor's
	// angle x holds it): the one it swims at (a player's is where they look, which is where they swim). None at the
	// surface or out of the water: its body is level there.
	[[nodiscard]] inline std::optional<float> DivePitch(const Protocol::PlayerState& a_state)
	{
		constexpr auto UNDER = Protocol::kSwimming | Protocol::kUnderwater;
		if ((a_state.flags & UNDER) != UNDER) {
			return std::nullopt;
		}
		const float degrees = std::clamp(static_cast<float>(a_state.pitch), -MAX_DIVE_DEGREES, MAX_DIVE_DEGREES);
		return -degrees * std::numbers::pi_v<float> / 180.0f;
	}

	// The water's own pull (a swimmer floats up to the surface) must not take an actor from the depth it is shown at:
	// one whose height moved more than this in the engine's update is put back where it is shown (and counted).
	inline constexpr float REFLOAT_DRIFT = 8.0f;

	// An actor shown swimming is placed again after the engine's update (it is placed before it, as always) when the
	// update took it this far off the height it was placed at.
	[[nodiscard]] inline bool Refloat(const Protocol::PlayerState& a_state, float a_zPlaced, float a_zAfter)
	{
		return (a_state.flags & Protocol::kSwimming) != 0 && !(std::abs(a_zAfter - a_zPlaced) <= REFLOAT_DRIFT);
	}
}
