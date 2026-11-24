#pragma once

#include "game/HookTarget.h"

#include <Windows.h>

// Puts a_hook in a vtable's slot, whatever function is there: the game's own, or another plugin's hook of the same slot
// (a combat or animation plugin hooks the same graph notify and the same update), which is chained to as that plugin
// chains to the game's. So the hook works whatever the load order. Refused only when the slot points at no code (junk,
// freed or guarded memory) or holds our hook already (src/game/HookTarget.h). a_original holds the function the slot
// held before the slot changes (a job thread may call the hook at once); false: not hooked, a_original left as it was.
// a_who names the system in the log ("Players", "NPCs").
namespace VtableHook
{
	template <class Fn>
	bool Slot(std::uintptr_t a_vtable, std::size_t a_index, Fn a_hook, Fn& a_original, std::string_view a_who, std::string_view a_what)
	{
		const auto slot = a_vtable + sizeof(void*) * a_index;
		const auto current = *reinterpret_cast<const std::uintptr_t*>(slot);
		const auto hook = reinterpret_cast<std::uintptr_t>(a_hook);
		const auto image = reinterpret_cast<std::uintptr_t>(REX::W32::GetModuleHandleW(nullptr));
		MEMORY_BASIC_INFORMATION info{};
		const bool queried = current != 0 && ::VirtualQuery(reinterpret_cast<void*>(current), &info, sizeof(info)) != 0;
		const bool inGame = queried && reinterpret_cast<std::uintptr_t>(info.AllocationBase) == image;
		const auto verdict = HookTarget::Judge(current, hook, queried, queried ? info.State : 0, queried ? info.Protect : 0, inGame);
		switch (verdict) {
		case HookTarget::Verdict::kOurs:
			logs::warn("{}: the {} slot holds our hook already: left as it is", a_who, a_what);
			return false;
		case HookTarget::Verdict::kNotCode:
			logs::error("{}: the {} slot holds {:#x}, which is not code (state {:#x}, protection {:#x}): not hooked", a_who, a_what, current, queried ? info.State : 0,
				queried ? info.Protect : 0);
			return false;
		case HookTarget::Verdict::kGame:
		case HookTarget::Verdict::kChained:
			break;
		}
		a_original = reinterpret_cast<Fn>(current);
		REL::safe_write(slot, hook);
		if (verdict == HookTarget::Verdict::kGame) {
			logs::info("{}: hooked the {} (game+{:X})", a_who, a_what, current - image);
			return true;
		}
		// Another module's hook: named by its file and the offset in it, so a report says which plugin we chain to.
		char        path[MAX_PATH]{};
		const auto  module = static_cast<HMODULE>(info.AllocationBase);
		const auto  length = ::GetModuleFileNameA(module, path, MAX_PATH);
		const auto  name = length > 0 ? HookTarget::FileName(std::string_view{ path, length }) : std::string_view{ "an unnamed module" };
		logs::info("{}: hooked the {}, chained to another plugin's hook of it ({}+{:X})", a_who, a_what, name, current - reinterpret_cast<std::uintptr_t>(info.AllocationBase));
		return true;
	}
}
