#pragma once

#include "Protocol.h"
#include "game/NpcRules.h"

#include <string>
#include <utility>
#include <vector>

// What an actor wears and holds, read and put on, for every actor this game shows for another game: a copy of an NPC
// another game runs (NpcSync) and a friend's stand-in (RemotePlayers), and what our own player holds (LocalPlayer). One
// rule for all of them (NpcRules::PlanGear, CheckGear): each body slot, each hand (a weapon, a staff, a shield, a
// torch, a scroll), the ammunition. Spells in hands only where asked (Spells::kHands): a stand-in's, taught what it
// lacks (it is this game's own and never saved). A copy of an NPC is dressed with Spells::kLeft: a spell in a hand is
// neither put on nor taken off nor judged (its runner's AI casts it; a copy holding one would cast it here when its
// runner's cast events play: docs/DESIGN.md, "Not shared yet"). Main thread.
namespace Gear
{
	// A runtime armor's or weapon's plugin form (the one it was made from: an enchanted or tempered copy), for
	// FormIds::Shareable; 0: none.
	[[nodiscard]] std::uint32_t ArmorTemplateOf(std::uint32_t a_id);
	[[nodiscard]] std::uint32_t WeaponTemplateOf(std::uint32_t a_id);
	[[nodiscard]] std::uint32_t SlotsOf(const RE::TESBoundObject* a_object);
	// Held in both hands: a weapon of a two-handed type (greatsword, battleaxe, warhammer, bow, crossbow), or any weapon
	// or spell whose equip slot is none of the one-hand ones (a ritual spell, a mod's two-handed weapon).
	[[nodiscard]] bool TwoHanded(const RE::TESForm* a_form);
	// A spell (a SpellItem of the spell type: a scroll is an item and goes into a hand as one).
	[[nodiscard]] bool IsSpell(const RE::TESForm* a_form);
	// The object in each hand, a two-handed one in the right alone (NpcRules::OneReportEach).
	[[nodiscard]] std::pair<RE::TESForm*, RE::TESForm*> HeldObjects(RE::Actor* a_actor);

	// What an actor wears and holds now (and, given a_carried, what it carries, the spells it knows of those in
	// a_wanted's hands counted as two each), as NpcRules::PlanGear and CheckGear take it.
	[[nodiscard]] NpcRules::Wearing WearingOf(RE::Actor* a_actor, std::vector<NpcRules::Count>* a_carried, const Protocol::NpcGear* a_wanted = nullptr);

	// Whether spells go in an actor's hands: a stand-in's (kHands) or not (kLeft: a copy of an NPC, as above).
	enum class Spells : bool
	{
		kLeft,
		kHands,
	};

	// What it wears and holds, read back, against the gear it was dressed for (NpcRules::CheckGear). With kLeft, a hand
	// that holds a spell, or should, is not judged.
	[[nodiscard]] NpcRules::GearShown Check(const Protocol::NpcGear& a_gear, RE::Actor* a_actor, Spells a_spells);

	struct Dressed
	{
		std::size_t equipped = 0;
		std::size_t unequipped = 0;
		std::size_t missing = 0;  // what it lacks and may not be given (PlanGear)
		std::size_t skipped = 0;  // named by a form this game has as no item and no spell
	};

	// Takes off and puts on what a_gear says (NpcRules::PlanGear); a_mayAdd: it is this game's own actor (a spawn's copy,
	// a stand-in), given what it lacks. With kLeft a spell in a hand, wanted or held, is counted skipped and left be.
	Dressed Dress(const Protocol::NpcGear& a_gear, bool a_mayAdd, RE::Actor* a_actor, Spells a_spells);

	// What our player holds, as the looks send it (Protocol::PlayerLooks' hands): each hand's object, the ammunition and
	// the voice (the shout or power ready), a runtime form as the plugin form it was made from, or 0 with its unnamed bit.
	struct Hands
	{
		std::uint32_t right = 0;
		std::uint32_t left = 0;
		std::uint32_t ammo = 0;
		std::uint32_t voice = 0;
		std::uint8_t  unnamed = 0;
		bool operator==(const Hands&) const = default;
	};
	[[nodiscard]] Hands HandsOf(RE::Actor* a_actor);
	// The voice an actor has ready (a shout or a power); none: 0.
	[[nodiscard]] RE::TESForm* VoiceOf(RE::Actor* a_actor);
	// Puts a_voice (a shout or a power) in an actor's voice; 0 clears it. A power is taught first when a_mayAdd (a spell
	// an actor is taught is the actor's own). A shout is never taught: an actor that is not the player keeps its shouts
	// in its base (a stand-in's base is a load-order preset, which a save would keep), so it is only readied. False when
	// it is neither, or (not a_mayAdd) one the actor does not know.
	bool SetVoice(RE::Actor* a_actor, std::uint32_t a_voice, bool a_mayAdd);
	// How many of an item an actor carries.
	[[nodiscard]] std::int32_t CountOf(RE::Actor* a_actor, RE::FormID a_id);
	// The shouts an actor's base keeps (its spell list's): a stand-in's never changes (the rig compares it).
	[[nodiscard]] std::uint32_t BaseShouts(RE::Actor* a_actor);
	// Puts one thing the actor carries or knows in a hand (a weapon, a staff, a spell, a scroll, a torch, a shield) or the
	// quiver (a_hand kNone): the rig's set-up of our player (dev `actions equip`). Empty when asked of the game; otherwise
	// why not (not carried, not known, not that kind). Whether it took is read back (HandsOf).
	[[nodiscard]] std::string EquipOne(RE::Actor* a_actor, RE::TESForm* a_form, NpcRules::Hand a_hand);
}
