#pragma once

#include "Protocol.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Each player's status (protocol 28, kPlayerStatus/kRemoteStatus; docs/DESIGN.md, "Friends on the compass and the
// maps"): what the others need beside the states to show a player where the states can't, and to put their health on
// the name over their head.
// - Ours: read a few times a second (Frame): our health in percent, whether a load door, a fast travel or a loading
//   screen is taking us away (travelling), the place (BGSLocation) we are in, and inside an interior the way in on the
//   map (where we were last outside; after a load, the door out of where we are). Sent when it changes enough
//   (MarkerRules::Worth), travel at once.
// - Theirs: the newest of each friend, as the server passes them on (and gives a joiner).
//
// Main thread only.
namespace Party
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// Welcomed into a session: ours is sent at the next Frame, whatever was sent before.
	void Started();
	// Once a tick in a session after the welcome.
	void Frame(const SendFn& a_send);
	void Heard(const Protocol::RemoteStatus& a_msg);
	void Left(std::uint32_t a_playerId);
	// The session ended: nothing is known of anyone.
	void Clear();
	// A load or a new game: where we were last outside is of another world.
	void ForgetWorld();

	// A friend's newest status (none: none came yet).
	[[nodiscard]] std::optional<Protocol::PlayerStatus> StatusOf(std::uint32_t a_playerId);
	// Our own status as it would be sent now (none outside a world), for the rig.
	[[nodiscard]] std::optional<Protocol::PlayerStatus> Ours();
	// Whether a load door, a fast travel or a loading screen is taking our player away now.
	[[nodiscard]] bool Travelling();
	// The name a friend is shown by (their stand-in's, and the one over its head): a_name, with their health when hurt
	// (MarkerRules::HealthShown, from their status; the text key party.tag_hurt).
	[[nodiscard]] std::string Tag(std::uint32_t a_playerId, std::string_view a_name);

	// Developer channel `party`: ours (health, travel, place, inside, the way in, statuses sent) and each friend's
	// status as it came.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
