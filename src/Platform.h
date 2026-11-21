#pragma once

// What the game runs on (Windows, or Wine/Proton on Linux) and how it was started, for the log (LaunchReport.h says
// what is logged), and the game's own folder for the plugin's files.
namespace Platform
{
	// At plugin load, after the log is open: the platform and launch lines.
	void LogStartup();

	// The game runs under Wine (Proton): ntdll has wine_get_version.
	[[nodiscard]] bool UnderWine();

	// The folder SkyrimSE.exe is in (empty if Windows can't tell).
	[[nodiscard]] const std::filesystem::path& GameFolder();

	// A file of the game's, named from the game's folder ("Data/SKSE/Plugins/SkyrimCoop.ini"): as the plugin always
	// opened it, from the working folder, when it is there; otherwise from the game's folder, where SKSE found the
	// plugin. A launcher that starts the game from another folder (some Linux mod managers can) leaves the working
	// folder elsewhere, and the ini and the texts then went unread.
	[[nodiscard]] std::filesystem::path GamePath(std::string_view a_relative);
}
