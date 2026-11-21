#pragma once

#include "Protocol.h"

// The other players as this game shows them: one stand-in character each, placed and moved by the states their games
// send.
//
// NOT WRITTEN YET (docs/PORTING.md, "The other players"): this keeps who is in the session and where they were last
// said to be, and puts nothing in the world. Main thread only.
namespace RemotePlayers
{
	void Joined(std::uint32_t a_playerId, std::string_view a_name);
	void Left(std::uint32_t a_playerId);

	// The newest state of a player who has joined; one of a player who has not is dropped.
	void Moved(std::uint32_t a_playerId, const Protocol::PlayerState& a_state);

	// The session ended, or a save is about to be loaded: every stand-in goes.
	void Clear();

	[[nodiscard]] std::size_t Count();
}
