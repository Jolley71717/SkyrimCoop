#pragma once

#include <string>
#include <string_view>

// The game's own console commands, run from the plugin as if typed into the console (`coc Riverwood`, `save name`,
// `0001A2B3.kill`). For the developer channel's `console` command: the rig places both players, saves, and sets up its
// fights, locks and kills with it. Main thread.
namespace Console
{
	// Empty when the command ran; otherwise why it could not: no script to run it in, or a "<ref id>." target that names
	// no reference.
	std::string Run(std::string_view a_command);
}
