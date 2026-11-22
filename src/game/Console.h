#pragma once

#include <string_view>

// The game's own console commands, run from the plugin as if typed into the console (`coc Riverwood`, `save name`).
// For the developer channel's `console` command: the rig places both players with it and saves with it. Main thread.
namespace Console
{
	// False when the game could not make the script to run it in.
	bool Run(std::string_view a_command);
}
