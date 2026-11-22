#pragma once

#include <string>
#include <string_view>

// The developer channel's virtual gamepad, for scripted rig tests (PadPlan.h holds what it holds). The game's XInput
// imports (XInputGetState, XInputGetCapabilities, XInputSetState) are hooked, and while the pad is plugged in, pad 0
// is ours: the game's own gamepad code reads it as a real one and moves the player with it, as a player's pad would.
// Each game process has its own pad, so the host's and the VM's games are driven apart, and nothing goes through
// Windows' input: the keyboard and mouse of whoever sits at the PC are left alone.
//
// Carried from F4Multiplayer's src/game/VirtualPad.cpp (commit a7bddd24b), cut down: its muting of the game's keyboard
// and mouse while the window is not in front (InputGate) is left for when the rig shows Skyrim needs it.
//
// Developer channel only (bDevChannel): without it nothing is hooked and no pad is ever plugged in.
namespace VirtualPad
{
	// At plugin load: hooks the game's XInput imports (developer channel only).
	void Install();

	// pad ... (PadPlan::USAGE), on the main thread.
	std::string Command(std::string_view a_args);

	// walkto ... (PadPlan::WALK_USAGE), on the main thread: steers the player with the sticks toward a reference or a
	// point, re-aimed each tick, until within the radius; progress goes to the log once a second.
	std::string WalkTo(std::string_view a_args);

	// Each tick on the main thread: steps a walkto.
	void Frame();
}
