#include "Test.h"

#include "game/Gait.h"

#include <algorithm>

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
