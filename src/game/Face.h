#pragma once

#include "Protocol.h"

#include <optional>
#include <string>
#include <string_view>

// Faces (docs/DESIGN.md, "Players": "Its face"): our player's face as it goes to the others, and a friend's put on the
// copy of a preset their stand-in stands on. The rules free of the game are in src/game/FaceRules.h. Main thread.
namespace Face
{
	// Our player's face: its head parts, hair colour, complexion, skin, height, face sliders, tint layers (the player's
	// own tint masks, each named by its race's mask) and skin tone. None while there is no player or race.
	[[nodiscard]] std::optional<Protocol::PlayerFace> Read();

	// What a stand-in's copy of a preset holds, read the same way (its tint layers in place of the player's masks): the
	// developer channel's and the rig's read-back.
	[[nodiscard]] std::optional<Protocol::PlayerFace> ReadBase(const RE::TESNPC* a_npc);

	// The base a_player's stand-in stands on to show a_face: a copy of a_preset (a character-creation preset: its copy
	// keeps the flag, so the engine builds its head at run time from its parts, as it does the player's and the presets')
	// with the face and the body's a_weight put on it. The same friend's same face and weight is placed again on the copy
	// made for it (until the next load); otherwise a new one is made, unless this game run made all it may (full) or the
	// game made none (base nullptr). Each copy is remembered with its preset (PresetOf) until the next load.
	struct Copy
	{
		RE::TESNPC* base = nullptr;
		bool        made = false;  // made now (not one made before)
		bool        full = false;
	};
	[[nodiscard]] Copy CopyFor(std::uint32_t a_player, RE::TESNPC* a_preset, const Protocol::PlayerFace& a_face, float a_weight);
	[[nodiscard]] bool Full();

	// The preset a copy was made from (nullptr: a_base is no copy of ours since the last load).
	[[nodiscard]] RE::TESNPC* PresetOf(const RE::TESBoundObject* a_base);

	// Before a load: no copy made before it is placed again (FaceRules::CopyBook). The forms stay (the game frees none).
	void ForgetCopies();

	// Whether an actor's 3D has a face node (its head was built), and our player's.
	[[nodiscard]] bool HasHead(RE::Actor* a_actor);
	[[nodiscard]] bool OursLoaded();
	[[nodiscard]] bool OursHasHead();

	// Its 3D built again on the engine's next update of it (what a face editor asks after changing an NPC's face).
	void Rebuild(RE::Actor* a_actor);
	// Our player's head asked back, once: a stand-in's head seemed to take it.
	void RepairOurs();

	// What this session has of ours (Session, after each send): for the developer channel.
	void Sent(const Protocol::PlayerFace& a_face);

	// Developer channel `faces` (ours): read=, our face's hash and its pieces, what went to the session. `faces nudge`
	// changes our player's face for the rig (a face slider and the hair colour, the head built again), `faces nudge undo`
	// puts it back; each says the new hash.
	[[nodiscard]] std::string Describe();
	[[nodiscard]] std::string Nudge(std::string_view a_args);

}
