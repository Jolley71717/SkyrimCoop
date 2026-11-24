#include "game/Gear.h"

#include "FormIds.h"

// Windows' headers name GetObjectA GetObject; BGSDefaultObjectManager's GetObject is meant here.
#ifdef GetObject
#	undef GetObject
#endif

namespace Gear
{
	namespace
	{
		const RE::BGSEquipSlot* SlotObject(RE::DEFAULT_OBJECTS::DEFAULT_OBJECT a_which)
		{
			const auto defaults = RE::BGSDefaultObjectManager::GetSingleton();
			return defaults ? defaults->GetObject<RE::BGSEquipSlot>(a_which) : nullptr;
		}

		// The hand slot to equip into: a one-handed weapon or a one-handed spell goes in the hand named; a two-handed one,
		// a torch, a shield or a scroll where the game puts it (a two-handed object's own slot is both hands: a one-hand
		// slot would be the wrong one).
		const RE::BGSEquipSlot* HandSlot(NpcRules::Hand a_hand, const RE::TESForm* a_form)
		{
			if (a_hand == NpcRules::Hand::kNone || !(a_form->Is(RE::FormType::Weapon) || IsSpell(a_form)) || TwoHanded(a_form)) {
				return nullptr;
			}
			return SlotObject(a_hand == NpcRules::Hand::kLeft ? RE::DEFAULT_OBJECTS::kLeftHandEquip : RE::DEFAULT_OBJECTS::kRightHandEquip);
		}

		// The form a plugin form id names in this game; none for 0 or a runtime id (another game's, or nothing here).
		RE::TESForm* FormOf(std::uint32_t a_id)
		{
			return a_id != 0 && !FormIds::IsRuntime(a_id) ? RE::TESForm::LookupByID(a_id) : nullptr;
		}

		// A spell by its id in this game, a runtime one included (a copy may hold one its own game made).
		bool IsSpellId(std::uint32_t a_id)
		{
			return a_id != 0 && IsSpell(RE::TESForm::LookupByID(a_id));
		}

		// A runtime form named as the plugin form it was made from, or 0 with its bit set in a_unnamed.
		std::uint32_t Named(const RE::TESForm* a_form, std::uint8_t a_bit, std::uint8_t& a_unnamed)
		{
			if (!a_form) {
				return 0;
			}
			const auto id = a_form->GetFormID();
			const auto shared = FormIds::Shareable(id, a_form->Is(RE::FormType::Armor) ? ArmorTemplateOf : WeaponTemplateOf);
			if (!shared) {
				a_unnamed = static_cast<std::uint8_t>(a_unnamed | a_bit);
				return 0;
			}
			return *shared;
		}
	}

	std::uint32_t ArmorTemplateOf(std::uint32_t a_id)
	{
		const auto armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(a_id);
		return armor && armor->templateArmor ? armor->templateArmor->GetFormID() : 0;
	}

	std::uint32_t WeaponTemplateOf(std::uint32_t a_id)
	{
		const auto weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_id);
		return weapon && weapon->templateWeapon ? weapon->templateWeapon->GetFormID() : 0;
	}

	std::uint32_t SlotsOf(const RE::TESBoundObject* a_object)
	{
		const auto armor = a_object ? a_object->As<RE::TESObjectARMO>() : nullptr;
		return armor ? armor->GetSlotMask().underlying() : 0;
	}

	bool TwoHanded(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return false;
		}
		if (const auto weapon = a_form->As<RE::TESObjectWEAP>();
			weapon && (weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow())) {
			return true;
		}
		const auto type = a_form->As<RE::BGSEquipType>();
		const auto slot = type ? type->GetEquipSlot() : nullptr;
		const auto defaults = RE::BGSDefaultObjectManager::GetSingleton();
		if (!slot || !defaults) {
			return false;
		}
		for (const auto one : { RE::DEFAULT_OBJECTS::kLeftHandEquip, RE::DEFAULT_OBJECTS::kRightHandEquip, RE::DEFAULT_OBJECTS::kEitherHandEquip,
				 RE::DEFAULT_OBJECTS::kVoiceEquip, RE::DEFAULT_OBJECTS::kPotionEquip }) {
			if (defaults->GetObject<RE::BGSEquipSlot>(one) == slot) {
				return false;
			}
		}
		return true;
	}

	bool IsSpell(const RE::TESForm* a_form)
	{
		return a_form && a_form->GetFormType() == RE::FormType::Spell;
	}

	std::pair<RE::TESForm*, RE::TESForm*> HeldObjects(RE::Actor* a_actor)
	{
		const auto right = a_actor->GetEquippedObject(false);
		const auto left = a_actor->GetEquippedObject(true);
		const auto idOf = [](const RE::TESForm* a_form) { return a_form ? a_form->GetFormID() : 0u; };
		const auto hands = NpcRules::OneReportEach(idOf(right), idOf(left), TwoHanded(right), TwoHanded(left));
		const auto pick = [&](std::uint32_t a_id) -> RE::TESForm* {
			return a_id == 0 ? nullptr : a_id == idOf(right) ? right : left;
		};
		return { pick(hands.right), pick(hands.left) };
	}

	NpcRules::Wearing WearingOf(RE::Actor* a_actor, std::vector<NpcRules::Count>* a_carried, const Protocol::NpcGear* a_wanted)
	{
		NpcRules::Wearing now;
		for (const auto& [item, data] : a_actor->GetInventory()) {
			const auto& [count, entry] = data;
			if (count <= 0) {
				continue;
			}
			if (a_carried) {
				a_carried->push_back({ item->GetFormID(), count });
			}
			if (entry && entry->IsWorn() && item->IsArmor()) {
				now.armor.push_back({ item->GetFormID(), FormIds::Shareable(item->GetFormID(), ArmorTemplateOf).value_or(0), SlotsOf(item) });
			}
		}
		// A spell is in no inventory: one the actor knows counts as carried twice (both hands may hold it).
		if (a_carried && a_wanted) {
			for (const auto id : { a_wanted->right, a_wanted->left }) {
				const auto spell = id != 0 && !FormIds::IsRuntime(id) ? RE::TESForm::LookupByID<RE::SpellItem>(id) : nullptr;
				if (spell && IsSpell(spell) && a_actor->HasSpell(spell) &&
					std::ranges::none_of(*a_carried, [&](const NpcRules::Count& a_c) { return a_c.id == id; })) {
					a_carried->push_back({ id, 2 });
				}
			}
		}
		const auto idOf = [](const RE::TESForm* a_form) { return a_form ? a_form->GetFormID() : 0u; };
		const auto [right, left] = HeldObjects(a_actor);
		now.right = idOf(right);
		now.left = idOf(left);
		now.ammo = idOf(a_actor->GetCurrentAmmo());
		return now;
	}

	NpcRules::GearShown Check(const Protocol::NpcGear& a_gear, RE::Actor* a_actor, Spells a_spells)
	{
		const auto now = WearingOf(a_actor, nullptr);
		if (a_spells == Spells::kHands) {
			return NpcRules::CheckGear(a_gear, now);
		}
		// A hand with a spell in it, wanted or held, is not judged (Dress leaves it be).
		auto judged = a_gear;
		if (IsSpellId(judged.right) || IsSpellId(now.right)) {
			judged.unnamed = static_cast<std::uint8_t>(judged.unnamed | Protocol::kRightUnnamed);
		}
		if (IsSpellId(judged.left) || IsSpellId(now.left)) {
			judged.unnamed = static_cast<std::uint8_t>(judged.unnamed | Protocol::kLeftUnnamed);
		}
		return NpcRules::CheckGear(judged, now);
	}

	Dressed Dress(const Protocol::NpcGear& a_gear, bool a_mayAdd, RE::Actor* a_actor, Spells a_spells)
	{
		Dressed    out;
		const auto manager = RE::ActorEquipManager::GetSingleton();
		if (!manager) {
			return out;
		}
		const bool spells = a_spells == Spells::kHands;
		// Without spells: a spell in the runner's NPC's hand is never in an inventory, so it is not looked for (a
		// load-order copy would count it missing): the hand is planned as empty and the spell counted skipped.
		auto wanted = a_gear;
		if (!spells) {
			for (auto* hand : { &wanted.right, &wanted.left }) {
				if (IsSpellId(*hand)) {
					*hand = 0;
					++out.skipped;
				}
			}
		}
		std::vector<NpcRules::Count> carried;
		const auto                   now = WearingOf(a_actor, &carried, spells ? &wanted : nullptr);
		const auto                   plan = NpcRules::PlanGear(wanted, now, carried, a_mayAdd);
		// What is put on is held on against the actor's own AI (the equip's "force", Papyrus' abPreventRemoval) only on an
		// actor this game made as it ran (a stand-in, a spawn's copy), which is never saved: on a load-order NPC's copy that
		// lock would stay in the friend's save after the session (a follower who will not take other armor).
		const bool                   holdOn = FormIds::IsRuntime(a_actor->GetFormID());
		const auto                   formOf = [&](std::uint32_t a_id) -> RE::TESForm* {
			// Any id of this game, a runtime one included: what the plan takes off and puts on from what the actor has is
			// named by this game's own ids (an enchanted or tempered copy made here is one); another game's runtime ids
			// never reach a plan (FormTranslate drops them, PlanGear keeps an unnamed hand as it is).
			const auto form = a_id != 0 ? RE::TESForm::LookupByID(a_id) : nullptr;
			return spells || !IsSpell(form) ? form : nullptr;  // without spells, one held is neither taken off nor put on
		};
		for (const auto& item : plan.unequip) {
			const auto form = formOf(item.id);
			if (const auto spell = IsSpell(form) ? form->As<RE::SpellItem>() : nullptr) {
				a_actor->DeselectSpell(spell);
				++out.unequipped;
			} else if (const auto bound = form ? form->As<RE::TESBoundObject>() : nullptr) {
				manager->UnequipObject(a_actor, bound, nullptr, 1, HandSlot(item.hand, bound), false, true, false, true);
				++out.unequipped;
			} else {
				++out.skipped;
			}
		}
		for (const auto& add : plan.add) {
			const auto form = formOf(add.id);
			if (const auto spell = IsSpell(form) ? form->As<RE::SpellItem>() : nullptr) {
				a_actor->AddSpell(spell);  // taught once, whatever the count (a spell in both hands is one spell)
			} else if (const auto bound = form ? form->As<RE::TESBoundObject>() : nullptr; bound && add.count > 0) {
				a_actor->AddObjectToContainer(bound, nullptr, add.count, nullptr);
			}
		}
		for (const auto& item : plan.equip) {
			const auto form = formOf(item.id);
			if (const auto spell = IsSpell(form) ? form->As<RE::SpellItem>() : nullptr) {
				manager->EquipSpell(a_actor, spell, TwoHanded(spell) ? nullptr : HandSlot(item.hand == NpcRules::Hand::kNone ? NpcRules::Hand::kRight : item.hand, spell));
				++out.equipped;
			} else if (const auto bound = form ? form->As<RE::TESBoundObject>() : nullptr) {
				manager->EquipObject(a_actor, bound, nullptr, 1, HandSlot(item.hand, bound), false, holdOn, false, true);
				++out.equipped;
			} else {
				++out.skipped;
			}
		}
		out.missing = plan.missing;
		return out;
	}

	RE::TESForm* VoiceOf(RE::Actor* a_actor)
	{
		return a_actor ? a_actor->GetActorRuntimeData().selectedPower : nullptr;
	}

	Hands HandsOf(RE::Actor* a_actor)
	{
		Hands out;
		if (!a_actor) {
			return out;
		}
		const auto [right, left] = HeldObjects(a_actor);
		out.right = Named(right, Protocol::kRightUnnamed, out.unnamed);
		out.left = Named(left, Protocol::kLeftUnnamed, out.unnamed);
		out.ammo = Named(a_actor->GetCurrentAmmo(), Protocol::kAmmoUnnamed, out.unnamed);
		out.voice = Named(VoiceOf(a_actor), Protocol::kVoiceUnnamed, out.unnamed);
		return out;
	}

	std::int32_t CountOf(RE::Actor* a_actor, RE::FormID a_id)
	{
		std::int32_t n = 0;
		if (!a_actor) {
			return n;
		}
		for (const auto& [item, data] : a_actor->GetInventory([&](RE::TESBoundObject& a_object) { return a_object.GetFormID() == a_id; })) {
			n += data.first;
		}
		return n;
	}

	std::uint32_t BaseShouts(RE::Actor* a_actor)
	{
		const auto base = a_actor ? a_actor->GetActorBase() : nullptr;
		const auto list = base ? base->GetSpellList() : nullptr;
		return list ? list->numShouts : 0;
	}

	std::string EquipOne(RE::Actor* a_actor, RE::TESForm* a_form, NpcRules::Hand a_hand)
	{
		const auto manager = RE::ActorEquipManager::GetSingleton();
		if (!manager || !a_actor || !a_form) {
			return "no equip manager, actor or form";
		}
		if (const auto spell = IsSpell(a_form) ? a_form->As<RE::SpellItem>() : nullptr) {
			if (a_hand == NpcRules::Hand::kNone) {
				return "a spell goes in a hand";
			}
			if (!a_actor->HasSpell(spell)) {
				return "a spell it does not know";
			}
			manager->EquipSpell(a_actor, spell, TwoHanded(spell) ? nullptr : HandSlot(a_hand, spell));
			return {};
		}
		const auto bound = a_form->As<RE::TESBoundObject>();
		if (!bound || !(a_form->Is(RE::FormType::Weapon) || a_form->Is(RE::FormType::Armor) || a_form->Is(RE::FormType::Light) ||
						  a_form->Is(RE::FormType::Scroll) || a_form->Is(RE::FormType::Ammo))) {
			return "not a weapon, a shield, a torch, a scroll, ammunition or a spell";
		}
		if ((a_hand == NpcRules::Hand::kNone) != a_form->Is(RE::FormType::Ammo)) {
			return a_hand == NpcRules::Hand::kNone ? "only ammunition goes in the quiver" : "ammunition goes in the quiver";
		}
		if (CountOf(a_actor, bound->GetFormID()) <= 0) {
			return "not carried";
		}
		manager->EquipObject(a_actor, bound, nullptr, 1, HandSlot(a_hand, bound), false, true, false, true);
		return {};
	}

	bool SetVoice(RE::Actor* a_actor, std::uint32_t a_voice, bool a_mayAdd)
	{
		const auto manager = RE::ActorEquipManager::GetSingleton();
		if (!manager || !a_actor) {
			return false;
		}
		if (a_voice == 0) {
			a_actor->GetActorRuntimeData().selectedPower = nullptr;  // none ready, as its friend has
			return true;
		}
		const auto form = FormOf(a_voice);
		if (!form) {
			return false;
		}
		if (const auto shout = form->As<RE::TESShout>()) {
			// Never Actor::AddShout here: an actor that is not the player keeps its shouts in its base (TESNPC's spell
			// list), and a stand-in's base is a load-order preset that a save would keep changed. Readied alone.
			if (!a_mayAdd && !a_actor->HasShout(shout)) {
				return false;
			}
			manager->EquipShout(a_actor, shout);
			return true;
		}
		if (const auto power = IsSpell(form) ? form->As<RE::SpellItem>() : nullptr) {
			if (a_mayAdd) {
				a_actor->AddSpell(power);  // the actor's own (its added spells), not its base's
			} else if (!a_actor->HasSpell(power)) {
				return false;
			}
			manager->EquipSpell(a_actor, power, SlotObject(RE::DEFAULT_OBJECTS::kVoiceEquip));
			return true;
		}
		return false;
	}
}
