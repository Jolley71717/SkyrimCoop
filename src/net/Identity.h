#pragma once

// A random number that stays the same on this PC (kept in
// Documents\My Games\Skyrim Special Edition\SKSE\SkyrimCoop_player.id), so the server recognises a player
// who reconnects and gives them their old player ID back. Not a secret: it only says "the same
// person as before" within one hosting session.
namespace Identity
{
	// Documents\My Games\Skyrim Special Edition\SKSE (the folder SKSE plugins log to), where this mod keeps its
	// per-player files. Nothing when Windows can't tell where Documents is.
	[[nodiscard]] std::optional<std::filesystem::path> DataFolder();

	[[nodiscard]] std::uint64_t Get();
}
