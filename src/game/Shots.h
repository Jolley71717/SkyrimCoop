#pragma once

#include "Protocol.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Projectiles, the game's side (docs/DESIGN.md, "Projectiles"; the rules are src/game/ShotRules.h). Every projectile an
// actor launches is updated through the update of its class (arrow, missile, flame, beam, cone, lobber, barrier), which
// this hooks:
// - At a projectile's first update, one of our player's or of an NPC this game runs is said to the others (kShots): its
//   record, its sources (weapon, ammunition, spell, enchantment, poison), where it left from, its aim, power and scale.
// - Every other game launches the same from the actor that shows its shooter there (a friend's stand-in, the copy of
//   the NPC), at the moment that actor is drawn at, from the shooter's origin and on its aim (turned back to it when the
//   engine aims it elsewhere). What it hits there is that game's business: a stand-in deals no harm (its attack damage
//   and its spells are refused, RemotePlayers.cpp), a copy's deals what the local game runs (as its blows do).
// - A projectile a stand-in's or a moved copy's own graph launches (its replayed release or cast) is ended before it
//   moves: the wire's launch is the one that flies.
// - The launches of this game are ended once their flight is over (an arrow that stuck after a short while, a rune
//   after its life), and all of them before a save: no save holds one.
//
// Main thread, but the update hook (any thread).
namespace Shots
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// At kDataLoaded: hooks the update of the seven projectile classes. False when none went in (the log says which).
	bool Install();

	// Welcomed into a session as a_ourId.
	void Started(std::uint32_t a_ourId);
	// Once a tick in a session after the welcome, after NpcSync::Frame: ours out, theirs launched, natives ended, our
	// launches ended when their flight is over.
	void Frame(const SendFn& a_send);
	// A friend's shots (their game's, or of the NPCs it runs).
	void Heard(const Protocol::Shots& a_msg);
	// The session ended: every launch of ours is ended, and nothing is known.
	void Clear();
	// A load or a new game takes the world: no projectile of the old one is held.
	void ForgetWorld();
	// SKSE's kSaveGame, before the game writes the save: every launch of ours is ended.
	void BeforeSave();

	// Developer channel `shots`: ours (hooked=N/7, the projectiles seen and said by kind, coalesced, dropped, not ours),
	// theirs (heard, launched by kind, late, no shooter, too far, unknown records, full, failed launches), the natives
	// ended, the launches ended and how (stuck, old, at a save), the ones flying now, the aim (turned back, the worst
	// error before and after), per player. `shots find <missile|flame|beam|lobber|cone|crossbow>`: a spell of the load
	// order whose projectile is that kind (aimed, fire and forget or held), or a crossbow and a bolt, for the rig.
	// `shots last`: the last shot said and the last launched.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
