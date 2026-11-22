#pragma once

// The developer channel's commands (DevChannel.h). Every handler runs on the game's main thread, from the tick.
//
// The rig's commands: the first test's (status, steam, probe) and the session's (session, where, standins, strays, graph,
// pad, walkto, console). F4Multiplayer's src/DevCommands.cpp has about a hundred, nearly all about Fallout 4's systems;
// the names kept here are its names (`status`, `steam join <id>`, `pad`, `walkto`), so the rig's habits carry.
namespace DevCommands
{
	using Handler = std::string (*)(std::string_view a_args);

	// The handler for a command name, or nullptr.
	[[nodiscard]] Handler Find(std::string_view a_name);

	// One line naming every command.
	[[nodiscard]] std::string Help();

	// What the start-up found, for `status`: the game's version and the Address Library's state ("ok" or why not).
	void NoteStart(std::string a_game, std::string a_addressLibrary);

	// Once a tick: the first test's timed lines (FirstTest.h).
	void Frame();
}
