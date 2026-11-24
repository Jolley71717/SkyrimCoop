#pragma once

#include "Protocol.h"

namespace RE
{
	class Actor;
}

namespace SKSE
{
	class SerializationInterface;
}

// The other players as this game shows them: one stand-in character each, placed and moved by the states their games
// send (docs/DESIGN.md, "The other players").
//
// A stand-in is a plain NPC of the friend's race and sex, wearing the armor they wear, named as they are. Its base is
// one of the game's own character-creation presets (an NPC of the load order, never a form made at run time), and the
// game never runs its AI: the actor update of every Character goes through our hook, which for a stand-in places it
// where its friend is (StateBuffer), turns it, runs the engine's no-AI update and drives its animation graph (Gait; the
// graph's Speed and Direction through the engine's own channels for them, GaitDrive).
//
// A stand-in is never saved (docs/DESIGN.md, "Nothing of the session in a save"): every one is taken out of the world
// before the game writes a save (BeforeSave, from SKSE's kSaveGame: disabled then, deleted a second later, never both in
// one frame) and placed again after; the co-save lists the ones taken out, and after a load any of them, or any other
// placed actor of a preset base, is removed (AfterLoad; that
// sweep only runs when this game has a reason to look, so another mod's preset actors are left alone otherwise).
//
// Main thread only, except the hook.
namespace RemotePlayers
{
	// At kDataLoaded: hooks the Character class's actor update. False when it can't (the reason is in the log).
	bool Install();

	void Joined(std::uint32_t a_playerId, std::string_view a_name);
	void Left(std::uint32_t a_playerId);

	// A state of a player who has joined, as it came; one of a player who has not is dropped.
	void Moved(std::uint32_t a_playerId, const Protocol::PlayerState& a_state);
	// What a player's character looks like; a stand-in already there is made again if it changed.
	void Looked(std::uint32_t a_playerId, const Protocol::PlayerLooks& a_looks);

	// Once a tick: places, dresses and takes away stand-ins as the players and this game's world say.
	void Frame();

	// The session ended: every player and stand-in goes.
	void Clear();

	// Once a tick, in a session or not: each stand-in on its way out of the world goes a step further. It is disabled
	// first and deleted only once the game has taken it out (src/game/Retire.h); never both in one frame.
	void TendGraveyard();

	// A save is about to be loaded, or a new game started: the stand-ins go with the world (their players stay, and
	// come back once the new world is there).
	void ForgetStandIns();

	// SKSE's kSaveGame, before the game writes the save: every stand-in is taken out of the world.
	void BeforeSave();
	// The co-save's record of the stand-ins taken out for this save, and its reading at a load.
	void WriteCoSave(SKSE::SerializationInterface* a_intf);
	void ReadCoSave(SKSE::SerializationInterface* a_intf, std::uint32_t a_version, std::uint32_t a_length);
	// After a load (kPostLoadGame): any stand-in that came back with the save is removed. The sweep of every placed
	// preset actor runs only when this game placed a stand-in, the co-save named some, or friends are in the session.
	void AfterLoad();

	[[nodiscard]] std::size_t Count();

	// That player's stand-in, placed in this world now (enabled, its 3D there); null when there is none. Main thread:
	// what a friend's use of a mechanism is made by (src/game/WorldObjects.cpp).
	[[nodiscard]] RE::Actor* StandInOf(std::uint32_t a_playerId);

	// Developer channel: `standins` (one line each), `strays` (placed actors of a preset base in the loaded game:
	// 0 outside a session means nothing of a session was saved), `graph <player> [event <name> | vars]`.
	[[nodiscard]] std::string Describe();
	[[nodiscard]] std::string Strays();
	[[nodiscard]] std::string Graph(std::string_view a_args);

	// The co-save record's type: "SKST" (stand-ins).
	inline constexpr std::uint32_t COSAVE_STANDINS = 0x54534B53;
	inline constexpr std::uint32_t COSAVE_STANDINS_VERSION = 1;
}
