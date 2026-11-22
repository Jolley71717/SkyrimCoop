#pragma once

#include <cstdint>
#include <format>
#include <string>

// The name of the Address Library file a game version reads, as CommonLibSSE-NG looks for it (REL/IDDB.h): in
// Data/SKSE/Plugins, "versionlib-1-7-104-0.bin" for the Anniversary Edition line (1.6 and later), "version-1-5-97-0.bin"
// before it. No game code here, so it is unit-tested; main.cpp checks the file is there before anything asks the
// library for an address, because the library's own answer to a missing file is a message box and the end of the game.
namespace AddressLibraryFile
{
	[[nodiscard]] inline std::string Name(std::uint16_t a_major, std::uint16_t a_minor, std::uint16_t a_patch, std::uint16_t a_build)
	{
		const bool anniversary = a_major > 1 || (a_major == 1 && a_minor >= 6);
		return std::format("{}-{}-{}-{}-{}.bin", anniversary ? "versionlib" : "version", a_major, a_minor, a_patch, a_build);
	}

	// Where that file is, from the game's folder.
	[[nodiscard]] inline std::string Path(std::uint16_t a_major, std::uint16_t a_minor, std::uint16_t a_patch, std::uint16_t a_build)
	{
		return "Data/SKSE/Plugins/" + Name(a_major, a_minor, a_patch, a_build);
	}
}
