#pragma once

#include "Protocol.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Knockdowns, the game's side (docs/DESIGN.md, "Knockdowns"; the rules are src/game/KnockRules.h). A living actor knocked
// down or ragdolled (a push, an explosion, a giant's blow, a fall, a paralysis), shown the same in every game:
// - Said: our player and the NPCs this game runs are watched once a tick (their knock state); a knock is said down (where
//   it began, its launch, push or paralysis) and up (where it rests) (kKnocks).
// - Played: a friend's down knocks the actor that shows it here (a stand-in, a copy) with the engine's own knock
//   (AIProcess::KnockExplosion; a paralysis by its Paralysis value, AIProcess::KnockParalyze when that alone does not
//   take). While knocked it is held (Held): not placed by the update hooks, its walk not told, its runner's actions
//   dropped, the engine's own update run. Lying while its friend's actor is still down it is kept down by its Paralysis
//   value, taken back at the friend's up, before a save and whenever it is let go.
// - Asked: a knock this game's world gives a copy (or a stand-in) is held the same way; a copy's push or paralysis is
//   asked of the NPC's runner (kKnocks, phase ask), which knocks its NPC the same way (a paralysis by its Paralysis value,
//   held until this game's ask-up says the copy's own paralysis is over, MAX_DOWN at most), and its down then joins the
//   held copy here. An actor let go while still down is not this world's knock until it stands again (no hold, no ask).
// - Kept here: the graph events of an actor that is knocked are not sent as actions (EventsStay): every game plays its
//   own knock.
//
// Main thread, but Held and EventsStay (the update and graph notify hooks, on the game's job threads).
namespace Knocks
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// Welcomed into a session as a_ourId.
	void Started(std::uint32_t a_ourId);
	// Once a tick in a session after the welcome, after NpcSync::Frame: friends' knocks played when due, held actors
	// tended and let go, ours said, this world's knocks of copies held and asked of their runners, asks applied.
	void Frame(const SendFn& a_send);
	void Heard(const Protocol::Knocks& a_msg);
	// A player left: their stand-in is let go; a copy whose runner left with no up is treated as up.
	void Left(std::uint32_t a_playerId);
	// The session ended: every held actor let go (its Paralysis value taken back), nothing known.
	void Clear();
	// A load or a new game takes the world: every held actor let go.
	void ForgetWorld();
	// SKSE's kSaveGame: every Paralysis value this game added is taken back (none is in the save); the actors stay held
	// and are kept down again after it.
	void BeforeSave();
	// An actor of ours on its way out of the world (a stand-in, a copy of a spawn): let go.
	void Release(RE::Actor* a_actor);

	// Whether a_actor is held by a knock here: not placed by the update hooks, its walk not told, the engine's own update
	// run for it (any thread).
	[[nodiscard]] bool Held(const RE::Actor* a_actor);
	// Whether a_actor's graph events stay in this game now: it is knocked (any thread; counted).
	[[nodiscard]] bool EventsStay(RE::Actor* a_actor);

	// Developer channel `knocks`: ours (said down and up, by kind, the fastest launch) | theirs (heard, played by kind,
	// refused by verdict, joined, took, kept down, let up, how each hold ended, the rest gap) | native (copies and stand-ins
	// knocked by this world, asked) | asks (heard, applied, refused) | scale (the speed a magnitude throws, learnt).
	// `knocks last`: the last said, played and asked. `knocks list`: each actor held. `knocks holds`: the last let go.
	// `knocks state <local id>`: an actor's knock state, phase, Paralysis value, whether held and kept. For the rig's set-up
	// (as its injected attacks are): `knocks push <local id> <magnitude> [<from local id>]` (the engine's knock on that
	// actor, away from the other actor, else from in front of it) and `knocks paralyze <local id> <on|off>` (its Paralysis
	// value, the engine's paralysis knock; off takes back only what on added).
	[[nodiscard]] std::string Command(std::string_view a_args);
}
