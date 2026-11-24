#pragma once

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Fast travel together (docs/DESIGN.md, "Fast travel together"; the rule is in src/game/TravelRules.h): our player's
// fast travels and carriage rides are said to the session, and a friend's offers us to follow it for a minute when we
// were near them as they left; the follow key (iFollowKey in SkyrimCoop.ini, F7) takes it, by the game's own fast travel.
// Main thread.
namespace TravelTogether
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// Listens for the follow key (the game's input events). Once, when the game's data is loaded.
	void Install();
	// Each frame while in the session: says a trip of ours as it begins, and follows when the key was pressed.
	void Frame(const SendFn& a_send);
	// A friend's trip (kTrip): an offer to follow when we were near where they left.
	void Heard(const Protocol::Trip& a_trip);
	// Leaving the session.
	void Clear();
	// Dev channel `travel [follow]`: trips said and heard, the offer standing, follows and refusals; `follow` presses the key.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
