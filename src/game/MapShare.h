#pragma once

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Map markers shared (docs/DESIGN.md, "Map markers shared"): a place one player discovers while in the session shows on
// every player's map, ready for fast travel, as the game itself puts a quest's place on the map (ObjectReference.AddToMap).
// What each map had before the session stays its own. bShareMap = false in SkyrimCoop.ini: neither ours to them nor
// theirs to us. The rules with no game in them are src/game/MapRules.h. Main thread.
namespace MapShare
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// At each welcome: what our map has now is the start (not news to anyone).
	void Started();
	// Each frame while in the session: reads our markers for new finds and sends them, and puts heard ones on our map.
	void Frame(const SendFn& a_send);
	// The session's markers (kMapMarkers): a friend's finds, or the session's whole book when we join.
	void Heard(const Protocol::MapMarkers& a_msg);
	// After a save is loaded or a new game started: what the session found goes on that map again, and its own map is the
	// start again.
	void AfterLoad();
	// Leaving the session.
	void Clear();
	// The load order's map markers of places (read once, when first asked), and a marker's place name ("" for none):
	// for fast travel together, which goes to the one nearest where a carriage lands.
	[[nodiscard]] const std::vector<std::uint32_t>& Places();
	[[nodiscard]] std::string PlaceName(std::uint32_t a_ref);
	// Dev channel `mapshare`.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
