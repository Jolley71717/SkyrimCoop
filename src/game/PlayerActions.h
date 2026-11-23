#pragma once

#include "Protocol.h"

#include <string>
#include <string_view>
#include <vector>

// What our player does, as the others' stand-in of us shows it (docs/DESIGN.md, "Players"): every event our player's
// animation graph takes (an attack, a power attack, a block, a bash, a bow drawn and let go, a spell begun in either
// hand, a shout, a weapon drawn or put away), heard as the game sends it to the graph (the player's
// IAnimationGraphManagerHolder::NotifyAnimationGraph), and sent to the session once a tick (kPlayerActions), each with
// the moment it was heard. The walk's own events are not (each game works those out from the states). A budget keeps a
// flood out (StandInRules::Outbox).
namespace PlayerActions
{
	// At kDataLoaded: hooks the player's graph notify. False when it could not (the others then see our walk, our gear
	// and our weapon drawn, but no attack, cast or shout).
	bool Install();

	// In a session after the welcome (true), or out of it: events are only kept while there is a session to send them to.
	void Recording(bool a_on);

	// The events heard since the last call, at most Protocol::MAX_PLAYER_ACTIONS (the rest wait for the next).
	[[nodiscard]] std::vector<Protocol::PlayerAction> Take();

	// Developer channel `actions` (ours): hooked=, heard= by kind, sent=, dropped=, the last heard.
	[[nodiscard]] std::string Describe();

	// Developer channel `actions equip <right|left|ammo|voice> <hex form id>`: the rig's set-up of our player, as the
	// console sets a scene up: what it carries or knows put in a hand, the quiver or the voice. Main thread.
	[[nodiscard]] std::string Equip(std::string_view a_args);
}
