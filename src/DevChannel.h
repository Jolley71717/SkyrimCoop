#pragma once

// The developer channel: a line-based text protocol on 127.0.0.1 that lets the test rig's scripts ask a running game
// questions and give it orders (DevCommands.cpp has the commands), so a test needs nobody at the keyboard and no
// screenshots.
//
// Carried from F4Multiplayer's src/DevChannel.cpp (commit 1d29178b1) with its safety rules:
//   - off unless bDevChannel = true in SkyrimCoop.ini (never for players)
//   - bound to the loopback interface only: another machine reaches it only through an ssh tunnel
//   - every connection must first send "auth <token>"; the token is made new at each start and written to
//     Documents/My Games/Skyrim Special Edition/SKSE/SkyrimCoop_dev.token, in the user's own folder. It is never logged.
// The line handling is in DevLine.h (unit-tested); this file is the sockets and the hand-over to the main thread.
namespace DevChannel
{
	// Starts the listener on a thread of its own when the ini turns it on.
	void Start();

	// The main thread's share: runs the commands the listener handed over. From the tick.
	void Frame();
}
