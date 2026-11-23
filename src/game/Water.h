#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

// Actors in water, the game's side (docs/DESIGN.md, "Players": "In water"; the rules are src/game/WaterRules.h, what a
// stand-in or a copy is shown doing src/game/Gait.h): whether a swimmer is under water, as our player's state and the
// states of the NPCs we run say it, and the developer channel's `swim`, which the rig reads. Main thread, but Heard.
namespace Water
{
	// A swimming actor with its head under water (the engine's flag, or enough of it under: WaterRules::Under). False for
	// one not swimming.
	[[nodiscard]] bool Under(RE::Actor* a_actor, bool a_swimming);

	// The water's height at a place, as the engine has it there (a river's or a lake's placed water, or the cell's own);
	// none where it has none or can't say (a cell not loaded).
	[[nodiscard]] std::optional<float> HeightAt(const RE::NiPoint3& a_at, RE::TESObjectCELL* a_cell);

	// Each tick in a session: our player's swimming and being under water, for `swim` (when each last changed).
	void Sample();

	// An event our player's graph took (PlayerActions' hook, any thread): the last ones are kept for `swim`, which says
	// those heard as our player went into or out of the water or under it.
	void Heard(std::string_view a_event, std::chrono::steady_clock::time_point a_at);

	// Developer channel `swim`: ours (swimming as the actor state, the engine's flags and the character controller have
	// it, under water, how deep, how often each changed, and the graph events heard at each change), then each
	// stand-in's (RemotePlayers::Swim) and the copies' (NpcSync::Swim). `swim find [radius]`: the nearest point to our
	// player where the water is deep enough to swim in (WaterRules::FindSpot), for the rig to walk to.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
