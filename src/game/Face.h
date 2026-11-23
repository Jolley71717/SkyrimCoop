#pragma once

#include "Protocol.h"
#include "game/FaceRules.h"

#include <functional>
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
	// that holds it; a new one is written into a copy of that preset no actor stands on (a_inUse: a stand-in here or on
	// its way out has it as its base; FaceRules::CopyBook), and only when there is none is a new copy made, unless this
	// game run made all it may (full) or the game made none (base nullptr). Each copy is remembered with its preset
	// (PresetOf) until the next load.
	struct Copy
	{
		RE::TESNPC* base = nullptr;
		bool        made = false;       // made now
		bool        rewritten = false;  // an unused copy given this face now
		bool        full = false;
	};
	[[nodiscard]] Copy CopyFor(std::uint32_t a_player, RE::TESNPC* a_preset, const Protocol::PlayerFace& a_face, float a_weight, const std::function<bool(const RE::TESNPC*)>& a_inUse);
	[[nodiscard]] bool Full();

	// The preset a copy was made from (nullptr: a_base is no copy of ours since the last load).
	[[nodiscard]] RE::TESNPC* PresetOf(const RE::TESBoundObject* a_base);

	// Before a load: no copy made before it is placed again (FaceRules::CopyBook). The forms stay (the game frees none).
	void ForgetCopies();

	// Whether an actor's third-person 3D has a face node (its head was built).
	[[nodiscard]] bool HasHead(RE::Actor* a_actor);

	// Our own player's head, for FaceRules::Watch: its third-person 3D loaded, a face node on it, and whether it can be
	// judged now (judged: in the race it last had a face node in, so not a werewolf's or a vampire lord's form, not in
	// first person, no face editor open). Each read notes the race when a face node is seen.
	struct Ours
	{
		bool loaded = false;
		bool head = false;
		bool judged = false;
	};
	[[nodiscard]] Ours OurHead();

	// A face editor is open: the race menu (which face editor mods open too) or any menu that pauses the game. Our face is
	// not sent meanwhile (FaceRules::Settle), and our head not judged.
	[[nodiscard]] bool Editing();

	// What a stand-in's built head shows (FaceRules::Head3D), read from its 3D: a node for each of a_copy's head parts
	// under its face node, none for a_preset's parts the copy left out, its hair tinted a_hairRgb (0: the copy's colour).
	[[nodiscard]] FaceRules::Head3D Shown3D(RE::Actor* a_actor, const RE::TESNPC* a_copy, const RE::TESNPC* a_preset, std::uint32_t a_hairRgb);

	// Runtime character-creation-preset NPCs in the game now (forms made as it ran, with the preset flag), and of those,
	// the ones this game run did not make itself: after a load, a copy a save brought back (none should: a stand-in has its
	// preset back before any save).
	struct RuntimePresets
	{
		std::size_t all = 0;
		std::size_t notOurs = 0;
	};
	[[nodiscard]] RuntimePresets CountRuntimePresets();

	// Its 3D built again on the engine's next update of it (what a face editor asks after changing an NPC's face).
	void Rebuild(RE::Actor* a_actor);
	// Our player's head asked back, once: a stand-in's head seemed to take it. Never while our head can't be judged
	// (OurHead: a beast form, first person, a face editor): false, and nothing done.
	bool RepairOurs();

	// What this session has of ours (Session, after each send): for the developer channel.
	void Sent(const Protocol::PlayerFace& a_face);

	// Developer channel `faces` (ours): read=, our face's hash and its pieces (a_sessionHash: its hash with its forms in
	// the session's numbering, which every game computes alike whatever its load order), what went to the session. `faces nudge`
	// changes our player's face for the rig (a face slider and the hair colour, the head built again), `faces nudge undo`
	// puts it back; each says the new hash.
	[[nodiscard]] std::string Describe(const std::function<std::uint32_t(const Protocol::PlayerFace&)>& a_sessionHash);
	[[nodiscard]] std::string Nudge(std::string_view a_args);

}
