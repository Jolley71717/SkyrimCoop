#pragma once

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Doors, locks and mechanisms in a session (docs/DESIGN.md, "Doors, locks and mechanisms"; the rules, free of the game,
// are src/game/RefRules.h). The game's own events say what changed: a door or gate opened or closed
// (TESOpenCloseEvent), a lock changed (TESLockChangedEvent: picked, unlocked with a key, locked by a script), our
// player used something (TESActivateEvent). Each is read on the next frame and reported if the session doesn't know it
// yet; the session's word on each reference is applied here where it reads otherwise, at once when the reference is
// loaded and once it loads otherwise; a friend's use of a mechanism is made here by their stand-in.
//
// From the tick only (GameThread), except the event sinks, which only queue. The tick is not yet the game's main thread
// (GameThread.h): the game writes made here are counted when they are made off it (`refs`: offmain=).
namespace WorldObjects
{
	using SendFn = std::function<void(std::vector<std::uint8_t>)>;

	// At kDataLoaded: the event sinks. False when the game's event source is not there (said in the log).
	bool Install();

	// The session let us in as a_playerId: what this game knew of the last session's doors is forgotten.
	void Welcomed(std::uint32_t a_playerId);

	// Once a tick while in a session and let in: reports what changed here, applies the session's words that waited,
	// and asks for every kept word again after a load. a_send sends one packet on the reliable channel.
	void Frame(const SendFn& a_send);

	// From the session: its word on a reference (kRemoteRefState), and a friend's use of a mechanism (kRemoteActivate).
	void Heard(const Protocol::RefState& a_state);
	void Used(const Protocol::Activation& a_use);
	// ... and what a friend's use did in their game (kRemoteUseEffects): made here where our stand-in's use did not
	// (docs/DESIGN.md, "What a use did"; the rules are src/game/UseRules.h).
	void HeardEffects(const Protocol::UseEffects& a_effects);

	// SKSE's kSaveGame, before the game writes the save (and before the stand-ins are taken out): the items lent to
	// stand-ins for friends' uses are taken back.
	void BeforeSave();

	// From the session: a report of ours it did not take (kRefRefused). It is no longer out; a busy one goes again.
	void Refused(const Protocol::RefRefusal& a_refusal);

	// A save was loaded or a new game started: its doors are as that world has them, so every word of the session is
	// asked for again and applied in full.
	void AfterLoad();

	// The session ended.
	void Clear();

	// Developer channel: `refs` (counts), `refs near [radius]` (this class's loaded references near our player), `refs
	// refuse <on|off>` (friends' uses refused here, the rig's stand-in for a script that does nothing for a stand-in), `ref
	// <hex id>` (one reference), `use <hex id>` (our player uses it, as the activate key does).
	[[nodiscard]] std::string Command(std::string_view a_args);
	[[nodiscard]] std::string One(std::string_view a_args);
	[[nodiscard]] std::string Use(std::string_view a_args);
	// `script <hex id>`: the Papyrus scripts bound to it, each one's state and whole-number and true/false variables.
	[[nodiscard]] std::string Scripts(std::string_view a_args);
	// `anims <hex id>`: the animation events read on it lately (a friend's use's made here marked *), the last one.
	[[nodiscard]] std::string Anims(std::string_view a_args);
	// `enables <hex id> [radius]`: the references near it a use's effect can enable or disable, and which are disabled.
	[[nodiscard]] std::string Enables(std::string_view a_args);
}
