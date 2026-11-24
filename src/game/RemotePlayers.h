#pragma once

#include "Protocol.h"
#include "game/GaitDrive.h"

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
	// What a player's character looks like: a stand-in already there is made again for another body (race, sex,
	// weight), or dressed again for other gear (what they wear, hold in each hand, shoot and have in their voice).
	void Looked(std::uint32_t a_playerId, const Protocol::PlayerLooks& a_looks);
	// What a player did (their graph's events: attacks, blocks, casts, shouts, draws), each played on their stand-in at
	// the moment it is drawn at. Those of a player with no stand-in here are counted and dropped.
	void Acted(std::uint32_t a_playerId, const std::vector<Protocol::PlayerAction>& a_actions);

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

	// For the copies of other games' NPCs (NpcSync), which go through the same hooks:
	// Whether an actor is one of this game's stand-ins, or an actor of ours on its way out of the world.
	[[nodiscard]] bool IsOurs(const RE::Actor* a_actor);
	// The stand-in that shows a_playerId here (none: not placed), and the player a stand-in shows (0: it is none). Main
	// thread.
	[[nodiscard]] RE::NiPointer<RE::Actor> StandInOf(std::uint32_t a_playerId);
	[[nodiscard]] std::uint32_t            PlayerOf(const RE::Actor* a_actor);
	// Each friend's newest state, as it came (their place, for who is nearest an NPC).
	[[nodiscard]] std::vector<Protocol::PlayerState> FriendStates();
	// How many copies NpcSync moves: while any, the actor update hook asks NpcSync::UpdateCopy about every Character
	// that is not a stand-in, and the leg channels look actors up.
	void SetCopies(std::size_t a_count);
	// What a copy's graph is given for Speed and Direction from now on (GaitDrive.h), and no more.
	void DriveLegs(RE::Actor* a_actor, GaitDrive::Values a_values);
	void StopLegs(RE::Actor* a_actor);
	// What the game's own actor update does at its end and the no-AI update (a stand-in's, a copy's) does not: an actor
	// that still wants to draw or put away is told again: its friend's wish (a_friendDrawn, a stand-in) or the wish it
	// has (a copy; StandInRules::FinishWish). True when it was.
	bool FinishDraw(RE::Actor* a_actor, std::optional<bool> a_friendDrawn = std::nullopt);
	// An actor this game placed (a copy of a spawn) goes out of the world as a stand-in does: disabled, then deleted
	// (Retire.h), its AI off meanwhile, taken out before a save and named in the co-save until it is gone.
	// The rig's log of what moves a stand-in's weapon state: every graph event about weapons it is sent (by this plugin or the
	// game) and every change of its state, capped.
	void TraceEvent(RE::TESObjectREFR* a_ref, const char* a_event, bool a_taken);
	void TraceState(RE::Actor* a_actor);
	void TakeOut(RE::Actor* a_actor, std::chrono::steady_clock::time_point a_placedAt);

	// Developer channel: `standins` (one line each), `strays` (placed actors of a preset base in the loaded game:
	// 0 outside a session means nothing of a session was saved), `graph <player> [event <name> | vars | weapon | equip | active]`.
	[[nodiscard]] std::string Describe();
	// Developer channel `actions` (the stand-ins' part): per player, the actions that came and were played or turned
	// away by kind, the weapon drawn against its friend's, what each hand holds against the looks, and what it did here
	// (its spells' effects refused, blows, casts, health taken and given back).
	[[nodiscard]] std::string Actions();
	[[nodiscard]] std::string Strays();
	[[nodiscard]] std::string Graph(std::string_view a_args);

	// The co-save record's type: "SKST" (stand-ins).
	inline constexpr std::uint32_t COSAVE_STANDINS = 0x54534B53;
	inline constexpr std::uint32_t COSAVE_STANDINS_VERSION = 2;  // 2: each actor's base too (copies of spawns are in it)
}
