#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

// The first test in a game (docs/GETTING-STARTED.md): what the plugin's log must show for the three things the
// project has to prove over Steam, each with how long it took. Steam's state is looked at once a tick and every change
// that matters becomes one log line starting "First test:", which tools/rig/skyrim/lobbytest.sh reads. No game and no
// Steam here, so it is unit-tested; src/DevCommands.cpp feeds it.
//
//   1. a lobby opens:       "First test: lobby open (lobby <id>) <n> ms after it was asked for"
//   2. the friend joins:    "First test: joined lobby <id> <n> ms after it was asked for" (the friend's log)
//                           "First test: a player joined our lobby (<n> members) <n> ms after it opened" (the host's)
//   3. messages both ways:  src/steam/SteamProbe.cpp's "First test: relay ..." lines
class FirstTest
{
public:
	using Clock = std::chrono::steady_clock;

	struct Seen
	{
		bool          hosting = false;
		std::uint64_t lobby = 0;    // the lobby this game is in (its own when hosting)
		int           members = 0;  // in that lobby, this game included
	};

	void AskedToHost(Clock::time_point a_now) { hostAskedAt = a_now; }
	void AskedToJoin(Clock::time_point a_now) { joinAskedAt = a_now; }

	// The lines Steam's state since the last call makes.
	std::vector<std::string> Look(const Seen& a_now, Clock::time_point a_time)
	{
		std::vector<std::string> lines;
		const auto               ms = [&](std::optional<Clock::time_point> a_since) {
			return a_since ? std::format("{} ms after it was asked for", std::chrono::duration_cast<std::chrono::milliseconds>(a_time - *a_since).count()) :
			                 std::string{ "(not asked for through the developer channel: a Steam invite or Join Game)" };
		};

		if (a_now.lobby != last.lobby) {
			if (last.lobby != 0) {
				lines.push_back(std::format("First test: left lobby {}", last.lobby));
				lobbyAt.reset();
			}
			if (a_now.lobby != 0) {
				lobbyAt = a_time;
				if (a_now.hosting) {
					lines.push_back(std::format("First test: lobby open (lobby {}) {}", a_now.lobby, ms(hostAskedAt)));
					hostAskedAt.reset();
				} else {
					lines.push_back(std::format("First test: joined lobby {} ({} members) {}", a_now.lobby, a_now.members, ms(joinAskedAt)));
					joinAskedAt.reset();
				}
			}
			last = a_now;
			return lines;
		}

		if (a_now.lobby != 0 && a_now.members != last.members) {
			if (a_now.members > last.members) {
				const auto opened = lobbyAt ? std::chrono::duration_cast<std::chrono::milliseconds>(a_time - *lobbyAt).count() : 0;
				lines.push_back(std::format("First test: a player joined our lobby ({} members) {} ms after it opened", a_now.members, opened));
			} else {
				lines.push_back(std::format("First test: a player left our lobby ({} members)", a_now.members));
			}
		}
		last = a_now;
		return lines;
	}

private:
	Seen                             last;
	std::optional<Clock::time_point> hostAskedAt;
	std::optional<Clock::time_point> joinAskedAt;
	std::optional<Clock::time_point> lobbyAt;
};
