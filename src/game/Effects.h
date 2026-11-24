#pragma once

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Our player's magic on NPCs other games run, dealt in their runners' games, and Slow Time shared (src/game/EffectRules.h;
// docs/DESIGN.md, "Magic on NPCs").
//
// The effect hook (RemotePlayers' MagicTarget::AddTarget hooks, any thread) asks Forwards of every effect our player puts
// on a copy of an NPC another game runs: one its runner's game deals (EffectRules::OnCopy) is noted and refused here, and
// the next tick sends it to that game (kNpcEffects; the server routes it by its book). That game puts it on its real NPC
// (Heard), our player's stand-in its caster, through the same AddTarget, which the hook lets through while Effects
// applies it (Applying). Our player's Slow Time on themselves (NoteSelf) goes to every other game (kSlowTimes), which puts
// it on its own player when that player is near our stand-in there.
namespace Effects
{
	using SendFn = std::function<void(std::vector<std::uint8_t>, bool)>;

	// Welcomed into a session as a_ourId.
	void Started(std::uint32_t a_ourId);
	// Once a tick in a session after the welcome: what the hook noted, sent.
	void Frame(const SendFn& a_send);
	// A friend's effects on NPCs this game runs: put on each.
	void Heard(const Protocol::NpcEffects& a_msg);
	// A friend's Slow Time: put on our player when near their stand-in.
	void Heard(const Protocol::SlowTime& a_msg);
	// The session ended: nothing noted or known.
	void Clear();

	// For the effect hook (any thread): whether an effect a_caster puts on a_target here is held back, its runner's game
	// to deal: a_caster is our player and a_target a copy of an NPC another game runs, and the effect is of a kind that
	// game deals (EffectRules::Route::kRunner; one of kBoth is noted but taken here too).
	// a_data: what the hook was given.
	[[nodiscard]] bool Forwards(RE::Actor* a_caster, RE::Actor* a_target, const RE::MagicTarget::AddTargetData& a_data, std::int32_t a_archetype);
	// For the effect hook (any thread): an effect our player puts on themselves; Slow Time is noted for the others.
	void NoteSelf(RE::Actor* a_caster, const RE::MagicTarget::AddTargetData& a_data, std::int32_t a_archetype);
	// Whether this thread is putting on an effect a friend's game sent (the effect hook lets it through).
	[[nodiscard]] bool Applying();

	// Developer channel `effects`: ours held back and sent by route and kind, dropped and why; theirs put on, refused and
	// why; Slow Times sent, shared and not (too far, elsewhere). `effects last`: the last sent and put on.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
