#pragma once

#include <string>
#include <string_view>

// The world leaves a friend's stand-in alone, the game's side (docs/DESIGN.md, "Players"; the rules are
// src/game/PeaceRules.h). A stand-in is a plain NPC to the engine, so every actor here (guards and townsfolk, creatures,
// dragons, horses, followers, summons: every Character) could fight it, talk to it, gather to watch it or flee from it,
// and our player's swing or spell on it would be an assault on a person of this game. None of that is the friend:
//   - the engine's asking an actor's AI about a stand-in is turned away where it asks, in the Character class's own
//     functions (CheckValidTarget: a combat target; InitiateDialogue: a conversation; InitiateSpectator: a crowd;
//     InitiateFlee: a threat), and a stand-in asks none of it itself;
//   - an actor seen with a stand-in as its combat target all the same (a console's startcombat, a path the hooks do not
//     see) is told to stop fighting, twice a second at most once per actor each 2 s (PeaceRules::CalmBook);
//   - weapons and spells pass through a stand-in (the engine's ghost flag, kept on after each of its updates), and every
//     effect anyone puts on it is refused (RemotePlayers' magic target hooks, PeaceRules::JudgeMagic): no hit, no
//     assault, no crime, no fight;
//   - its own harmless blows already deal nothing (RemotePlayers); any fight they would start against it is one of the
//     above, turned away.
// Not run in a game yet; the rig step `peace` (tools/rig/steps/peace.sh) is written for it.
//
// Main thread, but AfterUpdate (the actor update hook) and the hooks (the engine's AI and combat jobs).
namespace StandInPeace
{
	// At kDataLoaded, after RemotePlayers::Install: hooks the four Character functions. Says in the log what it could not
	// (the sweep still runs then).
	void Install();
	// In a stand-in's own update, after the engine's (RemotePlayers' update hook, on the thread that updates it): the
	// ghost flag kept on. Any thread.
	void AfterUpdate(RE::Actor* a_standIn);
	// Once a tick in a session, after RemotePlayers::Frame: the sweep of actors fighting a stand-in (PeaceRules::SWEEP_EVERY).
	void Frame();
	// The session ended: the book of actors told to stop is emptied.
	void Clear();
	// Developer channel `peace`: whether each hook is in and how often it was asked and turned away (by kind: target,
	// talk, watch, flee), the sweep's counts (calmed=: actors told to stop fighting a stand-in; givenUp=; targeting=: those
	// seen fighting one at the last sweep; the book's size), the last actor turned away or told and about which stand-in,
	// the ghost flag kept (ghostSet=: times set again after an update; noProcess=), our player's bounty in every crime
	// faction together (bounty=), then each stand-in (RemotePlayers::Peace). `peace targeting`: every actor in high process
	// whose combat target is a stand-in, now. `peace find <base> [radius]`: the nearest actor of that base to our player
	// (never a stand-in), its distance, whether it is dead or fighting, and its combat target (the rig finds the creature it
	// placed).
	[[nodiscard]] std::string Command(std::string_view a_args);
}
