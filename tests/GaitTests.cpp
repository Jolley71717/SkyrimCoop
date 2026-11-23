#include "Test.h"

#include "game/Gait.h"
#include "game/NpcRules.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

// What a stand-in's graph is told so its legs do what its friend's do (src/game/Gait.h).

namespace
{
	Protocol::PlayerState Moving(float a_speed, std::uint8_t a_flags = Protocol::kNone)
	{
		Protocol::PlayerState state;
		state.speed = a_speed;
		state.flags = a_flags;
		return state;
	}

	bool Has(const Gait::Step& a_step, std::string_view a_event)
	{
		return std::ranges::find(a_step.events, a_event) != a_step.events.end();
	}
}

TEST("gait: a friend who starts walking starts the stand-in's legs, and one who stops stops them")
{
	const auto start = Gait::Next({}, Moving(150.0f), false);
	CHECK(start.events == std::vector<std::string_view>{ "moveStart" });
	CHECK(start.shown.moving);
	CHECK(start.speed == 150.0f);

	// Walking on: nothing new to say, the speed set.
	const auto on = Gait::Next(start.shown, Moving(160.0f), false);
	CHECK(on.events.empty());
	CHECK(on.speed == 160.0f);

	const auto stop = Gait::Next(on.shown, Moving(0.0f), false);
	CHECK(stop.events == std::vector<std::string_view>{ "moveStop" });
	CHECK(stop.speed == 0.0f);
}

TEST("gait: sprinting, sneaking and a jump each start and end with their own event")
{
	auto step = Gait::Next({}, Moving(450.0f, Protocol::kSprinting), false);
	CHECK(Has(step, "moveStart"));
	CHECK(Has(step, "SprintStart"));
	step = Gait::Next(step.shown, Moving(300.0f), false);
	CHECK(step.events == std::vector<std::string_view>{ "SprintStop" });

	step = Gait::Next(step.shown, Moving(300.0f, Protocol::kSneaking), false);
	CHECK(step.events == std::vector<std::string_view>{ "SneakStart" });
	step = Gait::Next(step.shown, Moving(300.0f), false);
	CHECK(step.events == std::vector<std::string_view>{ "SneakStop" });

	// A jump on the move is the directional one, a jump standing still the standing one; landing ends both.
	step = Gait::Next(step.shown, Moving(300.0f, Protocol::kJumping), false);
	CHECK(step.events == std::vector<std::string_view>{ "JumpDirectionalStart" });
	step = Gait::Next(step.shown, Moving(300.0f), false);
	CHECK(step.events == std::vector<std::string_view>{ "JumpLand" });
	const auto standing = Gait::Next({}, Moving(0.0f, Protocol::kJumping), false);
	CHECK(standing.events == std::vector<std::string_view>{ "JumpStandingStart" });
}

TEST("gait: sneaking is said before the move starts, so the sneaking walk is the one started")
{
	const auto step = Gait::Next({}, Moving(100.0f, Protocol::kSneaking), false);
	REQUIRE(step.events.size() == 2);
	CHECK(step.events[0] == "SneakStart");
	CHECK(step.events[1] == "moveStart");
}

TEST("gait: a friend whose states stopped coming stands: legs, sprint and jump stop, a sneak stays")
{
	auto step = Gait::Next({}, Moving(450.0f, Protocol::kSprinting | Protocol::kSneaking), false);
	step = Gait::Next(step.shown, Moving(450.0f, Protocol::kSprinting | Protocol::kSneaking | Protocol::kJumping), false);
	const auto held = Gait::Next(step.shown, Moving(450.0f, Protocol::kSprinting | Protocol::kSneaking | Protocol::kJumping), true);
	CHECK(Has(held, "moveStop"));
	CHECK(Has(held, "SprintStop"));
	CHECK(Has(held, "JumpLand"));
	CHECK(!Has(held, "SneakStop"));
	CHECK(held.speed == 0.0f);
	CHECK(!held.shown.moving);
	CHECK(held.shown.sneaking);
}

TEST("gait: the direction is the fraction of a turn the state packed")
{
	auto state = Moving(200.0f);
	state.direction = 64;  // a quarter turn: strafing right
	CHECK(Gait::Next({}, state, false).direction == 0.25f);
}

// ---- in water (protocol 11), and what a graph turned away ----

namespace
{
	using Results = std::vector<std::pair<std::string_view, bool>>;

	// Every event of a step taken, but those named.
	Results Answer(const Gait::Step& a_step, std::initializer_list<std::string_view> a_refused = {})
	{
		Results results;
		for (const auto event : a_step.events) {
			results.emplace_back(event, std::ranges::find(a_refused, event) == a_refused.end());
		}
		return results;
	}

	using Events = std::vector<std::string_view>;
}

TEST("gait: a friend who swims out into the water is shown swimming first, and the walk is started again in the water's mode")
{
	auto walking = Gait::Next({}, Moving(150.0f), false);
	walking.shown = Gait::Settle(walking.shown, Answer(walking));
	const auto in = Gait::Next(walking.shown, Moving(120.0f, Protocol::kSwimming), false);
	CHECK((in.events == Events{ "SwimStart", "moveStart" }));  // the swim's own stroke, though it was walking already
	CHECK(in.shown.swimming);
	CHECK(in.shown.moving);

	// Swimming on: nothing new.
	CHECK(Gait::Next(in.shown, Moving(120.0f, Protocol::kSwimming), false).events.empty());

	// Out onto the shore, still walking: the water left, the walk on land started again.
	const auto out = Gait::Next(in.shown, Moving(150.0f), false);
	CHECK((out.events == Events{ "SwimStop", "moveStart" }));
	CHECK(!out.shown.swimming);

	// Into the water standing (it waded in and stopped): the swim alone.
	const auto still = Gait::Next({}, Moving(0.0f, Protocol::kSwimming), false);
	CHECK((still.events == Events{ "SwimStart" }));
}

TEST("gait: a sprint is said again in the new mode; a jump never shows in the water")
{
	auto step = Gait::Next({}, Moving(450.0f, Protocol::kSprinting), false);
	step = Gait::Next(step.shown, Moving(450.0f, Protocol::kSprinting | Protocol::kSwimming), false);
	CHECK((step.events == Events{ "SwimStart", "moveStart", "SprintStart" }));

	// A sneak is no walk mode: it is not said again.
	step = Gait::Next({}, Moving(100.0f, Protocol::kSneaking), false);
	step = Gait::Next(step.shown, Moving(100.0f, Protocol::kSneaking | Protocol::kSwimming), false);
	CHECK((step.events == Events{ "SwimStart", "moveStart" }));

	// A state that says both in the air and swimming (a dive's first frame) shows the swim, never a jump.
	const auto splash = Gait::Next({}, Moving(300.0f, Protocol::kJumping | Protocol::kSwimming), false);
	CHECK(!splash.shown.inAir);
	CHECK(!Has(splash, "JumpDirectionalStart"));
	CHECK(!Has(splash, "JumpStandingStart"));
}

TEST("gait: a friend whose states stopped coming in the water stays in it, its legs stopped")
{
	const auto step = Gait::Next({}, Moving(150.0f, Protocol::kSwimming), false);
	const auto held = Gait::Next(step.shown, Moving(150.0f, Protocol::kSwimming), true);
	CHECK((held.events == Events{ "moveStop" }));
	CHECK(held.shown.swimming);
}

TEST("gait: an actor whose race does not walk (a fish) is never told to swim: its graph is always in the water")
{
	const auto fish = Gait::Next({}, Moving(150.0f, Protocol::kSwimming), false, false);
	CHECK((fish.events == Events{ "moveStart" }));
	CHECK(fish.shown.swimming);  // what it does is still known (its actor state's bits, `swim`)
	CHECK(Gait::Next(fish.shown, Moving(150.0f), false, false).events.empty());
}

TEST("gait: an event the graph turns away is sent again alone, not with the frame's others")
{
	// A sneak and a move started together; the move turned away.
	const auto step = Gait::Next({}, Moving(100.0f, Protocol::kSneaking), false);
	const auto settled = Gait::Settle(step.shown, Answer(step, { "moveStart" }));
	CHECK(settled.sneaking);
	CHECK(!settled.moving);
	const auto again = Gait::Next(settled, Moving(100.0f, Protocol::kSneaking), false);
	CHECK((again.events == Events{ "moveStart" }));

	// Each kind rolls back its own part: a sprint's start, a sneak's end, a jump's start and its landing.
	auto shown = Gait::Settle({ .moving = true, .sprinting = true }, Results{ { "SprintStart", false } });
	CHECK(!shown.sprinting);
	CHECK(shown.moving);
	shown = Gait::Settle({}, Results{ { "SneakStop", false } });
	CHECK(shown.sneaking);
	shown = Gait::Settle({}, Results{ { "JumpLand", false } });
	CHECK(shown.inAir);
	shown = Gait::Settle({ .inAir = true }, Results{ { "JumpStandingStart", false } });
	CHECK(!shown.inAir);
	// All taken: as the step said.
	CHECK(Gait::Settle(step.shown, Answer(step)) == step.shown);
}

TEST("gait: a copy's graph that turns a walk event away is not asked again (a creature may have no sneak at all)")
{
	const auto step = Gait::Next({}, Moving(100.0f, Protocol::kSneaking), false);
	const auto settled = Gait::Settle(step.shown, Answer(step, { "SneakStart" }), false);
	CHECK(settled == step.shown);
	CHECK(Gait::Next(settled, Moving(100.0f, Protocol::kSneaking), false).events.empty());
}

TEST("gait: a swim event turned away is asked again three times for one change, then given up until the next")
{
	for (const bool retry : { true, false }) {  // a stand-in's and a copy's alike
		Gait::Shown shown;
		int         asked = 0;
		bool        gaveUp = false;
		for (int frame = 0; frame < 10; ++frame) {
			const auto step = Gait::Next(shown, Moving(0.0f, Protocol::kSwimming), false);
			if (Has(step, "SwimStart")) {
				++asked;
			}
			shown = Gait::Settle(step.shown, Answer(step, { "SwimStart" }), retry);
			gaveUp = gaveUp || shown.swimGivenUp;
		}
		CHECK(asked == Gait::SWIM_TRIES);
		CHECK(gaveUp);
		CHECK(shown.swimming);  // taken as shown: nothing more asked
		CHECK(shown.swimRefused == 0);

		// The next change (out of the water) has its own tries.
		const auto out = Gait::Next(shown, Moving(0.0f), false);
		CHECK((out.events == Events{ "SwimStop" }));
		const auto once = Gait::Settle(out.shown, Answer(out, { "SwimStop" }), retry);
		CHECK(once.swimming);  // still shown in it: asked again
		CHECK(once.swimRefused == 1);
		CHECK(!once.swimGivenUp);
	}

	// Taken at the second try: no give-up, no tries left over.
	Gait::Shown shown;
	auto        step = Gait::Next(shown, Moving(0.0f, Protocol::kSwimming), false);
	shown = Gait::Settle(step.shown, Answer(step, { "SwimStart" }));
	CHECK(!shown.swimming);
	step = Gait::Next(shown, Moving(0.0f, Protocol::kSwimming), false);
	CHECK((step.events == Events{ "SwimStart" }));
	shown = Gait::Settle(step.shown, Answer(step));
	CHECK(shown.swimming);
	CHECK(shown.swimRefused == 0);
	CHECK(!shown.swimGivenUp);
	CHECK(Gait::Next(shown, Moving(0.0f, Protocol::kSwimming), false).events.empty());
}

TEST("gait: the actor state's movement bits follow what is shown, and swimming as the friend does")
{
	const auto bits = [](const Protocol::PlayerState& a_state, bool a_swimEvents = true) {
		return Gait::Bits(Gait::Next({}, a_state, false, a_swimEvents), a_state);
	};
	CHECK((bits(Moving(150.0f)) == Gait::MoveBits{ .walking = true, .forward = true }));
	CHECK((bits(Moving(350.0f)) == Gait::MoveBits{ .running = true, .forward = true }));
	CHECK((bits(Moving(500.0f, Protocol::kSprinting)) == Gait::MoveBits{ .sprinting = true, .forward = true }));
	CHECK((bits(Moving(0.0f, Protocol::kSneaking)) == Gait::MoveBits{ .sneaking = true }));
	CHECK(bits(Moving(0.0f)) == Gait::MoveBits{});
	// Swimming whatever the graph took (a fish's, or a swim given up on).
	CHECK((bits(Moving(100.0f, Protocol::kSwimming), false) == Gait::MoveBits{ .walking = true, .forward = true, .swimming = true }));
}

TEST("gait: a swimmer under water is pitched as it swims, held to 75 degrees; at the surface or on land it is level")
{
	const auto radians = [](float a_degrees) { return a_degrees * std::numbers::pi_v<float> / 180.0f; };
	auto       state = Moving(100.0f, Protocol::kSwimming | Protocol::kUnderwater);
	state.pitch = -40;  // looking down: diving
	REQUIRE(Gait::DivePitch(state).has_value());
	CHECK(std::fabs(*Gait::DivePitch(state) - radians(40.0f)) < 1e-5f);  // down is positive in the game's x
	state.pitch = 30;
	CHECK(std::fabs(*Gait::DivePitch(state) - -radians(30.0f)) < 1e-5f);
	state.pitch = -90;
	CHECK(std::fabs(*Gait::DivePitch(state) - radians(Gait::MAX_DIVE_DEGREES)) < 1e-5f);
	state.pitch = 90;
	CHECK(std::fabs(*Gait::DivePitch(state) - -radians(Gait::MAX_DIVE_DEGREES)) < 1e-5f);

	state.flags = Protocol::kSwimming;  // at the surface
	CHECK(!Gait::DivePitch(state));
	state.flags = Protocol::kUnderwater;  // the flag with no swim (never sent so): level
	CHECK(!Gait::DivePitch(state));
	state.flags = Protocol::kNone;
	CHECK(!Gait::DivePitch(state));
}

TEST("gait: a swimmer the water's update moved off its height is put back; on land, or within 8 units, it is not")
{
	const auto swimming = Moving(100.0f, Protocol::kSwimming);
	CHECK(Gait::Refloat(swimming, -200.0f, -150.0f));  // floated up 50
	CHECK(Gait::Refloat(swimming, -200.0f, -230.0f));
	CHECK(!Gait::Refloat(swimming, -200.0f, -195.0f));
	CHECK(!Gait::Refloat(swimming, -200.0f, -200.0f));
	CHECK(Gait::Refloat(swimming, -200.0f, std::numeric_limits<float>::quiet_NaN()));
	CHECK(!Gait::Refloat(Moving(100.0f), -200.0f, -100.0f));  // on land the engine's own update stands
}

TEST("gait: no event Gait sends is one a copy also plays from its runner's actions (it would come twice)")
{
	for (const auto event : { Gait::MOVE_START, Gait::MOVE_STOP, Gait::SNEAK_START, Gait::SNEAK_STOP, Gait::SPRINT_START, Gait::SPRINT_STOP, Gait::JUMP_MOVING,
			 Gait::JUMP_STANDING, Gait::JUMP_LAND, Gait::SWIM_START, Gait::SWIM_STOP }) {
		CHECK(!NpcRules::Replayed(event));
	}
}
