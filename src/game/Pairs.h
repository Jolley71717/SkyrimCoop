#pragma once

#include "Protocol.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Kill moves and every other paired animation, the game's side (docs/DESIGN.md, "Kill moves and paired animations";
// the rules are src/game/PairRules.h). A paired animation is one idle the engine plays on two actors together: a kill
// move of any weapon or creature (a sneak kill, a dragon's bite-and-throw, a werewolf's), a vampire's feeding.
// - Said: when the graph of our player or of an NPC this game runs takes a paired idle's event (Noted, from the graph
//   notify hooks of PlayerActions and NpcSync), or its kill move flag is up with no event heard, this game says the pair
//   (kPairs): the two actors as the session names them (the other one is its process's target), the idle by its record
//   and its event, whether it kills, where it began.
// - Kept here: while an actor is in a pair (its kill move flag, a pair's event lately, or held by one played here) its
//   graph's events are not sent to the others as actions (EventsStay): every game plays the whole pair itself.
// - Played: a friend's pair, at the moment its actors are drawn at, on the actors that show both here (a stand-in, a copy;
//   an NPC this game runs only as the victim of a kill), with the engine's own paired idle (AIProcess::PlayIdle, with the
//   other actor as its target), when PairRules::Judge says so. While it plays, both are held: the update hooks neither
//   place them nor tell them their walk, and the engine's own update runs for them (Held), until both are out of it.
// - Held too: a copy (an NPC another game runs) in a pair the engine plays here with one of ours (the victim of our
//   player's sneak kill or our follower's kill move on an NPC the other game runs, or one handed over mid-move), from the
//   pair's say or its flag until the flag falls, by the same rules; its runner's actions are dropped meanwhile.
// - Not said: an actor a pair played here holds (it is in a friend's pair), nor one whose kill move flag is up with an
//   old pair's idle (PairRules::JudgeByFlag).
//
// Main thread, but Noted, EventsStay and Held (the graph notify and update hooks, on the game's job threads).
namespace Pairs
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// At kDataLoaded: the load order's paired idles, by their events (for a pair whose idle record this game lacks), and
	// for `pairs find`. False when there are none (said in the log).
	bool Install();

	// Welcomed into a session as a_ourId.
	void Started(std::uint32_t a_ourId);
	// Once a tick in a session after the welcome, after NpcSync::Frame: ours said, friends' played when due, the actors
	// held by pairs let go when they are over.
	void Frame(const SendFn& a_send);
	void Heard(const Protocol::Pairs& a_msg);
	// A player left: their pairs waiting go, and any pair played here on their stand-in is let go.
	void Left(std::uint32_t a_playerId);
	// The session ended: every actor held by a pair is let go, and nothing is known.
	void Clear();
	// A load or a new game takes the world: no actor of the old one is held.
	void ForgetWorld();
	// SKSE's kSaveGame: every actor held by a pair is let go (placed by its states again).
	void BeforeSave();
	// An actor of ours on its way out of the world (a stand-in, a copy of a spawn): its pair, and the other actor of it,
	// let go.
	void Release(RE::Actor* a_actor);

	// From the graph notify hooks (any thread), for our player and the NPCs this game runs: an event a_actor's graph
	// took; a pair's is kept (the attacker's said by the next tick).
	void Noted(RE::Actor* a_actor, std::string_view a_event);
	// Whether a_actor's graph events stay in this game now: it is in a pair (any thread; counted).
	[[nodiscard]] bool EventsStay(RE::Actor* a_actor);
	// Whether a_actor is held by a pair played here: not placed by the update hooks, its walk not told, the engine's own
	// update run for it (any thread).
	[[nodiscard]] bool Held(const RE::Actor* a_actor);

	// Developer channel `pairs`: ours (pair events heard by role, said by event and by flag, by kind, coalesced, what was
	// not said and why, events kept here) | theirs (heard, played by kind, refused by verdict, failed, took, ended, never
	// took, given up, released, held now, late) | the load order's paired idles. `pairs last`: the last said and played.
	// `pairs list`: each pair held here (a friend's played here, or a copy held as our actor's partner: from=here), with
	// whether each actor is in it now and was ever, and how far apart they are. `pairs holds`: the last of each let go (sawA,
	// sawV: each side seen in it; apartMax: the most they were apart while both were). `pairs find <kill|feed|any>`: the lowest-id paired idle of that kind (a kill
	// move's event holds "KillMove", a feeding's "Feed"), for the rig. `pairs play <idle> <victim> [<attacker>]`: our
	// player (or an NPC this game runs, by its local id) plays that paired idle on the victim (local ids, hex): the rig's
	// set-up, as its injected attacks are.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
