#pragma once

#include <string>
#include <string_view>

// Each friend on our compass, the world map and the local map, and their name over their head (docs/DESIGN.md, "Friends
// on the compass and the maps"). What is shown where is MarkerRules (game-free, tested); the friends' places are their
// states (RemotePlayers) and their statuses (Party: travelling, inside and the way in, health).
//
// Drawn with the menus' own Flash (ActionScript 2: empty clips and the drawing API, a text field for the name), in a
// layer of ours on top of each: no swf of ours, nothing of the menus changed. The HUD's layer is drawn as the HUD menu
// advances (the compass and the names), the map's as the map menu does.
//
// Main thread: Frame takes what is to be drawn; the menus' hooks draw it.
namespace FriendMarkers
{
	// At kDataLoaded: hooks the HUD menu's and the map menu's advance. Each says in the log when it can't.
	void Install();

	// Once a tick in a session: each friend's place, look and name as the menus will draw them.
	void Frame();
	// The session ended (or none): nothing is drawn.
	void Clear();

	// Developer channel `markers`: what each menu drew last (the compass found and its width, the map's camera and the
	// spot of each friend in its units, the names shown), and `markers span <degrees>` sets how much of a turn the
	// compass shows (the rig measures it).
	[[nodiscard]] std::string Command(std::string_view a_args);
}
