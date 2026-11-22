#include "Test.h"

#include "game/Retire.h"

// How a stand-in leaves the world: disabled first, deleted only once that is done (src/game/Retire.h).

using namespace std::chrono_literals;

namespace
{
	Retire::Facts Standing(bool a_has3D, Retire::Duration a_sincePlaced)
	{
		return { .has3D = a_has3D, .sincePlaced = a_sincePlaced };
	}

	Retire::Facts Disabled(bool a_has3D, Retire::Duration a_since, std::uint64_t a_ticks)
	{
		return { .has3D = a_has3D, .sincePlaced = 1min, .sinceDisabled = a_since, .ticksSinceDisabled = a_ticks };
	}
}

TEST("retire: a stand-in in the world is disabled first, never deleted at once")
{
	CHECK(Retire::Next(Standing(true, 0ms)) == Retire::Step::kDisable);
	CHECK(Retire::Next(Standing(true, 10min)) == Retire::Step::kDisable);
}

TEST("retire: one whose 3D is still loading waits for it, but not for ever")
{
	CHECK(Retire::Next(Standing(false, 0ms)) == Retire::Step::kWait);
	CHECK(Retire::Next(Standing(false, 14s)) == Retire::Step::kWait);
	CHECK(Retire::Next(Standing(false, Retire::LOAD_WAIT)) == Retire::Step::kDisable);
}

TEST("retire: disabled in this tick, it is not deleted in this tick (the crash of the first two-player test)")
{
	// A save, a player leaving, a session ending: each disables and asks again at once. The answer must be no.
	CHECK(Retire::Next(Disabled(true, 0ms, 0)) == Retire::Step::kWait);
	CHECK(Retire::Next(Disabled(false, 0ms, 0)) == Retire::Step::kWait);
}

TEST("retire: deleted only a second and some ticks after it was disabled, its 3D gone")
{
	CHECK(Retire::Next(Disabled(false, Retire::DELETE_AFTER, Retire::DELETE_AFTER_TICKS)) == Retire::Step::kDelete);
	CHECK(Retire::Next(Disabled(false, 5s, 500)) == Retire::Step::kDelete);
	// Not before the second,
	CHECK(Retire::Next(Disabled(false, 999ms, 500)) == Retire::Step::kWait);
	// nor before the game ran the ticks (a long pause with the task pool not run: a menu, a save),
	CHECK(Retire::Next(Disabled(false, 1min, Retire::DELETE_AFTER_TICKS - 1)) == Retire::Step::kWait);
	// nor while its 3D is in: the disable has not been done.
	CHECK(Retire::Next(Disabled(true, 1min, 5000)) == Retire::Step::kWait);
}

TEST("retire: one disabled long since that keeps its 3D is told, and never deleted")
{
	CHECK(!Retire::Stuck(Disabled(true, 14s, 900)));
	CHECK(Retire::Stuck(Disabled(true, Retire::LOAD_WAIT, 900)));
	CHECK(Retire::Next(Disabled(true, 10min, 90000)) == Retire::Step::kWait);
	CHECK(!Retire::Stuck(Disabled(false, 10min, 90000)));
	CHECK(!Retire::Stuck(Standing(true, 10min)));
}
