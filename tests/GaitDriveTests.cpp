#include "Test.h"

#include "game/GaitDrive.h"

// The speed and direction each stand-in's graph is given in place of the engine's (src/game/GaitDrive.h).

namespace
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	const Clock::time_point T0{ 100s };
}

TEST("gait drive: a stand-in's values are read back for it, and only for it")
{
	GaitDrive::Table<int> table;
	table.Set(1, 0xFF000801, { 320.0f, 0.25f }, T0);
	const auto got = table.Get(1, 0xFF000801, T0 + 10ms);
	CHECK(got.has_value());
	const GaitDrive::Values want{ 320.0f, 0.25f };
	CHECK(got == want);
	// Another actor: nothing set for it, so the engine's own values stand.
	CHECK(!table.Get(2, 0xFF000802, T0).has_value());
}

TEST("gait drive: an address the game gave to another actor does not read the old stand-in's values")
{
	GaitDrive::Table<int> table;
	table.Set(1, 0xFF000801, { 320.0f, 0.0f }, T0);
	CHECK(!table.Get(1, 0xFF000900, T0).has_value());
}

TEST("gait drive: values its update stopped writing go back to the engine's after FRESH")
{
	GaitDrive::Table<int> table;
	table.Set(1, 0xFF000801, { 150.0f, 0.5f }, T0);
	CHECK(table.Get(1, 0xFF000801, T0 + GaitDrive::FRESH).has_value());
	CHECK(!table.Get(1, 0xFF000801, T0 + GaitDrive::FRESH + 1ms).has_value());
	// Written again: read again.
	table.Set(1, 0xFF000801, { 0.0f, 0.5f }, T0 + 2s);
	const GaitDrive::Values stopped{ 0.0f, 0.5f };
	CHECK(table.Get(1, 0xFF000801, T0 + 2s) == stopped);
}

TEST("gait drive: a stand-in taken away, or all of them, leave nothing behind")
{
	GaitDrive::Table<int> table;
	table.Set(1, 0xFF000801, { 150.0f, 0.0f }, T0);
	table.Set(2, 0xFF000802, { 300.0f, 0.0f }, T0);
	table.Erase(1);
	CHECK(!table.Get(1, 0xFF000801, T0).has_value());
	CHECK(table.Get(2, 0xFF000802, T0).has_value());
	CHECK(table.Size() == 1);
	table.Clear();
	CHECK(table.Size() == 0);
}

TEST("gait drive: a flyer's flight values are held for its address, read without the actor, and counted while any is set")
{
	GaitDrive::Table<int> table;
	CHECK(!table.AnyFlight());
	table.Set(1, 0xFF000801, { 150.0f, 0.0f }, T0);
	CHECK(!table.AnyFlight());  // a walker: the flight channels skip the table
	const GaitDrive::Values flying{ .speed = 900.0f, .flight = true, .turnDelta = -0.5f, .pitchDelta = 0.25f, .targetSpeed = 1800.0f };
	table.Set(2, 0x000F77F8, flying, T0);
	CHECK(table.AnyFlight());
	const auto held = table.Held(2, T0 + 10ms);
	CHECK(held.has_value());
	CHECK(held->first == 0x000F77F8u);
	CHECK(held->second == flying);
	CHECK(!table.Held(3, T0).has_value());                          // an address nothing was set for
	CHECK(!table.Held(2, T0 + GaitDrive::FRESH + 1ms).has_value());  // stale
	// Landed: set again without flight.
	table.Set(2, 0x000F77F8, { 300.0f, 0.0f }, T0 + 1s);
	CHECK(!table.AnyFlight());
	table.Set(2, 0x000F77F8, flying, T0 + 2s);
	table.Set(3, 0x000F77F9, flying, T0 + 2s);
	table.Erase(2);
	CHECK(table.AnyFlight());
	table.Erase(3);
	CHECK(!table.AnyFlight());
	table.Set(3, 0x000F77F9, flying, T0 + 3s);
	table.Clear();
	CHECK(!table.AnyFlight());
}
