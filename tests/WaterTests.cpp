#include "Test.h"

#include "Protocol.h"
#include "game/WaterRules.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// Players and NPCs in water (src/game/WaterRules.h): when a swimmer is under, where the rig finds water to swim in, and
// what our player's graph was told around the water.

using namespace std::chrono_literals;

TEST("water: a swimmer is under when the engine says so or when nine tenths of it is; one not swimming never is")
{
	using WaterRules::Under;
	CHECK(Under(true, true, 0.0f));
	CHECK(Under(true, false, 0.95f));
	CHECK(Under(true, false, WaterRules::SUBMERGED_UNDER));
	CHECK(!Under(true, false, 0.6f));  // at the surface: its head out
	CHECK(!Under(true, false, std::numeric_limits<float>::quiet_NaN()));
	CHECK(!Under(false, true, 1.0f));  // walking on a lake's bed (a mod's water breathing): not a swimmer
}

namespace
{
	// A shore along x = 1000: land at 0 west of it; east of it the water's surface at 0 and the bed sloping down 1 unit
	// a unit, so the water is deep enough to swim in from x = 1160.
	std::optional<float> Shore(float a_x, float)
	{
		return a_x <= 1000.0f ? std::nullopt : std::optional<float>{ a_x - 1000.0f };
	}
}

TEST("water: the nearest point deep enough to swim in is found on the grid, and how many points were read")
{
	std::size_t looked = 0;
	const auto  spot = WaterRules::FindSpot(0.0f, 0.0f, 2048.0f, Shore, &looked);
	REQUIRE(spot.has_value());
	// The first grid line with deep water SPOT_AROUND west of it too: past 1160 + 192 = 1352 (11 x 128). 1280, deep
	// itself, is not: a walker stopping short of it, on the shore's side, would wade.
	CHECK(spot->x == 1408.0f);
	CHECK(spot->y == 0.0f);  // straight east: the nearest of that line
	CHECK(spot->depth == 408.0f);
	CHECK(spot->distance == 1408.0f);
	CHECK(spot->looked == looked);
	CHECK(looked > 0);

	// From a point in the water: there.
	const auto here = WaterRules::FindSpot(1500.0f, 300.0f, 1024.0f, Shore);
	REQUIRE(here.has_value());
	CHECK(here->x == 1500.0f);
	CHECK(here->y == 300.0f);
	CHECK(here->distance == 0.0f);
}

TEST("water: no water within the radius is none, and a search never looks past the farthest it may")
{
	std::size_t looked = 0;
	CHECK(!WaterRules::FindSpot(0.0f, 0.0f, 1024.0f, Shore, &looked));  // the water starts 1160 away
	CHECK(looked > 0);

	// Dry land everywhere: none, every point within the radius read once.
	const auto dry = [](float, float) { return std::optional<float>{}; };
	CHECK(!WaterRules::FindSpot(0.0f, 0.0f, 256.0f, dry, &looked));
	CHECK(looked == 13);  // the grid points within 2 steps of the middle: 1 + 4 + 4 + 4

	// Shallow water (a stream): not deep enough.
	const auto stream = [](float, float) { return std::optional<float>{ 40.0f }; };
	CHECK(!WaterRules::FindSpot(0.0f, 0.0f, 512.0f, stream));

	// A radius past the farthest is held to it; a radius that is no number looks only where it stands.
	std::size_t far = 0;
	CHECK(!WaterRules::FindSpot(0.0f, 0.0f, 1e9f, dry, &far));
	std::size_t limit = 0;
	CHECK(!WaterRules::FindSpot(0.0f, 0.0f, WaterRules::MAX_RADIUS, dry, &limit));
	CHECK(far == limit);
	CHECK(!WaterRules::FindSpot(0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), dry, &looked));
	CHECK(looked == 1);

	// A deep channel too narrow to be deep all round (deep only within 150 of x = 0): none. As wide as the ring: there.
	const auto channel = [](float a_x, float) { return std::abs(a_x) <= 150.0f ? std::optional<float>{ 400.0f } : std::optional<float>{ 20.0f }; };
	CHECK(!WaterRules::FindSpot(0.0f, 0.0f, 1024.0f, channel));
	const auto wide = [](float a_x, float) { return std::abs(a_x) <= WaterRules::SPOT_AROUND ? std::optional<float>{ 400.0f } : std::nullopt; };
	const auto inWide = WaterRules::FindSpot(0.0f, 0.0f, 1024.0f, wide);
	REQUIRE(inWide.has_value());
	CHECK(inWide->x == 0.0f);

	// A depth read as no number is not water.
	const auto broken = [](float, float) { return std::optional<float>{ std::numeric_limits<float>::infinity() }; };
	CHECK(!WaterRules::FindSpot(0.0f, 0.0f, 256.0f, broken));
}

TEST("water: a body's rise from its pelvis to its head is about 90 standing, near 0 level, below 0 head down")
{
	using WaterRules::BodyRise;
	const auto about = [](std::optional<float> a_got, float a_want) { return a_got && std::fabs(*a_got - a_want) < 0.01f; };
	CHECK(about(BodyRise(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 60.0f), 90.0f));     // standing
	CHECK(about(BodyRise(0.0f, 0.0f, 0.0f, 60.0f, 0.0f, 0.0f), 0.0f));      // lying level, facing any way
	CHECK(about(BodyRise(0.0f, 0.0f, 0.0f, 0.0f, -60.0f, 0.0f), 0.0f));
	CHECK(about(BodyRise(10.0f, 10.0f, 10.0f, 40.0f, 50.0f, -40.0f), -45.0f));  // head 50 below the hips, 50 ahead: diving at 45
	CHECK(about(BodyRise(0.0f, 0.0f, 0.0f, 30.0f, 0.0f, 30.0f), 45.0f));
	CHECK(!BodyRise(5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f));  // one point: no line
	CHECK(!BodyRise(0.0f, 0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f));
}

TEST("water: the events heard around a moment are said each once, in the order first heard, and only the last 64 kept")
{
	using Clock = WaterRules::Clock;
	const auto        t = Clock::now();
	WaterRules::Recent recent;
	recent.Push("moveStart", t - 3s);
	recent.Push("SwimStart", t - 100ms);
	recent.Push("moveStart", t);
	recent.Push("SwimStart", t + 200ms);
	recent.Push("footLeft", t + 1400ms);
	recent.Push("late", t + 2s);
	CHECK((recent.Near(t, WaterRules::AROUND) == std::vector<std::string>{ "SwimStart", "moveStart", "footLeft" }));
	CHECK(recent.Near(t + 10s, WaterRules::AROUND).empty());

	for (int i = 0; i < 100; ++i) {
		recent.Push("event" + std::to_string(i), t + 10s + std::chrono::milliseconds(i));
	}
	CHECK(recent.Size() == WaterRules::Recent::MAX);
	const auto kept = recent.Near(t + 10s, 1s);
	REQUIRE(!kept.empty());
	CHECK(kept.front() == "event36");  // the oldest kept: 100 - 64
	CHECK(kept.back() == "event99");
}

TEST("water: our player's going into the water, under it, up and out are each counted with when; the first read is none")
{
	using Clock = WaterRules::Clock;
	const auto        t = Clock::now();
	WaterRules::Watch watch;
	CHECK(!watch.Feed(true, false, t));  // already swimming when first read: not a change
	CHECK(watch.entered == 0);
	CHECK(!watch.enteredAt);

	CHECK(watch.Feed(true, true, t + 1s));
	CHECK(watch.dived == 1);
	CHECK(watch.divedAt == t + 1s);
	CHECK(!watch.Feed(true, true, t + 2s));
	CHECK(watch.Feed(true, false, t + 3s));
	CHECK(watch.surfaced == 1);
	CHECK(watch.surfacedAt == t + 3s);
	CHECK(watch.Feed(false, false, t + 4s));
	CHECK(watch.left == 1);
	CHECK(watch.leftAt == t + 4s);
	CHECK(watch.Feed(true, false, t + 5s));
	CHECK(watch.entered == 1);
	CHECK(watch.enteredAt == t + 5s);

	// Out of the water and under it in one read (a load into a dungeon): both counted.
	CHECK(watch.Feed(false, true, t + 6s));
	CHECK(watch.left == 2);
	CHECK(watch.dived == 2);

	// After a load the first read is where it is again.
	watch.Forget();
	CHECK(!watch.Feed(true, false, t + 7s));
	CHECK(watch.entered == 1);
	CHECK(watch.surfaced == 1);
}

TEST("water: a state's swimming and under-water flags reach the other game as they went (protocol 11)")
{
	static_assert(Protocol::VERSION >= 11);
	Protocol::PlayerState state;
	state.flags = Protocol::kSwimming | Protocol::kUnderwater | Protocol::kWeaponDrawn;
	state.pitch = -50;
	const auto back = Protocol::DecodePlayerState(Protocol::Encode(state));
	REQUIRE(back);
	CHECK(back->flags == (Protocol::kSwimming | Protocol::kUnderwater | Protocol::kWeaponDrawn));
	CHECK(back->pitch == -50);
}
