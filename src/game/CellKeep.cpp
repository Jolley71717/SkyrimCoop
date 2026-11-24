#include "game/CellKeep.h"

#include "game/Furniture.h"
#include "game/Pairs.h"
#include "game/Riding.h"
#include "game/SpaceRules.h"

namespace CellKeep
{
	namespace
	{
		// The engine's move of a reference into a cell at a spot (what the console's and Papyrus' MoveTo end in;
		// CommonLibSSE-NG keeps it private, so it is called through the same address): no target, so the reference keeps
		// its own spot and angle.
		void MoveInto(RE::TESObjectREFR* a_ref, RE::TESObjectCELL* a_cell, RE::TESWorldSpace* a_world, const RE::NiPoint3& a_position, const RE::NiPoint3& a_angle)
		{
			using func_t = void(RE::TESObjectREFR*, const RE::ObjectRefHandle&, RE::TESObjectCELL*, RE::TESWorldSpace*, const RE::NiPoint3&, const RE::NiPoint3&);
			static REL::Relocation<func_t> func{ RELOCATION_ID(56227, 56626) };
			func(a_ref, RE::ObjectRefHandle{}, a_cell, a_world, a_position, a_angle);
		}

		std::optional<Moved> Recell(RE::Actor* a_actor)
		{
			const auto parent = a_actor->GetParentCell();
			const auto tes = RE::TES::GetSingleton();
			if (!parent || !tes) {
				return std::nullopt;
			}
			const auto at = a_actor->GetPosition();
			const auto under = parent->IsInteriorCell() ? nullptr : tes->GetCell(at);
			if (!SpaceRules::Recell(parent->IsInteriorCell(), parent->GetFormID(), under ? under->GetFormID() : 0, under && under->IsAttached())) {
				return std::nullopt;
			}
			MoveInto(a_actor, under, a_actor->GetWorldspace(), at, a_actor->GetAngle());
			return Moved{ .actor = a_actor->GetFormID(), .from = parent->GetFormID(), .to = under->GetFormID() };
		}
	}

	std::optional<Moved> Keep(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return std::nullopt;
		}
		RE::NiPointer<RE::Actor> mount;
		const bool               seated = Riding::Seated(a_actor) || a_actor->IsOnMount();
		const bool               hasMount = seated && a_actor->GetMount(mount) && mount;
		switch (SpaceRules::Whom(seated, hasMount, Furniture::Held(a_actor), Pairs::Held(a_actor))) {
		case SpaceRules::Mover::kSelf:
			return Recell(a_actor);
		case SpaceRules::Mover::kMount:
			if (Furniture::Held(mount.get()) || Pairs::Held(mount.get())) {
				return std::nullopt;
			}
			if (auto moved = Recell(mount.get())) {
				moved->mount = true;
				return moved;
			}
			return std::nullopt;
		case SpaceRules::Mover::kNone:
			return std::nullopt;
		}
		return std::nullopt;
	}
}
