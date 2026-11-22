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
	CHECK(got == GaitDrive::Values{ 320.0f, 0.25f });
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
	CHECK(table.Get(1, 0xFF000801, T0 + 2s) == GaitDrive::Values{ 0.0f, 0.5f });
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
