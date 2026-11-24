#include "Test.h"

#include "ConsoleTarget.h"

// The developer channel's console command: "<ref id>.<command>" must reach the game with that reference as its target
// (src/ConsoleTarget.h).

TEST("console target: a hex id before the dot is split off")
{
	const auto s = ConsoleTarget::SplitTarget("0001A2B3.kill");
	CHECK(s.id == 0x0001A2B3u);
	CHECK(s.command == "kill");

	const auto lower = ConsoleTarget::SplitTarget("ff00abc1.lock 50");
	CHECK(lower.id == 0xFF00ABC1u);
	CHECK(lower.command == "lock 50");

	const auto shortId = ConsoleTarget::SplitTarget("  14.startcombat 1A2B3 ");
	CHECK(shortId.id == 0x14u);
	CHECK(shortId.command == "startcombat 1A2B3 ");

	const auto spaced = ConsoleTarget::SplitTarget("0001A2B3. kill");
	CHECK(spaced.id == 0x0001A2B3u);
	CHECK(spaced.command == "kill");
}

TEST("console target: commands with no id are left whole")
{
	for (const std::string_view c : { "coc Riverwood", "player.kill", "dead.kill", "save rig", "123456789.kill",
	                                  "0001A2B3.", "00000000.kill", ".kill", "0001A2G3.kill", "set timescale to 20.0" }) {
		const auto s = ConsoleTarget::SplitTarget(c);
		CHECK(s.id == 0u);
		CHECK(s.command == c);
	}
}
