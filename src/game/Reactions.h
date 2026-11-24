#pragma once

#include "Protocol.h"
#include "game/NpcRules.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RE
{
	class Actor;
}

// Melee hit reactions in every game, the game's side (docs/DESIGN.md, "Combat"; the rules are src/game/ReactionRules.h).
// - A copy's stagger, recoil or blocked hit that our player's blow or block caused here is sent to the NPC's runner
//   (kNpcReactions), which plays it on its NPC; the runner's NPC sending it back is not played again on the copy (Echo).
// - A stagger played again on a stand-in or a copy (a friend's own, a runner's NPC's) is set to the size and way it had
//   where it happened (StaggerOf where it is heard, Play where it is played).
//
// Main thread, but HeardGraph, StaggerOf and Play (the graph notify hooks and the actor update hook, on the game's job
// threads) and the hit sink.
namespace Reactions
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// Once, at kDataLoaded: our player's hits (either way) heard.
	void Install();
	// Welcomed into a session as a_ourId.
	void Started(std::uint32_t a_ourId);
	// Each tick in a session: the copies' reactions our player caused, sent to their runners.
	void Frame(const SendFn& a_send);
	// A friend's reactions on NPCs we run (the server names the friend): each played on its NPC.
	void Heard(const Protocol::NpcReactions& a_msg);
	// The session ended, or a load: nothing waits.
	void Clear();

	// From the Character graph notify hook (any thread): a_actor's graph took a_event. A reaction on an actor that is not
	// being played by us is kept to be judged (Frame).
	void HeardGraph(RE::Actor* a_actor, std::string_view a_event);
	// A stagger's size and way as a_actor's graph holds them now, when a_event is a stagger (any thread).
	[[nodiscard]] std::optional<Protocol::Stagger> StaggerOf(RE::Actor* a_actor, std::string_view a_event);
	// Plays a_event on a_actor (a stand-in or a copy) as ours to show, a stagger's size and way set first: never heard
	// as a reaction of its own (any thread).
	bool Play(RE::Actor* a_actor, const NpcRules::Event& a_event);
	// NpcSync::Actions (main thread): a_event on the copy of a_key is our own reaction coming back from its runner, not to
	// be played again.
	[[nodiscard]] bool Echo(const Protocol::NpcKey& a_key, std::string_view a_event, std::chrono::steady_clock::time_point a_now);

	// Developer channel `reactions`: ours (heard on copies, judged ours or not, sent by kind, dropped, echoes skipped) and
	// a friend's (came, played by kind, refused, too soon, not ours, not here, busy). For the rig.
	[[nodiscard]] std::string Describe();
}
