#pragma once

// The plugin's log (Documents/My Games/Skyrim Special Edition/SKSE/SkyrimCoop.log). CommonLib's SKSE::Init opens
// it; this keeps the last launches' logs and makes sure no line holds a whole SteamID64 or IP address
// (see Redact.h: players attach this file to public bug reports).
namespace Log
{
	// The log's file name without ".log": main.cpp gives SKSE::Init the same name.
	inline constexpr const char* NAME = "SkyrimCoop";

	// Before SKSE::Init: the previous logs move up to SkyrimCoop.1.log to .3.log. Returns why the
	// previous log could not be kept (logged once the log is open), or no error.
	std::error_code RotateBeforeInit();

	// Right after SKSE::Init, before any thread of ours starts: every line passes through Redact::Scrub
	// on its way to the file. A net under the call sites, which log players and accounts by alias already.
	void InstallScrubber();

	// "info" (the default: what players attach) or "debug". sLogLevel in the ini. False for anything else.
	bool                           SetLevel(std::string_view a_level);
	[[nodiscard]] std::string_view Level();

	void Flush();

	// Where the log is (empty before RotateBeforeInit, or without a Documents folder).
	[[nodiscard]] const std::filesystem::path& Path();
}
