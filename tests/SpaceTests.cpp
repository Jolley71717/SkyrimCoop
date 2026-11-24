#include "Test.h"

#include "game/SpaceRules.h"

#include <chrono>
#include <limits>

// A friend's stand-in through doors, fast travel, loads and exterior cells (docs/DESIGN.md, "The other players"): when
// it is in the world, when it goes into the cell under it, and when one drawn nowhere is placed again
// (src/game/SpaceRules.h).

using namespace std::chrono_literals;

namespace
{
	using SpaceRules::Why;

	SpaceRules::Seen Near()
	{
		return { .hereInWorld = true, .haveState = true, .haveLooks = true, .sameSpace = true, .distance = 500.0f };
	}
}

TEST("doors: a friend near in the same interior or worldspace is shown; each reason not to is said, first one first")
{
	CHECK(SpaceRules::Judge(Near()) == Why::kShown);

	auto seen = Near();
	seen.hereInWorld = false;
	seen.haveState = false;
	CHECK(SpaceRules::Judge(seen) == Why::kNotInWorld);  // our loading screen before theirs
	seen = Near();
	seen.haveState = false;
	CHECK(SpaceRules::Judge(seen) == Why::kNoState);
	seen = Near();
	seen.haveLooks = false;
	CHECK(SpaceRules::Judge(seen) == Why::kNoLooks);
	seen = Near();
	seen.sameSpace = false;
	CHECK(SpaceRules::Judge(seen) == Why::kElsewhere);
	CHECK(SpaceRules::WhyName(Why::kElsewhere) == "elsewhere");
	CHECK(SpaceRules::WhyName(Why::kTravelling) == "travelling");
}

TEST("doors: a stand-in goes as soon as its friend's status says travelling, before their state from the other side")
{
	auto seen = Near();
	seen.placed = true;
	seen.travellingFor = 0ms;
	CHECK(SpaceRules::Judge(seen) == Why::kTravelling);
	seen.travellingFor = SpaceRules::TRAVEL_HIDES_FOR - 1ms;
	CHECK(SpaceRules::Judge(seen) == Why::kTravelling);
	// Not placed while it says so either: it arrives with their first state where they arrive.
	seen.placed = false;
	CHECK(SpaceRules::Judge(seen) == Why::kTravelling);

	// A status left saying travelling hides no one for good: the states decide after it.
	seen.travellingFor = SpaceRules::TRAVEL_HIDES_FOR;
	CHECK(SpaceRules::Judge(seen) == Why::kShown);
	seen.sameSpace = false;
	CHECK(SpaceRules::Judge(seen) == Why::kElsewhere);
}

TEST("doors: a stand-in is placed within the nearer range and kept to the farther, so it does not flicker at the edge")
{
	auto seen = Near();
	seen.distance = SpaceRules::SHOW_WITHIN - 1.0f;
	CHECK(SpaceRules::Judge(seen) == Why::kShown);
	seen.distance = SpaceRules::SHOW_WITHIN + 1.0f;
	CHECK(SpaceRules::Judge(seen) == Why::kFar);
	seen.placed = true;
	CHECK(SpaceRules::Judge(seen) == Why::kShown);
	seen.distance = SpaceRules::HIDE_BEYOND;
	CHECK(SpaceRules::Judge(seen) == Why::kFar);
	seen.distance = std::numeric_limits<float>::quiet_NaN();
	CHECK(SpaceRules::Judge(seen) == Why::kFar);
}

TEST("doors: a stand-in outside goes into the cell under it when that is another the game has loaded, never inside")
{
	CHECK(SpaceRules::Recell(false, 0x0000A001, 0x0000A002, true));
	CHECK(!SpaceRules::Recell(false, 0x0000A001, 0x0000A001, true));  // already there
	CHECK(!SpaceRules::Recell(false, 0x0000A001, 0x0000A002, false));  // not loaded: its 3D would go with it
	CHECK(!SpaceRules::Recell(false, 0x0000A001, 0, true));            // no cell under it
	CHECK(!SpaceRules::Recell(true, 0x0000A001, 0x0000A002, true));    // an interior is one cell
}

TEST("doors: a stand-in drawn nowhere for three seconds is placed again; any frame with its 3D starts the count over")
{
	SpaceRules::Drawn watch;
	const auto        t0 = SpaceRules::Clock::time_point{} + 100s;
	CHECK(!watch.Tick(true, t0));
	CHECK(!watch.Tick(false, t0 + 1s));
	CHECK(!watch.Tick(false, t0 + 1s + SpaceRules::NO_3D_FOR - 1ms));
	CHECK(watch.Tick(false, t0 + 1s + SpaceRules::NO_3D_FOR));
	CHECK(!watch.Tick(true, t0 + 10s));
	CHECK(!watch.Tick(false, t0 + 11s));
	watch.Reset();
	CHECK(!watch.Tick(false, t0 + 13s));
	CHECK(watch.Tick(false, t0 + 13s + SpaceRules::NO_3D_FOR));
}
