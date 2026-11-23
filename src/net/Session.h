#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <string_view>

// The session as this game takes part in it (docs/DESIGN.md, "The session").
//
// Hosting: the session server (common/Server) runs on its own thread in this game, reached by friends through Steam
// (SteamServerTransport) and by this game's own connection over UDP on this machine only. Joining: a connection to the
// host's Steam id (SteamClient). A friend joins from Steam itself: an invite or "Join Game" (Steam's join callbacks),
// a steam://joinlobby address, or the lobby on the command line Steam starts the game with (LaunchArgs); Steam puts
// them in the host's lobby and this connects to its owner.
//
// Once let in, the game sends where its player is 20 times a second and what they look like when it changes, and
// hands what the others send to RemotePlayers. Every packet goes through FormTranslate on its way out and in, so the
// forms it names are the session's on the wire (by plugin file name) and this game's everywhere else. Main thread only.
namespace Session
{
	// Once a tick.
	void Frame();

	// Starts hosting; false (and a_why) when it can't.
	bool Host(std::string& a_why);

	// Hosting, or joining or joined to someone's.
	[[nodiscard]] bool Active();

	// Ends this game's part: the connection, the server when hosting, the lobby, every stand-in.
	void Leave(std::string_view a_why);

	// For the developer channel's `session`: one line, key=value, never a name or a Steam id.
	[[nodiscard]] std::string Describe();

	// A face's fingerprint (FaceRules::Hash) with its forms in the session's numbering, as it goes out: the same face is
	// the same number in every game of the session whatever its load order (the developer channel's `faces`, the rig's).
	[[nodiscard]] std::uint32_t SessionFaceHash(const Protocol::PlayerFace& a_face);

	// For the developer channel's `forms` (net/FormsCommand.h): this game's edge to the session's form IDs, and one form
	// looked up by ID, or by its plugin's file name and its number within the plugin.
	[[nodiscard]] std::string Forms(std::string_view a_args);
}
