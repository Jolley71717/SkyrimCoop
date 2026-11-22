#pragma once

#include "steam/SteamApi.h"

// The first test's third point: messages over Steam's relay, both ways, timed (net/RelayProbe.h has the messages
// and the sums). Pings go to Steam::OtherPlayer() on a channel of their own, so nothing else that uses Steam's
// messages (the session's transport, later) mixes with them. Main thread only, from the tick.
//
// The friend sends first: a host's Steam accepts messages from players in its lobby (Steam.cpp, SessionRequested),
// while a friend's accepts only from those it has sent to. Once the friend has pinged, the host can ping back.
namespace SteamProbe
{
	inline constexpr int CHANNEL = 5;

	// Every tick: answers pings, takes in pongs, logs each first.
	void Frame();

	// Sends a_count pings to the other player, one each tick. The reply for the developer channel.
	std::string Send(std::uint32_t a_count);

	// One line: what was sent and answered, the round trips; in the log too with a_log (once, at the end of a test: the
	// rig asks for the report every second while it waits).
	std::string Report(bool a_log);

	void Reset();
}
