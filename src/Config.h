#pragma once

#include "Settings.h"

// The plugin's settings as read from Data/SKSE/Plugins/SkyrimCoop.ini when the plugin loads (Settings.h has the
// keys and their defaults).
namespace Config
{
	// Reads the file (the defaults when there is none) and says in the log what was read. Sets the log's level.
	void Load();

	[[nodiscard]] const Settings::Values& Get();
}
